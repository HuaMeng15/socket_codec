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

    # GCC_STATE lines
    re_gcc = re.compile(
        r"\[" + _TS + r"\].*\[GCC_STATE\] target=(\d+) delay_based=(\d+) "
        r"loss_based=(\d+) slope=(\S+) threshold=(\S+) usage=(\S+) "
        r"queuing_delay_ms=(\S+)"
    )
    gcc_rows = []
    for m in re_gcc.finditer(text):
        gcc_rows.append({
            "ts": parse_ts(m.group(1)),
            "target": int(m.group(2)),
            "delay_based": int(m.group(3)),
            "loss_based": int(m.group(4)),
            "slope": float(m.group(5)),
            "threshold": float(m.group(6)),
            "usage": m.group(7),
            "queuing_delay": float(m.group(8)),
        })

    # SCHEDULE lines (link capacity ground truth)
    re_sched = re.compile(
        r"\[" + _TS + r"\].*\[SCHEDULE\] t=(\d+)ms set bandwidth to (\d+) kbps"
    )
    sched = []
    for m in re_sched.finditer(text):
        sched.append({"ts": parse_ts(m.group(1)), "kbps": int(m.group(2))})

    # Initial bandwidth from the "Network simulator enabled: bw=NNNNkbps"
    re_init_bw = re.search(r"Network simulator enabled: bw=(\d+)kbps", text)
    init_bw = int(re_init_bw.group(1)) if re_init_bw else None

    # Achieved send bitrate from per-frame sent bytes
    re_send = re.compile(
        r"\[" + _TS + r"\].*\[DataSender\] Sending frame (\d+) size=(\d+) bytes"
    )
    sends = []
    for m in re_send.finditer(text):
        sends.append({"ts": parse_ts(m.group(1)),
                      "frame": int(m.group(2)),
                      "bytes": int(m.group(3))})

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

    # Build link-capacity step function over time
    cap_t, cap_v = [], []
    if init_bw is not None:
        cap_t.append(0.0)
        cap_v.append(init_bw)
    for s in sched:
        cap_t.append(s["ts"] - t0)
        cap_v.append(s["kbps"])

    # Achieved send bitrate: sliding window over frames (kbps)
    # group sends into ~0.5s bins
    achieved_t, achieved_v = [], []
    if sends:
        bin_s = 0.5
        bin_start = sends[0]["ts"]
        acc_bytes = 0
        for s in sends:
            if s["ts"] - bin_start >= bin_s:
                kbps = acc_bytes * 8 / 1000.0 / (s["ts"] - bin_start)
                achieved_t.append(bin_start - t0)
                achieved_v.append(kbps)
                bin_start = s["ts"]
                acc_bytes = 0
            acc_bytes += s["bytes"]

    # Write CSV for reference
    csv_path = result_dir / "gcc_state.csv"
    with open(csv_path, "w") as f:
        f.write("t_sec,target_kbps,delay_based_kbps,loss_based_kbps,"
                "slope,threshold,usage,queuing_delay_ms\n")
        for r in gcc_rows:
            f.write(f"{r['ts']-t0:.3f},{r['target']},{r['delay_based']},"
                    f"{r['loss_based']},{r['slope']:.4f},{r['threshold']:.3f},"
                    f"{r['usage']},{r['queuing_delay']:.3f}\n")
    print(f"Wrote {csv_path} ({len(gcc_rows)} GCC samples)")

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
        ax1.plot(achieved_t, achieved_v, label="Achieved send rate",
                 color="C3", alpha=0.6, linestyle=":")
    ax1.set_ylabel("Bitrate (kbps)")
    ax1.set_title("GCC Congestion Control: Bitrate vs Link Capacity")
    ax1.legend(loc="upper right")
    ax1.grid(True, alpha=0.3)

    # Panel 2: queuing delay (integrated trendline signal)
    if gcc_rows:
        ax2.plot(gt, [r["queuing_delay"] for r in gcc_rows],
                 label="Queuing delay (accumulated)", color="C5")
        # Mark overuse events
        ovr_t = [r["ts"] - t0 for r in gcc_rows if r["usage"] == "overuse"]
        for t in ovr_t:
            ax2.axvline(t, color="red", alpha=0.1)
    ax2.set_ylabel("Queuing delay (ms)")
    ax2.set_xlabel("Time (s)")
    ax2.set_title("Integrated queuing delay (red lines = overuse events)")
    ax2.legend(loc="upper right")
    ax2.grid(True, alpha=0.3)

    fig.tight_layout()
    out = fig_dir / "gcc_behavior.png"
    fig.savefig(out, dpi=100)
    print(f"Wrote {out}")


if __name__ == "__main__":
    main()
