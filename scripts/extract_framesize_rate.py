#!/usr/bin/env python3
"""
Extract per-frame size and target bitrate from send.log; write frame_size.log.
Use scripts/draw.py to generate plots from the result dir.
Usage: python3 scripts/extract_framesize_rate.py <result_dir> [--fps 30]
"""
import argparse
import re
import sys
from datetime import datetime
from pathlib import Path


# Only match our app's timestamp (YYYY-MM-DD HH:MM:SS.fff) so x264 "[info]" etc. are not captured when logs are interleaved.
_TS = r"(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d+)"


def parse_ts(s: str) -> float:
    return datetime.strptime(s.strip("[]"), "%Y-%m-%d %H:%M:%S.%f").timestamp()


def main():
    parser = argparse.ArgumentParser(description="Extract frame size and bitrate from send.log")
    parser.add_argument("result_dir", type=str, help="Result directory (e.g. result/10to1_mock)")
    parser.add_argument("--fps", type=int, default=30, help="FPS for bytes->kbps conversion")
    args = parser.parse_args()

    result_dir = Path(args.result_dir)
    send_log = result_dir / "send.log"
    fps = args.fps

    if not send_log.exists():
        print(f"Missing {send_log}", file=sys.stderr)
        sys.exit(1)

    text = send_log.read_text()

    # [DataSender] Sending frame N size=XXXX bytes in M packets
    # [DataSender] Sending frame N fragment size=XXXX bytes in M packets
    re_send_size = re.compile(
        r"\[" + _TS + r"\].*\[DataSender\] Sending frame (\d+) (?:fragment )?size=(\d+) bytes"
    )
    # [Encoder] Initial bitrate X kbps (from any encoder implementation)
    re_initial = re.compile(
        r"\[" + _TS + r"\].*\[Encoder\] Initial bitrate (\d+) kbps"
    )
    # [Encoder] Set target bitrate to X kbps
    re_set_bitrate = re.compile(
        r"\[" + _TS + r"\].*\[Encoder\] Set target bitrate to (\d+) kbps"
    )
    # [SlicePacedEncoder] Target bitrate now X kbps, next frame slices=Y
    re_slice_set_bitrate = re.compile(
        r"\[" + _TS + r"\].*\[SlicePacedEncoder\] Target bitrate now (\d+) kbps"
    )
    # [SlicePacedEncoder] Frame N slice=S bitrate=X ...
    # Existing x264_slice logs did not include a generic initial bitrate event,
    # so use the per-frame encoder bitrate when it is available.
    re_slice_frame_bitrate = re.compile(
        r"\[" + _TS + r"\].*\[SlicePacedEncoder\] Frame (\d+) slice=\d+ bitrate=(\d+)"
    )

    # Frame events keyed by frame index so multiple fragments collapse into one
    # per-frame row.
    frames = {}
    for m in re_send_size.finditer(text):
        ts_str, frame_idx, size_str = m.group(1), int(m.group(2)), int(m.group(3))
        ts = parse_ts(ts_str)
        if frame_idx not in frames:
            frames[frame_idx] = {"ts": ts, "size": 0}
        frames[frame_idx]["ts"] = min(frames[frame_idx]["ts"], ts)
        frames[frame_idx]["size"] += size_str

    frame_rows = sorted(
        ((data["ts"], frame_idx, data["size"]) for frame_idx, data in frames.items()),
        key=lambda x: (x[0], x[1]),
    )

    frame_bitrates = {}
    for m in re_slice_frame_bitrate.finditer(text):
        frame_idx, rate = int(m.group(2)), int(m.group(3))
        frame_bitrates.setdefault(frame_idx, rate)

    # Bitrate events: (timestamp, bitrate_kbps)
    bitrate_events = []
    for m in re_initial.finditer(text):
        ts_str, rate = m.group(1), int(m.group(2))
        bitrate_events.append((parse_ts(ts_str), rate))
    for m in re_set_bitrate.finditer(text):
        ts_str, rate = m.group(1), int(m.group(2))
        bitrate_events.append((parse_ts(ts_str), rate))
    for m in re_slice_set_bitrate.finditer(text):
        ts_str, rate = m.group(1), int(m.group(2))
        bitrate_events.append((parse_ts(ts_str), rate))
    bitrate_events.sort(key=lambda x: x[0])

    # For each frame, target bitrate = last bitrate event at or before frame timestamp
    def get_target_bitrate(frame_idx, ts_frame):
        if frame_idx in frame_bitrates:
            return frame_bitrates[frame_idx]
        cand = [r for t, r in bitrate_events if t <= ts_frame]
        if cand:
            return cand[-1]
        return bitrate_events[0][1] if bitrate_events else 0

    # Build output rows: frame_index, frame_size_bytes, frame_size_kbps, target_bitrate_kbps
    out_rows = []
    for ts, frame_idx, size_bytes in frame_rows:
        size_kbps = size_bytes * 8 * fps / 1000.0
        target_kbps = get_target_bitrate(frame_idx, ts)
        out_rows.append((frame_idx, size_bytes, size_kbps, target_kbps))

    # Write frame_size.log
    out_path = result_dir / "frame_size.log"
    with open(out_path, "w") as f:
        f.write("frame_index,frame_size_bytes,frame_size_kbps,target_bitrate_kbps\n")
        for frame_idx, size_bytes, size_kbps, target_kbps in out_rows:
            f.write(f"{frame_idx},{size_bytes},{size_kbps:.2f},{target_kbps}\n")

    print(f"Wrote {out_path} ({len(out_rows)} frames)")


if __name__ == "__main__":
    main()
