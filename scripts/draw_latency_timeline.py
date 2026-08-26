#!/usr/bin/env python3
"""
Draw a detailed timeline for latency root-cause analysis.
Input: result_dir, start_frame_index, end_frame_index.
X-axis: timestamp normalized to ms (0 = capture time of start_frame_index).
Layers bottom→top: 1=frame capture, 2=encode done, 3=packet send, 4=packet recv, 5=sender recv feedback.
Red point + arrow for target bitrate change timestamps, and lines connecting packet send→recv/feedback.

Usage: python3 scripts/draw_latency_timeline.py <result_dir> <start_frame> <end_frame>
"""
import argparse
import csv
import re
import sys
from datetime import datetime
from pathlib import Path


# Only match our app's timestamp format (YYYY-MM-DD HH:MM:SS.fff...) so x264 lines
# like "x264 [info]: ..." are not mistaken for timestamps when log output is interleaved.
TS_PATTERN = r"(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d+)"


def parse_ts(s: str) -> float:
    return datetime.strptime(s.strip("[]"), "%Y-%m-%d %H:%M:%S.%f").timestamp()


def parse_send_log(path: Path):
    re_capture = re.compile(
        r"\[" + TS_PATTERN + r"\].*\[VideoCaptureAndSend\] Read frame (\d+)"
    )
    re_send_start = re.compile(
        r"\[" + TS_PATTERN + r"\].*\[DataSender\] Sending frame (\d+) .* in (\d+) packets"
    )
    re_send_end = re.compile(
        r"\[" + TS_PATTERN + r"\].*\[DataSender\] Successfully sent frame (\d+) in (\d+) packets"
    )
    re_initial_bitrate = re.compile(
        r"\[" + TS_PATTERN + r"\].*\[Encoder\] Initial bitrate (\d+) kbps"
    )
    re_set_bitrate = re.compile(
        r"\[" + TS_PATTERN + r"\].*\[Encoder\] Set target bitrate to (\d+) kbps"
    )
    re_slice_set_bitrate = re.compile(
        r"\[" + TS_PATTERN + r"\].*\[SlicePacedEncoder\] Target bitrate now (\d+) kbps"
    )
    re_feedback = re.compile(
        r"\[" + TS_PATTERN + r"\].*\[FeedbackHandler\] Received feedback: frame=(\d+) packet=(\d+)"
    )
    re_send_size = re.compile(
        r"\[" + TS_PATTERN + r"\].*\[DataSender\] Sending frame (\d+) (?:fragment )?size=(\d+) bytes"
    )
    re_sent_packet = re.compile(
        r"\[" + TS_PATTERN + r"\].*\[DataSender\] Sent packet (\d+) for frame (\d+)"
    )

    text = path.read_text()
    capture_times = {}
    for m in re_capture.finditer(text):
        ts_str, frame = m.group(1), int(m.group(2))
        capture_times[frame] = parse_ts(ts_str)
    send_start = {}
    send_end = {}
    frame_sizes_bytes = {}
    for m in re_send_start.finditer(text):
        ts_str, frame, npkts = m.group(1), int(m.group(2)), int(m.group(3))
        send_start[frame] = (parse_ts(ts_str), npkts)
    for m in re_send_end.finditer(text):
        ts_str, frame, npkts = m.group(1), int(m.group(2)), int(m.group(3))
        send_end[frame] = parse_ts(ts_str)
    for m in re_send_size.finditer(text):
        # use the size from the \"Sending frame\" line as encoded size
        _, frame, size_bytes = m.group(1), int(m.group(2)), int(m.group(3))
        frame_sizes_bytes[frame] = size_bytes
    packet_send_events = []
    for m in re_sent_packet.finditer(text):
        ts_str, pkt, frame = m.group(1), int(m.group(2)), int(m.group(3))
        packet_send_events.append((parse_ts(ts_str), frame, pkt))
    bitrate_events = []
    for m in re_initial_bitrate.finditer(text):
        ts_str, kbps = m.group(1), int(m.group(2))
        bitrate_events.append((parse_ts(ts_str), kbps))
    for m in re_set_bitrate.finditer(text):
        ts_str, kbps = m.group(1), int(m.group(2))
        bitrate_events.append((parse_ts(ts_str), kbps))
    for m in re_slice_set_bitrate.finditer(text):
        ts_str, kbps = m.group(1), int(m.group(2))
        bitrate_events.append((parse_ts(ts_str), kbps))
    bitrate_events.sort(key=lambda x: x[0])
    feedback_times = []
    for m in re_feedback.finditer(text):
        ts_str, frame, pkt = m.group(1), int(m.group(2)), int(m.group(3))
        feedback_times.append((frame, pkt, parse_ts(ts_str)))
    return capture_times, send_start, send_end, bitrate_events, feedback_times, frame_sizes_bytes, packet_send_events


def parse_recv_log(path: Path):
    re_pkt = re.compile(
        r"\[" + TS_PATTERN + r"\].*\[ReceivedFrameDataHandler\] Received packet (\d+) for frame (\d+)"
    )
    text = path.read_text()
    recv_times = []
    for m in re_pkt.finditer(text):
        ts_str, pkt, frame = m.group(1), int(m.group(2)), int(m.group(3))
        recv_times.append((int(frame), int(pkt), parse_ts(ts_str)))
    return recv_times


def main():
    parser = argparse.ArgumentParser(
        description="Draw detailed latency timeline (capture, encode, send, recv)"
    )
    parser.add_argument("result_dir", type=str, help="Result directory (e.g. result/10to1_mock)")
    parser.add_argument("start_frame", type=int, help="Start frame index (inclusive)")
    parser.add_argument("end_frame", type=int, help="End frame index (inclusive)")
    parser.add_argument(
        "--fps",
        type=int,
        default=30,
        help="FPS used for encoded-size kbps annotation (default: 30)",
    )
    parser.add_argument(
        "--out",
        type=str,
        default=None,
        help="Output plot path (default: result_dir/figs/latency_timeline_<start>_<end>.png)",
    )
    args = parser.parse_args()

    result_dir = Path(args.result_dir)
    send_log = result_dir / "send.log"
    recv_log = result_dir / "recv.log"
    if not send_log.exists() or not recv_log.exists():
        print(f"Missing {send_log} or {recv_log}", file=sys.stderr)
        sys.exit(1)

    start_f, end_f = args.start_frame, args.end_frame
    if start_f > end_f:
        print("start_frame must be <= end_frame", file=sys.stderr)
        sys.exit(1)

    capture_times, send_start, send_end, bitrate_events, feedback_times, frame_sizes_bytes, packet_send_events = parse_send_log(send_log)
    recv_times = parse_recv_log(recv_log)

    if start_f not in capture_times:
        print(f"start_frame {start_f} not found in send.log (no capture time)", file=sys.stderr)
        sys.exit(1)
    t0 = capture_times[start_f]

    def to_ms(ts: float) -> float:
        return (ts - t0) * 1000.0

    fps = args.fps

    # Layer 1: frame capture (only frames in [start_f, end_f])
    layer_capture = []
    for f in range(start_f, end_f + 1):
        if f in capture_times:
            layer_capture.append((to_ms(capture_times[f]), f))

    # Layer 2: frame encode done (= send start for that frame)
    layer_encode_done = []
    for f in range(start_f, end_f + 1):
        if f in send_start:
            layer_encode_done.append((to_ms(send_start[f][0]), f))

    # Layer 3: every packet send (use real log times if available, else interpolate)
    layer_pkt_send = []
    send_ts = {}
    if packet_send_events:
        for (ts, frame, pkt) in packet_send_events:
            if start_f <= frame <= end_f:
                t_ms = to_ms(ts)
                layer_pkt_send.append((t_ms, frame, pkt))
                send_ts[(frame, pkt)] = t_ms
    else:
        for f in range(start_f, end_f + 1):
            if f not in send_start or f not in send_end:
                continue
            t_start, n = send_start[f]
            t_end = send_end[f]
            for p in range(n):
                if n <= 1:
                    t = t_start
                else:
                    t = t_start + (t_end - t_start) * p / (n - 1)
                t_ms = to_ms(t)
                layer_pkt_send.append((t_ms, f, p))
                send_ts[(f, p)] = t_ms

    # Layer 4: sender received feedback
    layer_feedback = []
    feedback_ts = {}
    for (frame, pkt, ts) in feedback_times:
        if start_f <= frame <= end_f:
            t_ms = to_ms(ts)
            layer_feedback.append((t_ms, frame, pkt))
            feedback_ts[(frame, pkt)] = t_ms

    # Layer 5: every packet receive (receiver side)
    layer_pkt_recv = []
    recv_ts = {}
    for (frame, pkt, ts) in recv_times:
        if start_f <= frame <= end_f:
            t_ms = to_ms(ts)
            layer_pkt_recv.append((t_ms, frame, pkt))
            recv_ts[(frame, pkt)] = t_ms

    # Bitrate change events in the time window of this frame range
    t_start_win = capture_times.get(start_f, 0)
    t_end_win = max(
        capture_times.get(end_f, 0),
        send_end[end_f] if end_f in send_end else 0,
    )
    for (frame, pkt, ts) in recv_times:
        if start_f <= frame <= end_f:
            t_end_win = max(t_end_win, ts)

    bitrate_in_window = [
        (to_ms(ts), kbps) for ts, kbps in bitrate_events
        if t_start_win <= ts <= t_end_win
    ]

    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("matplotlib required for drawing", file=sys.stderr)
        sys.exit(1)

    fig, ax = plt.subplots(figsize=(16, 7))
    # y from bottom (0) to top (4): capture, encode, send, recv, feedback
    layer_ys = [0, 1, 2, 3, 4]
    layer_labels = [
        "Frame capture",
        "Encode done",
        "Packet send",
        "Packet recv",
        "Sender recv feedback",
    ]

    # Draw layer events
    if layer_capture:
        xs = [x for x, _ in layer_capture]
        ax.scatter(xs, [layer_ys[0]] * len(xs), c="C0", s=80, label=layer_labels[0], alpha=0.8)
        # annotate frame index near capture point
        for (t_ms, f) in layer_capture:
            ax.text(
                t_ms,
                layer_ys[0] + 0.15,
                str(f),
                fontsize=14,
                ha="center",
                va="bottom",
                color="C0",
            )
    if layer_encode_done:
        xs = [x for x, _ in layer_encode_done]
        ax.scatter(xs, [layer_ys[1]] * len(xs), c="C1", s=80, label=layer_labels[1], alpha=0.8)
        # annotate encoded size (kbps) if we know the frame size in bytes
        for (t_ms, f) in layer_encode_done:
            size_bytes = frame_sizes_bytes.get(f)
            if size_bytes is None:
                continue
            size_kbps = size_bytes * 8.0 * fps / 1000.0
            ax.text(
                t_ms,
                layer_ys[1] + 0.15,
                f"{size_kbps:.0f}",
                fontsize=14,
                ha="center",
                va="bottom",
                color="C1",
            )
    if layer_pkt_send:
        xs = [x for x, _, _ in layer_pkt_send]
        ax.scatter(xs, [layer_ys[2]] * len(xs), c="C2", s=50, label=layer_labels[2], alpha=0.6)
    if layer_pkt_recv:
        xs = [x for x, _, _ in layer_pkt_recv]
        ax.scatter(xs, [layer_ys[3]] * len(xs), c="C3", s=6, label=layer_labels[3], alpha=0.6)
        # Per-frame latency at last packet receive position (load from CSV or compute from timeline)
        frame_latency_csv = result_dir / "frame_latency.csv"
        latencies_by_frame = {}
        if frame_latency_csv.exists():
            with open(frame_latency_csv) as f:
                for row in csv.DictReader(f):
                    latencies_by_frame[int(row["frame_index"])] = float(row["frame_latency"])
        last_recv_by_frame = {}
        for (t_ms, frame, pkt) in layer_pkt_recv:
            if frame not in last_recv_by_frame or t_ms > last_recv_by_frame[frame][0]:
                last_recv_by_frame[frame] = (t_ms, pkt)
        encode_done_ms = {f: t for t, f in layer_encode_done}
        y_recv = layer_ys[3]
        offset = 0.25
        for i, frame in enumerate(range(start_f, end_f + 1)):
            if frame not in last_recv_by_frame:
                continue
            x_last = last_recv_by_frame[frame][0]
            if frame in latencies_by_frame:
                lat_ms = latencies_by_frame[frame]
            else:
                t_encode = encode_done_ms.get(frame)
                if t_encode is not None:
                    lat_ms = x_last - t_encode
                else:
                    continue
            # Alternate top/bottom to avoid overlap: frame 0 top, 1 bottom, 2 top, ...
            if (i % 2) == 0:
                y_text = y_recv + offset
                va = "bottom"
            else:
                y_text = y_recv - offset
                va = "top"
            ax.annotate(
                f"{lat_ms:.0f}ms",
                xy=(x_last, y_recv),
                xytext=(x_last, y_text),
                fontsize=12,
                ha="left",
                va=va,
                color="C3",
                arrowprops=dict(arrowstyle="->", color="C3", lw=1.2),
            )
    if layer_feedback:
        xs = [x for x, _, _ in layer_feedback]
        ax.scatter(xs, [layer_ys[4]] * len(xs), c="C4", s=6, label=layer_labels[4], alpha=0.6)

    # Lines connecting first-packet send -> recv -> sender feedback (per frame)
    y_send = layer_ys[2]
    y_recv = layer_ys[3]
    y_fb = layer_ys[4]
    for frame in range(start_f, end_f + 1):
        key = (frame, 0)  # first packet of this frame
        t_send = send_ts.get(key)
        if t_send is None:
            continue
        xs = [t_send]
        ys = [y_send]
        t_recv = recv_ts.get(key)
        if t_recv is not None:
            xs.append(t_recv)
            ys.append(y_recv)
        t_fb = feedback_ts.get(key)
        if t_fb is not None:
            xs.append(t_fb)
            ys.append(y_fb)
        if len(xs) >= 2:
            ax.plot(xs, ys, color="lightgray", alpha=1.0, linewidth=2.0)

    # Red point + arrow for each bitrate change
    for t_ms, kbps in bitrate_in_window:
        # Vertical line from bottom layer to top layer to indicate bitrate change time
        ax.vlines(
            t_ms,
            layer_ys[0],
            layer_ys[-1] + 0.5,
            colors="red",
            linestyles="--",
            linewidth=2.0,
            alpha=0.7,
        )
        ax.annotate(
            f"bitrate updated to {kbps} kbps",
            xy=(t_ms, layer_ys[-1] + 0.3),
            xytext=(t_ms + 20.0, layer_ys[-1] + 0.3),
            fontsize=20,
            color="red",
            arrowprops=dict(arrowstyle="->", color="red", lw=1.5),
            ha="left",
            va="center",
        )

    ax.set_xlabel("Time (ms, 0 = capture of frame {})".format(start_f), fontsize=20)
    # ax.set_ylabel("Layer", fontsize=20)
    ax.set_yticks(layer_ys)
    ax.set_yticklabels(layer_labels)
    ax.set_ylim(-0.5, 4.8)
    ax.grid(True, alpha=0.3, axis="x")
    ax.set_title(
        f"Latency timeline (frames {start_f}–{end_f})",
        fontsize=20,
    )
    # ax.legend(loc="lower right", fontsize=20)
    ax.tick_params(axis="both", labelsize=20)
    fig.tight_layout()

    out_path = args.out
    if out_path is None:
        fig_dir = result_dir / "figs"
        fig_dir.mkdir(parents=True, exist_ok=True)
        out_path = fig_dir / f"latency_timeline_{start_f}_{end_f}.png"
    else:
        out_path = Path(out_path)
        out_path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(out_path, dpi=150)
    plt.close(fig)
    print(f"Saved {out_path}")


if __name__ == "__main__":
    main()
