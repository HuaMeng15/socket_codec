#!/usr/bin/env python3
import argparse
import csv
import math
import re
import shutil
import statistics
import subprocess
from pathlib import Path


TRIAL_COLUMNS = [
    "codec",
    "slice_number",
    "trial",
    "result_dir",
    "port",
    "send_status",
    "frames_requested",
    "frames_decoded",
    "frames_compared",
    "psnr_y",
    "psnr_u",
    "psnr_v",
    "psnr_avg",
    "vmaf",
    "frame_latency_count",
    "frame_latency_avg_ms",
    "frame_latency_max_ms",
    "frame_latency_p95_ms",
    "frame_latency_p99_ms",
    "overall_latency_count",
    "overall_latency_avg_ms",
    "overall_latency_max_ms",
    "overall_latency_p95_ms",
    "overall_latency_p99_ms",
    "packet_latency_count",
    "packet_latency_avg_ms",
    "packet_latency_max_ms",
    "packet_latency_p95_ms",
    "packet_latency_p99_ms",
    "frame_stall_100ms_count",
    "frame_stall_200ms_count",
]
TRIAL_TEXT_COLUMNS = {"codec", "slice_number", "result_dir"}
SUMMARY_TEXT_COLUMNS = {"codec", "slice_number"}
QUALITY_FRAME_COLUMNS = [
    "frame_index",
    "psnr_y",
    "psnr_u",
    "psnr_v",
    "psnr_avg",
    "vmaf",
]


def run_ffmpeg(cmd, log_path):
    result = subprocess.run(cmd, text=True, capture_output=True)
    log_path.write_text((result.stdout or "") + (result.stderr or ""))
    return result


def parse_metric(text, name):
    match = re.search(rf"{name}:((?:inf)|(?:[0-9.]+))", text)
    return match.group(1) if match else ""


def parse_number(value):
    if value is None:
        return ""
    value = str(value).strip()
    if value == "":
        return ""
    try:
        return int(value)
    except ValueError:
        pass
    try:
        return float(value)
    except ValueError:
        return value


def parse_psnr_frames(path):
    rows = []
    path = Path(path)
    if not path.exists():
        return rows

    kv_re = re.compile(r"(\w+):((?:-?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][-+]?\d+)?|inf|-inf|nan))")
    with path.open() as f:
        for line in f:
            row = {}
            for key, value in kv_re.findall(line):
                row[key] = parse_number(value)
            if "n" not in row:
                continue
            try:
                frame_index = int(row["n"]) - 1
            except (TypeError, ValueError):
                continue
            rows.append(
                {
                    "frame_index": frame_index,
                    "psnr_y": row.get("psnr_y", ""),
                    "psnr_u": row.get("psnr_u", ""),
                    "psnr_v": row.get("psnr_v", ""),
                    "psnr_avg": row.get("psnr_avg", ""),
                }
            )
    return rows


def parse_vmaf_frames(path):
    rows = []
    path = Path(path)
    if not path.exists():
        return rows

    with path.open(newline="") as f:
        reader = csv.reader(f)
        header = next(reader, None)
        if not header:
            return rows
        header = [col.strip() for col in header]
        try:
            frame_idx = header.index("Frame")
            vmaf_idx = header.index("vmaf")
        except ValueError:
            return rows
        for row in reader:
            if len(row) <= max(frame_idx, vmaf_idx):
                continue
            try:
                index = int(row[frame_idx])
            except ValueError:
                continue
            rows.append(
                {
                    "frame_index": index,
                    "vmaf": parse_number(row[vmaf_idx]),
                }
            )
    return rows


def write_quality_frames(result_dir, frames_compared):
    psnr_rows = parse_psnr_frames(result_dir / "psnr_frames.log")
    vmaf_rows = parse_vmaf_frames(result_dir / "vmaf_frames.csv")

    merged = {}
    for i in range(max(frames_compared, 0)):
        merged[i] = {
            "frame_index": i,
            "psnr_y": "",
            "psnr_u": "",
            "psnr_v": "",
            "psnr_avg": "",
            "vmaf": "",
        }

    for row in psnr_rows:
        merged.setdefault(
            row["frame_index"],
            {
                "frame_index": row["frame_index"],
                "psnr_y": "",
                "psnr_u": "",
                "psnr_v": "",
                "psnr_avg": "",
                "vmaf": "",
            },
        )
        merged[row["frame_index"]].update(row)

    for row in vmaf_rows:
        merged.setdefault(
            row["frame_index"],
            {
                "frame_index": row["frame_index"],
                "psnr_y": "",
                "psnr_u": "",
                "psnr_v": "",
                "psnr_avg": "",
                "vmaf": "",
            },
        )
        merged[row["frame_index"]].update(row)

    quality_rows = [merged[i] for i in sorted(merged)]

    quality_csv = result_dir / "quality_frames.csv"
    with quality_csv.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=QUALITY_FRAME_COLUMNS)
        writer.writeheader()
        for row in quality_rows:
            writer.writerow({key: row.get(key, "") for key in QUALITY_FRAME_COLUMNS})

    quality_log = result_dir / "quality_frames.log"
    with quality_log.open("w") as f:
        for row in quality_rows:
            f.write(
                "frame={frame_index} psnr_y={psnr_y} psnr_u={psnr_u} "
                "psnr_v={psnr_v} psnr_avg={psnr_avg} vmaf={vmaf}\n".format(
                    **{key: row.get(key, "") for key in QUALITY_FRAME_COLUMNS}
                )
            )

    return quality_rows, quality_csv, quality_log


def calc_quality(args):
    result_dir = Path(args.result_dir)
    result_dir.mkdir(parents=True, exist_ok=True)
    quality_csv = result_dir / "quality_metrics.csv"
    psnr_stats_path = result_dir / "psnr_frames.log"
    vmaf_csv_path = result_dir / "vmaf_frames.csv"

    frame_size = args.width * args.height * 3 // 2
    decoded = Path(args.decoded)
    frames_decoded = 0
    if decoded.exists() and frame_size > 0:
        frames_decoded = decoded.stat().st_size // frame_size
    frames_compared = min(args.frames, frames_decoded)

    row = {
        "frames_requested": args.frames,
        "frames_decoded": frames_decoded,
        "frames_compared": frames_compared,
        "psnr_y": "",
        "psnr_u": "",
        "psnr_v": "",
        "psnr_avg": "",
        "vmaf": "",
    }

    ffmpeg = shutil.which(args.ffmpeg)
    if not ffmpeg or frames_compared <= 0:
        with quality_csv.open("w", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=row.keys())
            writer.writeheader()
            writer.writerow(row)
        return row

    size = f"{args.width}x{args.height}"

    psnr_cmd = [
        ffmpeg,
        "-hide_banner",
        "-s", size,
        "-pix_fmt", "yuv420p",
        "-i", str(args.reference),
        "-s", size,
        "-pix_fmt", "yuv420p",
        "-i", str(decoded),
        "-frames:v", str(frames_compared),
        "-filter_complex", f"[0:v][1:v]psnr=stats_file={psnr_stats_path}",
        "-f", "null",
        "-",
    ]
    psnr_result = run_ffmpeg(psnr_cmd, result_dir / "psnr_ffmpeg.log")
    psnr_text = psnr_result.stdout + psnr_result.stderr
    row["psnr_y"] = parse_metric(psnr_text, "y")
    row["psnr_u"] = parse_metric(psnr_text, "u")
    row["psnr_v"] = parse_metric(psnr_text, "v")
    row["psnr_avg"] = parse_metric(psnr_text, "average")

    # Same ordering as x264_experiments/tests/slice_division_cost/test_slice_cost.sh:
    # distorted first, reference second.
    vmaf_cmd = [
        ffmpeg,
        "-hide_banner",
        "-s", size,
        "-pix_fmt", "yuv420p",
        "-i", str(decoded),
        "-s", size,
        "-pix_fmt", "yuv420p",
        "-i", str(args.reference),
        "-frames:v", str(frames_compared),
        "-filter_complex", f"[0:v][1:v]libvmaf=log_fmt=csv:log_path={vmaf_csv_path}",
        "-f", "null",
        "-",
    ]
    vmaf_result = run_ffmpeg(vmaf_cmd, result_dir / "vmaf_ffmpeg.log")
    vmaf_text = vmaf_result.stdout + vmaf_result.stderr
    match = re.search(r"VMAF score:\s*([0-9.]+)", vmaf_text)
    if match:
        row["vmaf"] = match.group(1)

    with quality_csv.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=row.keys())
        writer.writeheader()
        writer.writerow(row)

    quality_rows, quality_frames_csv, quality_frames_log = write_quality_frames(
        result_dir, frames_compared
    )
    print(
        f"Wrote {quality_frames_csv} and {quality_frames_log} "
        f"({len(quality_rows)} frames)"
    )
    return row


def percentile(values, pct):
    if not values:
        return ""
    values = sorted(values)
    index = math.ceil((pct / 100.0) * len(values)) - 1
    index = min(max(index, 0), len(values) - 1)
    return values[index]


def read_latency_stats(path, column):
    path = Path(path)
    values = []
    if path.exists():
        with path.open(newline="") as f:
            for row in csv.DictReader(f):
                try:
                    values.append(float(row[column]))
                except (KeyError, TypeError, ValueError):
                    pass

    if not values:
        return {"count": 0, "avg": "", "max": "", "p95": "", "p99": ""}

    return {
        "count": len(values),
        "avg": statistics.fmean(values),
        "max": max(values),
        "p95": percentile(values, 95),
        "p99": percentile(values, 99),
    }


def read_stall_count(path, column, threshold_ms):
    path = Path(path)
    count = 0
    if path.exists():
        with path.open(newline="") as f:
            for row in csv.DictReader(f):
                try:
                    value = float(row[column])
                except (KeyError, TypeError, ValueError):
                    continue
                if value > threshold_ms:
                    count += 1
    return count


def fmt(value):
    if value == "" or value is None:
        return ""
    if isinstance(value, int):
        return str(value)
    try:
        return f"{float(value):.6f}"
    except (TypeError, ValueError):
        return str(value)


def format_row(row, columns, text_columns):
    formatted = {}
    for key in columns:
        value = row.get(key, "")
        if key in text_columns:
            formatted[key] = "" if value is None else str(value)
        else:
            formatted[key] = fmt(value)
    return formatted


def append_trial_row(args):
    result_dir = Path(args.result_dir)
    quality = calc_quality(args)
    frame = read_latency_stats(result_dir / "frame_latency.csv", "frame_latency")
    overall = read_latency_stats(result_dir / "overall_latency.csv", "overall_latency")
    packet = read_latency_stats(result_dir / "packet_latency.csv", "packet_latency")
    stall_100_count = read_stall_count(
        result_dir / "frame_latency.csv", "frame_latency", 100.0
    )
    stall_200_count = read_stall_count(
        result_dir / "frame_latency.csv", "frame_latency", 200.0
    )

    row = {
        "codec": args.codec,
        "slice_number": args.slice_number,
        "trial": args.trial,
        "result_dir": str(result_dir),
        "port": args.port,
        "send_status": args.send_status,
        "frames_requested": quality["frames_requested"],
        "frames_decoded": quality["frames_decoded"],
        "frames_compared": quality["frames_compared"],
        "psnr_y": quality["psnr_y"],
        "psnr_u": quality["psnr_u"],
        "psnr_v": quality["psnr_v"],
        "psnr_avg": quality["psnr_avg"],
        "vmaf": quality["vmaf"],
        "frame_latency_count": frame["count"],
        "frame_latency_avg_ms": frame["avg"],
        "frame_latency_max_ms": frame["max"],
        "frame_latency_p95_ms": frame["p95"],
        "frame_latency_p99_ms": frame["p99"],
        "overall_latency_count": overall["count"],
        "overall_latency_avg_ms": overall["avg"],
        "overall_latency_max_ms": overall["max"],
        "overall_latency_p95_ms": overall["p95"],
        "overall_latency_p99_ms": overall["p99"],
        "packet_latency_count": packet["count"],
        "packet_latency_avg_ms": packet["avg"],
        "packet_latency_max_ms": packet["max"],
        "packet_latency_p95_ms": packet["p95"],
        "packet_latency_p99_ms": packet["p99"],
        "frame_stall_100ms_count": stall_100_count,
        "frame_stall_200ms_count": stall_200_count,
    }

    trials_csv = Path(args.trials_csv)
    trials_csv.parent.mkdir(parents=True, exist_ok=True)
    write_header = not trials_csv.exists()
    with trials_csv.open("a", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=TRIAL_COLUMNS)
        if write_header:
            writer.writeheader()
        writer.writerow(format_row(row, TRIAL_COLUMNS, TRIAL_TEXT_COLUMNS))

    print(
        f"{args.codec} slice_number={args.slice_number} trial {args.trial}: "
        f"decoded={quality['frames_decoded']} compared={quality['frames_compared']} "
        f"PSNR={quality['psnr_avg'] or 'N/A'} VMAF={quality['vmaf'] or 'N/A'} "
        f"stall100={stall_100_count} stall200={stall_200_count}"
    )


def summarize_trials(args):
    trials_csv = Path(args.summarize)
    rows = list(csv.DictReader(trials_csv.open(newline="")))
    numeric_cols = [
        "frames_decoded",
        "frames_compared",
        "psnr_y",
        "psnr_u",
        "psnr_v",
        "psnr_avg",
        "vmaf",
        "frame_latency_avg_ms",
        "frame_latency_max_ms",
        "frame_latency_p95_ms",
        "frame_latency_p99_ms",
        "overall_latency_avg_ms",
        "overall_latency_max_ms",
        "overall_latency_p95_ms",
        "overall_latency_p99_ms",
        "packet_latency_avg_ms",
        "packet_latency_max_ms",
        "packet_latency_p95_ms",
        "packet_latency_p99_ms",
        "frame_stall_100ms_count",
        "frame_stall_200ms_count",
    ]

    groups = []
    for row in rows:
        group = (row["codec"], row.get("slice_number", ""))
        if group not in groups:
            groups.append(group)

    summary_cols = ["codec", "slice_number", "trials", "successful_trials"]
    for col in numeric_cols:
        summary_cols.append(f"avg_{col}")

    summary_rows = []
    for codec, slice_number in groups:
        group = [
            row for row in rows
            if row["codec"] == codec and row.get("slice_number", "") == slice_number
        ]
        out = {
            "codec": codec,
            "slice_number": slice_number,
            "trials": len(group),
            "successful_trials": sum(1 for row in group if row.get("send_status") == "0"),
        }
        for col in numeric_cols:
            vals = []
            for row in group:
                try:
                    if row[col] != "":
                        vals.append(float(row[col]))
                except (KeyError, ValueError):
                    pass
            out[f"avg_{col}"] = statistics.fmean(vals) if vals else ""
        summary_rows.append(out)

    summary_csv = Path(args.summary_csv)
    with summary_csv.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=summary_cols)
        writer.writeheader()
        for row in summary_rows:
            writer.writerow(format_row(row, summary_cols, SUMMARY_TEXT_COLUMNS))

    print(f"Wrote {summary_csv}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--summarize", help="summarize a trials CSV")
    parser.add_argument("--summary-csv", default="summary.csv")
    parser.add_argument("--result-dir")
    parser.add_argument("--reference")
    parser.add_argument("--decoded")
    parser.add_argument("--width", type=int)
    parser.add_argument("--height", type=int)
    parser.add_argument("--frames", type=int)
    parser.add_argument("--codec")
    parser.add_argument("--slice-number", default="")
    parser.add_argument("--trial", type=int)
    parser.add_argument("--port", type=int)
    parser.add_argument("--send-status", type=int)
    parser.add_argument("--trials-csv")
    parser.add_argument("--ffmpeg", default="ffmpeg")
    args = parser.parse_args()

    if args.summarize:
        summarize_trials(args)
    else:
        required = [
            args.result_dir,
            args.reference,
            args.decoded,
            args.width,
            args.height,
            args.frames,
            args.codec,
            args.slice_number,
            args.trial,
            args.port,
            args.send_status,
            args.trials_csv,
        ]
        if any(value is None for value in required):
            parser.error("missing required trial arguments")
        append_trial_row(args)


if __name__ == "__main__":
    main()
