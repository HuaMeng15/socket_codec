#!/usr/bin/env python3
"""
Draw latency and frame-size plots from result_dir CSVs and logs.
Usage: python3 scripts/draw.py <result_dir> [--latency] [--frame-size]
  Default: draw all plots for which data exists.
  --latency: only draw latency plots (frame_latency, overall_latency, packet_latency_hist)
  --frame-size: only draw frame size / bitrate plot
"""
import argparse
import csv
import sys
from pathlib import Path

def main():
    parser = argparse.ArgumentParser(
        description="Draw latency and frame-size plots from result_dir"
    )
    parser.add_argument(
        "result_dir",
        type=str,
        help="Result directory (e.g. result/10to1_mock)",
    )
    parser.add_argument(
        "--latency",
        action="store_true",
        help="Only draw latency plots",
    )
    parser.add_argument(
        "--frame-size",
        action="store_true",
        help="Only draw frame size / bitrate plot",
    )
    args = parser.parse_args()

    result_dir = Path(args.result_dir)
    fig_dir = result_dir / "figs"
    do_latency = args.latency or (not args.latency and not args.frame_size)
    do_frame_size = args.frame_size or (not args.latency and not args.frame_size)

    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("matplotlib not found, skipping plots", file=sys.stderr)
        sys.exit(1)

    fig_dir.mkdir(parents=True, exist_ok=True)
    drawn = []

    if do_latency:
        # Frame latency
        frame_csv = result_dir / "frame_latency.csv"
        if frame_csv.exists():
            idx, lat = [], []
            with open(frame_csv) as f:
                r = csv.DictReader(f)
                for row in r:
                    idx.append(int(row["frame_index"]))
                    lat.append(float(row["frame_latency"]))
            if idx:
                fig, ax = plt.subplots(figsize=(10, 4))
                ax.plot(idx, lat, alpha=0.7)
                ax.set_xlabel("Frame index")
                ax.set_ylabel("Latency (ms)")
                ax.set_title("Frame latency (send_done -> decode_done)")
                ax.grid(True, alpha=0.3)
                fig.tight_layout()
                fig.savefig(fig_dir / "frame_latency.png", dpi=150)
                plt.close(fig)
                drawn.append("frame_latency.png")

        # Overall latency
        overall_csv = result_dir / "overall_latency.csv"
        if overall_csv.exists():
            idx, lat = [], []
            with open(overall_csv) as f:
                r = csv.DictReader(f)
                for row in r:
                    idx.append(int(row["frame_index"]))
                    lat.append(float(row["overall_latency"]))
            if idx:
                fig, ax = plt.subplots(figsize=(10, 4))
                ax.plot(idx, lat, alpha=0.7, color="C1")
                ax.set_xlabel("Frame index")
                ax.set_ylabel("Latency (ms)")
                ax.set_title("Overall latency (capture -> decode)")
                ax.grid(True, alpha=0.3)
                fig.tight_layout()
                fig.savefig(fig_dir / "overall_latency.png", dpi=150)
                plt.close(fig)
                drawn.append("overall_latency.png")

        # Packet latency histogram
        packet_csv = result_dir / "packet_latency.csv"
        if packet_csv.exists():
            pkt_lats = []
            with open(packet_csv) as f:
                r = csv.DictReader(f)
                for row in r:
                    pkt_lats.append(float(row["packet_latency"]))
            if pkt_lats:
                fig, ax = plt.subplots(figsize=(8, 4))
                ax.hist(pkt_lats, bins=50, alpha=0.7, edgecolor="black")
                ax.set_xlabel("Packet latency (ms)")
                ax.set_ylabel("Count")
                ax.set_title("Packet latency distribution")
                ax.grid(True, alpha=0.3)
                fig.tight_layout()
                fig.savefig(fig_dir / "packet_latency_hist.png", dpi=150)
                plt.close(fig)
                drawn.append("packet_latency_hist.png")

    if do_frame_size:
        frame_size_log = result_dir / "frame_size.log"
        if frame_size_log.exists():
            indices, size_kbps_list, target_kbps_list = [], [], []
            with open(frame_size_log) as f:
                r = csv.DictReader(f)
                for row in r:
                    indices.append(int(row["frame_index"]))
                    size_kbps_list.append(float(row["frame_size_kbps"]))
                    target_kbps_list.append(int(row["target_bitrate_kbps"]))
            if indices:
                fig, ax = plt.subplots(figsize=(10, 5))
                ax.plot(indices, size_kbps_list, label="Frame size (kbps)", alpha=0.8)
                ax.plot(indices, target_kbps_list, label="Target bitrate (kbps)", alpha=0.8)
                ax.set_xlabel("Frame index")
                ax.set_ylabel("kbps")
                ax.legend()
                ax.grid(True, alpha=0.3)
                ax.set_title("Frame size and target bitrate vs frame index")
                fig.tight_layout()
                fig.savefig(fig_dir / "frame_size_rate.png", dpi=150)
                plt.close(fig)
                drawn.append("frame_size_rate.png")

    if drawn:
        print(f"Plots saved to {fig_dir}/: {', '.join(drawn)}")
    else:
        print("No data found to plot.", file=sys.stderr)


if __name__ == "__main__":
    main()
