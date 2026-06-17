#!/usr/bin/env python3
"""
Plot real-WebRTC (SparkRTC) GCC behavior from an extracted gcc_state.csv and,
if available, overlay our implementation's target for comparison.

Usage: python3 scripts/plot_webrtc_gcc.py <webrtc_result_dir> [our_result_dir]
"""
import csv
import sys
from pathlib import Path


def load(csv_path):
    rows = []
    with open(csv_path) as f:
        for r in csv.DictReader(f):
            row = {}
            for k, v in r.items():
                try:
                    row[k] = float(v)
                except (ValueError, TypeError):
                    row[k] = v  # keep non-numeric columns (e.g. usage) as-is
            rows.append(row)
    return rows


def main():
    if len(sys.argv) < 2:
        print("Usage: plot_webrtc_gcc.py <webrtc_dir> [our_dir]", file=sys.stderr)
        sys.exit(1)
    wdir = Path(sys.argv[1])
    wrows = load(wdir / "gcc_state.csv")

    our_rows = None
    if len(sys.argv) >= 3:
        ours = Path(sys.argv[2]) / "gcc_state.csv"
        if ours.exists():
            our_rows = load(ours)

    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(13, 8), sharex=True)

    wt = [r["t_sec"] for r in wrows]
    # Link capacity reference (static 10 Mbps)
    ax1.axhline(10000, color="black", linestyle="--", linewidth=2,
                label="Link capacity (10 Mbps)")
    # NOTE: in real WebRTC the loss-based estimator is seeded with the
    # delay-based estimate, so target/delay-based/loss-based coincide ~99.7%
    # of the time. Draw them with distinct styles + widths so overlapping
    # lines remain individually visible (otherwise the last-drawn line hides
    # the others).
    ax1.plot(wt, [r["delay_based_kbps"] for r in wrows], color="C1",
             linewidth=4, alpha=0.5, label="WebRTC delay-based")
    ax1.plot(wt, [r["loss_based_kbps"] for r in wrows], color="C2",
             linewidth=2.2, alpha=0.8, label="WebRTC loss-based")
    ax1.plot(wt, [r["target_kbps"] for r in wrows], color="C0",
             linewidth=1.0, linestyle=":", label="WebRTC target")
    if our_rows:
        ot = [r["t_sec"] for r in our_rows]
        ax1.plot(ot, [r["target_kbps"] for r in our_rows], color="C3",
                 label="Ours target", linewidth=1.5)
    ax1.set_ylabel("Bitrate (kbps)")
    ax1.set_title("GCC bitrate under static 10 Mbps: real WebRTC (SparkRTC)"
                  + (" vs ours" if our_rows else ""))
    ax1.legend(loc="lower right", fontsize=9)
    ax1.grid(True, alpha=0.3)
    ax1.set_ylim(0, 12000)

    # Panel 2: RTT (WebRTC) — the delay signal
    ax2.plot(wt, [r["rtt_ms"] for r in wrows], color="C4",
             label="WebRTC RTT")
    ax2.set_ylabel("RTT (ms)")
    ax2.set_xlabel("Time (s)")
    ax2.set_title("WebRTC round-trip time")
    ax2.legend(loc="upper right")
    ax2.grid(True, alpha=0.3)

    fig_dir = wdir / "figs"
    fig_dir.mkdir(parents=True, exist_ok=True)
    out = fig_dir / "webrtc_gcc_behavior.png"
    fig.tight_layout()
    fig.savefig(out, dpi=100)
    print(f"Wrote {out}")

    # Quick stats
    import statistics
    conv = [r["target_kbps"] for r in wrows if r["t_sec"] > 10]
    if conv:
        print(f"WebRTC converged (t>10s): mean={statistics.mean(conv):.0f} "
              f"min={min(conv):.0f} max={max(conv):.0f} kbps")


if __name__ == "__main__":
    main()
