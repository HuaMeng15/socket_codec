#!/usr/bin/env python3
"""
Plot GCC congestion-control behavior from a sender log.

Extracts:
  - [GCC_STATE] lines: target / delay_based / loss_based bitrate, one-way delay
  - [Encoder] Set target bitrate lines: encoder's applied bitrate
  - [SCHEDULE] lines: the simulated link bandwidth steps (ground truth)
  - [DataSender] Sending frame ... : actual sent bytes -> achieved send bitrate

Produces <result_dir>/figs/gcc_behavior.png with two stacked panels:
  (1) bitrate: link capacity, GCC target, delay-based, loss-based, achieved
  (2) one-way delay over time

Usage: python3 scripts/plot_gcc.py <result_dir>
"""
import re
import sys
from datetime import datetime
from pathlib import Path

_TS = r"(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d+)"


def parse_ts(s):
    return datetime.strptime(s.strip("[]"), "%Y-%m-%d %H:%M:%S.%f").timestamp()


def main():
    if len(sys.argv) < 2:
        print("Usage: python3 scripts/plot_gcc.py <result_dir>", file=sys.stderr)
        sys.exit(1)
    result_dir = Path(sys.argv[1])
    send_log = result_dir / "send.log"
    if not send_log.exists():
        print(f"Missing {send_log}", file=sys.stderr)
        sys.exit(1)

    text = send_log.read_text()

    # GCC_STATE lines. Parse every "key=value" token generically so the CSV
    # automatically tracks whatever fields the log line emits — no need to edit
    # this regex each time a metric is added to GccController.
    re_gcc_line = re.compile(r"\[" + _TS + r"\].*\[GCC_STATE\] (.*)")
    re_kv = re.compile(r"(\w+)=(\S+)")

    def _coerce(v):
        """int if it looks like one, else float, else the raw string."""
        try:
            return int(v)
        except ValueError:
            pass
        try:
            return float(v)
        except ValueError:
            return v

    gcc_rows = []
    # Preserve the field order as first seen in the log for stable CSV columns.
    field_order = []
    for m in re_gcc_line.finditer(text):
        row = {"ts": parse_ts(m.group(1))}
        for km in re_kv.finditer(m.group(2)):
            key, val = km.group(1), km.group(2)
            row[key] = _coerce(val)
            if key not in field_order:
                field_order.append(key)
        gcc_rows.append(row)

    # Back-compat aliases used by the plotting code below.
    for r in gcc_rows:
        r["target"] = r.get("target", 0)
        r["delay_based"] = r.get("delay_based", 0)
        r["loss_based"] = r.get("loss_based", 0)
        r["slope"] = r.get("slope", 0.0)
        r["threshold"] = r.get("threshold", 0.0)
        r["usage"] = r.get("usage", "")
        r["queuing_delay"] = r.get("queuing_delay_ms", 0.0)

    # SCHEDULE lines (link capacity ground truth).
    # Groups: 1=timestamp, 2=schedule time (t=Nms), 3=bandwidth kbps.
    re_sched = re.compile(
        r"\[" + _TS + r"\].*\[SCHEDULE\] t=(\d+)ms set bandwidth to (\d+) kbps"
    )
    sched = []
    for m in re_sched.finditer(text):
        sched.append({"ts": parse_ts(m.group(1)), "kbps": int(m.group(3))})

    # Initial bandwidth from the "Network simulator enabled: bw=NNNNkbps"
    re_init_bw = re.search(r"Network simulator enabled: bw=(\d+)kbps", text)
    init_bw = int(re_init_bw.group(1)) if re_init_bw else None

    # Achieved send bitrate from per-frame sent bytes
    re_send = re.compile(
        r"\[" + _TS + r"\].*\[DataSender\] Sending frame (\d+) size=(\d+) bytes"
    )
    sends = []
    send_ts_by_frame = {}
    for m in re_send.finditer(text):
        ts = parse_ts(m.group(1))
        frame = int(m.group(2))
        sends.append({"ts": ts, "frame": frame, "bytes": int(m.group(3))})
        # First time we see this frame's send (the send-start timestamp).
        send_ts_by_frame.setdefault(frame, ts)

    # Real one-way latency from the receiver log: for each frame, the time the
    # last packet completed it (decode), minus the frame's send time. Sender and
    # receiver share the wall clock (localhost / same host), so this is a true
    # latency — not the trendline's accumulated delay-variation signal.
    recv_log = result_dir / "recv.log"
    latency_pts = []  # (send_ts, latency_ms)
    if recv_log.exists():
        rtext = recv_log.read_text()
        re_decode = re.compile(
            r"\[" + _TS + r"\].*Successfully decoded frame (\d+)")
        # Fallback: last received packet for a frame, if no decode line.
        re_rpkt = re.compile(
            r"\[" + _TS + r"\].*Received packet \d+ for frame (\d+)")
        recv_done = {}
        for m in re_decode.finditer(rtext):
            recv_done[int(m.group(2))] = parse_ts(m.group(1))
        if not recv_done:
            for m in re_rpkt.finditer(rtext):
                # keep the latest timestamp seen per frame
                recv_done[int(m.group(2))] = parse_ts(m.group(1))
        for frame, rts in recv_done.items():
            sts = send_ts_by_frame.get(frame)
            if sts is not None:
                latency_pts.append((sts, (rts - sts) * 1000.0))
        latency_pts.sort()

    if not gcc_rows:
        print("No [GCC_STATE] lines found — did the run receive feedback?",
              file=sys.stderr)

    # Establish t0 as earliest timestamp across all series
    all_ts = ([r["ts"] for r in gcc_rows] + [s["ts"] for s in sched] +
              [s["ts"] for s in sends])
    if not all_ts:
        print("No data to plot", file=sys.stderr)
        sys.exit(1)
    t0 = min(all_ts)
    t_end = max(all_ts) - t0

    # Build link-capacity step function over time. Start at t=0 with the
    # initial bandwidth (or the first scheduled value), apply each scheduled
    # step at its time, and extend the final level out to t_end so the last
    # segment is drawn as a horizontal line (not a single point).
    cap_t, cap_v = [], []
    if init_bw is not None:
        cap_t.append(0.0)
        cap_v.append(init_bw)
    for s in sched:
        st = s["ts"] - t0
        # Avoid a duplicate point at t=0 (schedule's first step often coincides
        # with the initial bandwidth) which would collapse into a vertical line.
        if cap_t and abs(st - cap_t[-1]) < 1e-6:
            cap_v[-1] = s["kbps"]
        else:
            cap_t.append(st)
            cap_v.append(s["kbps"])
    # Extend the last level to the end of the run.
    if cap_t and cap_t[-1] < t_end:
        cap_t.append(t_end)
        cap_v.append(cap_v[-1])

    # Achieved send bitrate, per frame: treat the mock encoder like a real one
    # and translate each frame's encoded size into an instantaneous bitrate.
    # rate = frame_bytes * 8 / frame_interval, where the interval is the time to
    # the next frame's send (last frame reuses the previous interval).
    achieved_t, achieved_v = [], []
    if len(sends) >= 2:
        for i, s in enumerate(sends):
            if i + 1 < len(sends):
                dt = sends[i + 1]["ts"] - s["ts"]
            else:
                dt = s["ts"] - sends[i - 1]["ts"]
            if dt <= 0:
                continue
            kbps = s["bytes"] * 8 / 1000.0 / dt
            achieved_t.append(s["ts"] - t0)
            achieved_v.append(kbps)

    # Write CSV for reference. Columns = t_sec followed by every field seen in
    # the GCC_STATE lines, in the order they first appeared in the log. This
    # stays complete automatically as new metrics are added to the log line.
    csv_path = result_dir / "gcc_state.csv"

    def _fmt(v):
        if isinstance(v, float):
            return f"{v:.4f}"
        return str(v)

    with open(csv_path, "w") as f:
        f.write("t_sec," + ",".join(field_order) + "\n")
        for r in gcc_rows:
            cells = [f"{r['ts']-t0:.3f}"] + [_fmt(r.get(k, "")) for k in field_order]
            f.write(",".join(cells) + "\n")
    print(f"Wrote {csv_path} ({len(gcc_rows)} GCC samples, "
          f"{len(field_order)} metrics: {', '.join(field_order)})")

    # Plot
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("matplotlib not found, CSV written but no plot", file=sys.stderr)
        return

    fig_dir = result_dir / "figs"
    fig_dir.mkdir(parents=True, exist_ok=True)

    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(12, 8), sharex=True)

    gt = [r["ts"] - t0 for r in gcc_rows]
    # Panel 1: bitrates
    if cap_t:
        ax1.step(cap_t, cap_v, where="post", label="Link capacity",
                 color="black", linewidth=2, linestyle="--")
    if gcc_rows:
        ax1.plot(gt, [r["target"] for r in gcc_rows], label="GCC target",
                 color="C0", linewidth=2)
        ax1.plot(gt, [r["delay_based"] for r in gcc_rows], label="Delay-based",
                 color="C1", alpha=0.7)
        ax1.plot(gt, [r["loss_based"] for r in gcc_rows], label="Loss-based",
                 color="C2", alpha=0.7)
    if achieved_t:
        ax1.plot(achieved_t, achieved_v, label="Achieved send rate (per frame)",
                 color="C3", alpha=0.6, linestyle=":")
    ax1.set_ylabel("Bitrate (kbps)")
    ax1.set_title("GCC Congestion Control: Bitrate vs Link Capacity")
    ax1.legend(loc="upper right")
    ax1.grid(True, alpha=0.3)

    # Panel 2: real per-frame one-way latency (recv_done - send), from logs.
    if latency_pts:
        lt = [(ts - t0) for ts, _ in latency_pts]
        lv = [lat for _, lat in latency_pts]
        ax2.plot(lt, lv, label="Frame latency (recv - send)", color="C5")
        # Mark overuse events from the GCC state stream.
        ovr_t = [r["ts"] - t0 for r in gcc_rows if r["usage"] == "overuse"]
        for t in ovr_t:
            ax2.axvline(t, color="red", alpha=0.1)
    ax2.set_ylabel("Latency (ms)")
    ax2.set_xlabel("Time (s)")
    ax2.set_title("Real one-way frame latency (red lines = overuse events)")
    ax2.legend(loc="upper right")
    ax2.grid(True, alpha=0.3)
    ax2.set_ylim(bottom=0)

    fig.tight_layout()
    out = fig_dir / "gcc_behavior.png"
    fig.savefig(out, dpi=100)
    print(f"Wrote {out}")


if __name__ == "__main__":
    main()
