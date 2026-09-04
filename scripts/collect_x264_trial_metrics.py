#!/usr/bin/env python3
import argparse
import csv
import json
import math
import re
import shutil
import statistics
import subprocess
from decimal import Decimal, ROUND_CEILING
from pathlib import Path


TRIAL_COLUMNS = [
    "experiment_mode",
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
    "frame_latency_p999_ms",
    "overall_latency_count",
    "overall_latency_avg_ms",
    "overall_latency_max_ms",
    "overall_latency_p95_ms",
    "overall_latency_p99_ms",
    "overall_latency_p999_ms",
    "overall_stall_100ms_count",
    "overall_stall_200ms_count",
    "packet_latency_count",
    "packet_latency_avg_ms",
    "packet_latency_max_ms",
    "packet_latency_p95_ms",
    "packet_latency_p99_ms",
    "frame_stall_100ms_count",
    "frame_stall_200ms_count",
]
TRIAL_TEXT_COLUMNS = {"experiment_mode", "codec", "slice_number", "result_dir"}
TRIAL_INTEGER_COLUMNS = {
    "trial",
    "port",
    "send_status",
    "frames_requested",
    "frames_decoded",
    "frames_compared",
    "frame_latency_count",
    "overall_latency_count",
    "overall_stall_100ms_count",
    "overall_stall_200ms_count",
    "packet_latency_count",
    "frame_stall_100ms_count",
    "frame_stall_200ms_count",
}
SUMMARY_TEXT_COLUMNS = {"experiment_mode", "codec", "slice_number"}
QUALITY_FRAME_COLUMNS = [
    "frame_index",
    "psnr_y",
    "psnr_u",
    "psnr_v",
    "psnr_avg",
    "vmaf",
]
DECODED_FRAME_RE = re.compile(r"Successfully decoded frame (\d+)")


def run_ffmpeg(cmd, log_path):
    result = subprocess.run(cmd, text=True, capture_output=True)
    log_path.write_text((result.stdout or "") + (result.stderr or ""))
    return result


def ffmpeg_supports_libvmaf(ffmpeg):
    result = subprocess.run(
        [ffmpeg, "-hide_banner", "-h", "filter=libvmaf"],
        text=True,
        capture_output=True,
    )
    text = (result.stdout or "") + (result.stderr or "")
    return "Unknown filter" not in text


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


def parse_easyvmaf_json(path):
    path = Path(path)
    if not path.exists():
        return "", []
    data = json.loads(path.read_text())
    frames = []
    for frame in data.get("frames", []):
        metrics = frame.get("metrics", {})
        if "vmaf" not in metrics:
            continue
        frames.append(
            {
                "frame_index": int(frame.get("frameNum", len(frames))),
                "vmaf": parse_number(metrics["vmaf"]),
            }
        )
    mean = data.get("pooled_metrics", {}).get("vmaf", {}).get("mean", "")
    return parse_number(mean), frames


def write_vmaf_frames_csv(rows, path):
    with Path(path).open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=["Frame", "vmaf"])
        writer.writeheader()
        for row in rows:
            writer.writerow(
                {
                    "Frame": row.get("frame_index", ""),
                    "vmaf": row.get("vmaf", ""),
                }
            )


def raw_yuv_input_args(raw_path, width, height, fps, loop_count=1):
    args = [
        "-f", "rawvideo",
        "-pix_fmt", "yuv420p",
        "-s", f"{width}x{height}",
        "-r", str(fps),
    ]
    if loop_count > 1:
        # -stream_loop is an input option and rewinds the raw YUV demuxer
        # without materializing a multi-gigabyte repeated reference file.
        args.extend(["-stream_loop", str(loop_count - 1)])
    args.extend(["-i", str(raw_path)])
    return args


def read_decoded_frame_ids(result_dir, frames_compared):
    """Return decoded source frame IDs when recv.log can map every raw frame."""
    recv_log = Path(result_dir) / "recv.log"
    if not recv_log.exists() or frames_compared <= 0:
        return [], "unavailable"

    frame_ids = []
    with recv_log.open(errors="replace") as handle:
        for line in handle:
            match = DECODED_FRAME_RE.search(line)
            if match:
                frame_ids.append(int(match.group(1)))

    if len(frame_ids) < frames_compared:
        return frame_ids, "incomplete"
    frame_ids = frame_ids[:frames_compared]
    if any(right <= left for left, right in zip(frame_ids, frame_ids[1:])):
        return frame_ids, "non_monotonic"
    return frame_ids, "mapped"


def select_expression(frame_ids):
    """Compress increasing frame IDs into an FFmpeg select expression."""
    if not frame_ids:
        return ""
    spans = []
    start = previous = frame_ids[0]
    for frame_id in frame_ids[1:]:
        if frame_id == previous + 1:
            previous = frame_id
            continue
        spans.append((start, previous))
        start = previous = frame_id
    spans.append((start, previous))

    terms = []
    for start, end in spans:
        if start == end:
            terms.append(f"eq(n\\,{start})")
        else:
            terms.append(f"between(n\\,{start}\\,{end})")
    return "+".join(terms)


def comparison_filter(frame_ids, fps, metric_filter, reference_input_first):
    """Align the repeated reference to the decoded source IDs before scoring."""
    expression = select_expression(frame_ids)
    if not expression:
        return ""
    reference_input = 0 if reference_input_first else 1
    distorted_input = 1 if reference_input_first else 0
    metric_inputs = "[ref][dist]" if reference_input_first else "[dist][ref]"
    return (
        f"[{reference_input}:v]select='{expression}',"
        f"setpts=N/({fps}*TB)[ref];"
        f"[{distorted_input}:v]setpts=N/({fps}*TB)[dist];"
        f"{metric_inputs}{metric_filter}"
    )


def raw_yuv_to_mp4(
    ffmpeg,
    raw_path,
    mp4_path,
    width,
    height,
    fps,
    frames,
    log_path,
    loop_count=1,
    frame_ids=None,
):
    cmd = [
        ffmpeg,
        "-hide_banner",
        *raw_yuv_input_args(raw_path, width, height, fps, loop_count),
    ]
    if frame_ids:
        cmd.extend(
            [
                "-vf",
                f"select='{select_expression(frame_ids)}',setpts=N/({fps}*TB)",
            ]
        )
    cmd.extend(
        [
            "-frames:v", str(frames),
            "-c:v", "libx264",
            "-preset", "ultrafast",
            "-crf", "0",
            "-pix_fmt", "yuv420p",
            "-y",
            str(mp4_path),
        ]
    )
    return run_ffmpeg(cmd, log_path)


def run_docker_vmaf(
    args, ffmpeg, result_dir, frames_compared, decoded_frame_ids=None
):
    if frames_compared <= 0:
        return "", []
    docker = shutil.which(args.docker_bin)
    if not docker:
        (result_dir / "vmaf_docker.log").write_text("docker not found\n")
        return "", []

    work_dir = result_dir / "vmaf_docker"
    work_dir.mkdir(parents=True, exist_ok=True)
    ref_mp4 = work_dir / "reference.mp4"
    dist_mp4 = work_dir / "distorted.mp4"

    ref_result = raw_yuv_to_mp4(
        ffmpeg,
        args.reference,
        ref_mp4,
        args.width,
        args.height,
        args.fps,
        frames_compared,
        result_dir / "vmaf_reference_mp4_ffmpeg.log",
        loop_count=args.reference_loop_count,
        frame_ids=decoded_frame_ids,
    )
    dist_result = raw_yuv_to_mp4(
        ffmpeg,
        args.decoded,
        dist_mp4,
        args.width,
        args.height,
        args.fps,
        frames_compared,
        result_dir / "vmaf_distorted_mp4_ffmpeg.log",
    )
    if ref_result.returncode != 0 or dist_result.returncode != 0:
        if not args.keep_vmaf_workdir:
            shutil.rmtree(work_dir, ignore_errors=True)
        return "", []

    cmd = [
        docker,
        "run",
        "--rm",
        "-v",
        f"{work_dir.resolve()}:/socket",
        args.vmaf_docker_image,
        "-r",
        "/socket/reference.mp4",
        "-d",
        "/socket/distorted.mp4",
        "-endsync",
    ]
    result = subprocess.run(cmd, text=True, capture_output=True)
    (result_dir / "vmaf_docker.log").write_text(
        (result.stdout or "") + (result.stderr or "")
    )
    if result.returncode != 0:
        if not args.keep_vmaf_workdir:
            shutil.rmtree(work_dir, ignore_errors=True)
        return "", []

    json_path = work_dir / "distorted_vmaf.json"
    mean, frame_rows = parse_easyvmaf_json(json_path)
    if frame_rows:
        write_vmaf_frames_csv(frame_rows, result_dir / "vmaf_frames.csv")
        (result_dir / "vmaf_docker.json").write_text(json_path.read_text())
    if not args.keep_vmaf_workdir:
        shutil.rmtree(work_dir, ignore_errors=True)
    return mean, frame_rows


def write_quality_frames(result_dir, frames_compared, decoded_frame_ids=None):
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
    if decoded_frame_ids and len(decoded_frame_ids) >= len(quality_rows):
        for sequence_index, row in enumerate(quality_rows):
            row["frame_index"] = decoded_frame_ids[sequence_index]

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
    reference = Path(args.reference)
    reference_frames = 0
    if reference.exists() and frame_size > 0:
        reference_frames = reference.stat().st_size // frame_size
    available_reference_frames = reference_frames * args.reference_loop_count
    frames_compared = min(args.frames, frames_decoded, available_reference_frames)
    decoded_frame_ids, alignment_status = read_decoded_frame_ids(
        result_dir, frames_compared
    )
    can_map_frame_ids = (
        alignment_status == "mapped"
        and len(decoded_frame_ids) == frames_compared
        and all(0 <= frame_id < available_reference_frames for frame_id in decoded_frame_ids)
    )
    needs_reference_alignment = can_map_frame_ids and decoded_frame_ids != list(
        range(frames_compared)
    )
    alignment_metadata = {
        "status": alignment_status,
        "frames_compared": frames_compared,
        "mapped_frame_count": len(decoded_frame_ids),
        "reference_alignment_applied": needs_reference_alignment,
        "first_decoded_frame_id": decoded_frame_ids[0] if decoded_frame_ids else None,
        "last_decoded_frame_id": decoded_frame_ids[-1] if decoded_frame_ids else None,
    }
    (result_dir / "quality_alignment.json").write_text(
        json.dumps(alignment_metadata, indent=2, sort_keys=True) + "\n"
    )

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

    psnr_filter = f"[0:v][1:v]psnr=stats_file={psnr_stats_path}"
    if needs_reference_alignment:
        psnr_filter = comparison_filter(
            decoded_frame_ids,
            args.fps,
            f"psnr=stats_file={psnr_stats_path}",
            reference_input_first=True,
        )
    psnr_cmd = [
        ffmpeg,
        "-hide_banner",
        *raw_yuv_input_args(
            args.reference,
            args.width,
            args.height,
            args.fps,
            args.reference_loop_count,
        ),
        *raw_yuv_input_args(decoded, args.width, args.height, args.fps),
        "-frames:v", str(frames_compared),
        "-filter_complex", psnr_filter,
        "-f", "null",
        "-",
    ]
    psnr_result = run_ffmpeg(psnr_cmd, result_dir / "psnr_ffmpeg.log")
    psnr_text = psnr_result.stdout + psnr_result.stderr
    row["psnr_y"] = parse_metric(psnr_text, "y")
    row["psnr_u"] = parse_metric(psnr_text, "u")
    row["psnr_v"] = parse_metric(psnr_text, "v")
    row["psnr_avg"] = parse_metric(psnr_text, "average")

    use_ffmpeg_vmaf = args.vmaf_method in ("auto", "ffmpeg")
    use_docker_vmaf = args.vmaf_method in ("auto", "docker")
    if use_ffmpeg_vmaf and ffmpeg_supports_libvmaf(ffmpeg):
        # Same ordering as x264_experiments/tests/slice_division_cost/test_slice_cost.sh:
        # distorted first, reference second.
        vmaf_filter = f"[0:v][1:v]libvmaf=log_fmt=csv:log_path={vmaf_csv_path}"
        if needs_reference_alignment:
            vmaf_filter = comparison_filter(
                decoded_frame_ids,
                args.fps,
                f"libvmaf=log_fmt=csv:log_path={vmaf_csv_path}",
                reference_input_first=False,
            )
        vmaf_cmd = [
            ffmpeg,
            "-hide_banner",
            *raw_yuv_input_args(decoded, args.width, args.height, args.fps),
            *raw_yuv_input_args(
                args.reference,
                args.width,
                args.height,
                args.fps,
                args.reference_loop_count,
            ),
            "-frames:v", str(frames_compared),
            "-filter_complex", vmaf_filter,
            "-f", "null",
            "-",
        ]
        vmaf_result = run_ffmpeg(vmaf_cmd, result_dir / "vmaf_ffmpeg.log")
        vmaf_text = vmaf_result.stdout + vmaf_result.stderr
        match = re.search(r"VMAF score:\s*([0-9.]+)", vmaf_text)
        if match:
            row["vmaf"] = match.group(1)
    elif use_ffmpeg_vmaf:
        (result_dir / "vmaf_ffmpeg.log").write_text(
            f"{ffmpeg} does not provide filter=libvmaf\n"
        )

    if not row["vmaf"] and use_docker_vmaf:
        docker_mean, _ = run_docker_vmaf(
            args,
            ffmpeg,
            result_dir,
            frames_compared,
            decoded_frame_ids if needs_reference_alignment else None,
        )
        if docker_mean != "":
            row["vmaf"] = docker_mean

    with quality_csv.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=row.keys())
        writer.writeheader()
        writer.writerow(row)

    quality_rows, quality_frames_csv, quality_frames_log = write_quality_frames(
        result_dir,
        frames_compared,
        decoded_frame_ids if can_map_frame_ids else None,
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
    rank = int(
        (Decimal(str(pct)) * len(values) / Decimal(100)).to_integral_value(
            rounding=ROUND_CEILING
        )
    )
    index = rank - 1
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
        return {
            "count": 0,
            "avg": "",
            "max": "",
            "p95": "",
            "p99": "",
            "p999": "",
        }

    return {
        "count": len(values),
        "avg": statistics.fmean(values),
        "max": max(values),
        "p95": percentile(values, 95),
        "p99": percentile(values, 99),
        "p999": percentile(values, 99.9),
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


def resolve_existing_trial_dir(row, trials_csv):
    result_dir = Path(row.get("result_dir", ""))
    required_files = (
        "frame_latency.csv",
        "overall_latency.csv",
        "packet_latency.csv",
        "quality_frames.csv",
    )
    if all((result_dir / name).is_file() for name in required_files):
        return result_dir.resolve()

    experiment_mode = row.get("experiment_mode", row.get("codec", ""))
    trial = row.get("trial", "")
    relocated_dir = Path(trials_csv).parent / experiment_mode / f"trial_{trial}"
    if not all((relocated_dir / name).is_file() for name in required_files):
        raise RuntimeError(
            f"cannot locate metric data for {experiment_mode}/trial_{trial}: "
            f"checked {result_dir} and {relocated_dir}"
        )
    return relocated_dir.resolve()


def upgrade_trials_csv_schema(trials_csv):
    """Backfill new latency percentiles in an older trials CSV atomically."""
    trials_csv = Path(trials_csv)
    if not trials_csv.is_file():
        return False

    with trials_csv.open(newline="") as f:
        reader = csv.DictReader(f)
        existing_columns = list(reader.fieldnames or [])
        rows = list(reader)

    if existing_columns == TRIAL_COLUMNS:
        return False

    unknown_columns = [col for col in existing_columns if col not in TRIAL_COLUMNS]
    missing_columns = [col for col in TRIAL_COLUMNS if col not in existing_columns]
    supported_missing = {
        "frame_latency_p999_ms",
        "overall_latency_p999_ms",
    }
    if unknown_columns or not set(missing_columns).issubset(supported_missing):
        raise RuntimeError(
            f"cannot safely upgrade {trials_csv}: "
            f"unknown_columns={unknown_columns}, missing_columns={missing_columns}"
        )

    for row in rows:
        result_dir = resolve_existing_trial_dir(row, trials_csv)
        if row.get("result_dir") != str(result_dir):
            row["result_dir"] = str(result_dir)
        if "frame_latency_p999_ms" in missing_columns:
            frame = read_latency_stats(
                result_dir / "frame_latency.csv", "frame_latency"
            )
            row["frame_latency_p999_ms"] = fmt(frame["p999"])
        if "overall_latency_p999_ms" in missing_columns:
            overall = read_latency_stats(
                result_dir / "overall_latency.csv", "overall_latency"
            )
            row["overall_latency_p999_ms"] = fmt(overall["p999"])

    temporary = trials_csv.with_name(f".{trials_csv.name}.schema-upgrade.tmp")
    with temporary.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=TRIAL_COLUMNS)
        writer.writeheader()
        for row in rows:
            writer.writerow({column: row.get(column, "") for column in TRIAL_COLUMNS})
    temporary.replace(trials_csv)
    print(
        f"Upgraded {trials_csv} with "
        "frame_latency_p999_ms and overall_latency_p999_ms"
    )
    return True


def read_rows_after_frame(path, minimum_frame_index):
    path = Path(path)
    if not path.is_file():
        raise RuntimeError(f"missing metric source: {path}")
    rows = []
    with path.open(newline="") as f:
        reader = csv.DictReader(f)
        if "frame_index" not in (reader.fieldnames or []):
            raise RuntimeError(f"missing frame_index column in {path}")
        for line_number, row in enumerate(reader, start=2):
            try:
                frame_index = int(row["frame_index"])
            except (KeyError, TypeError, ValueError) as error:
                raise RuntimeError(
                    f"invalid frame_index at {path}:{line_number}"
                ) from error
            if frame_index >= minimum_frame_index:
                rows.append(row)
    if not rows:
        raise RuntimeError(
            f"no samples remain in {path} after frame_index >= {minimum_frame_index}"
        )
    return rows


def numeric_values(rows, column, source):
    values = []
    for index, row in enumerate(rows, start=2):
        try:
            value = float(row[column])
        except (KeyError, TypeError, ValueError) as error:
            raise RuntimeError(f"invalid {column} at {source}:{index}") from error
        if not math.isfinite(value):
            raise RuntimeError(f"non-finite {column} at {source}:{index}")
        values.append(value)
    return values


def latency_stats_from_values(values):
    return {
        "count": len(values),
        "avg": statistics.fmean(values),
        "max": max(values),
        "p95": percentile(values, 95),
        "p99": percentile(values, 99),
        "p999": percentile(values, 99.9),
    }


def recalculate_trials(args):
    trials_csv = Path(args.recalculate_trials_csv)
    upgrade_trials_csv_schema(trials_csv)
    with trials_csv.open(newline="") as f:
        reader = csv.DictReader(f)
        if list(reader.fieldnames or []) != TRIAL_COLUMNS:
            raise RuntimeError(f"unexpected trials CSV schema: {trials_csv}")
        rows = list(reader)

    minimum_frame_index = math.ceil(args.trim_start_seconds * args.fps)
    for row in rows:
        for column in TRIAL_INTEGER_COLUMNS:
            value = row.get(column, "")
            if value != "":
                row[column] = int(float(value))
        result_dir = resolve_existing_trial_dir(row, trials_csv)
        if not (result_dir / "VALIDATED").is_file():
            raise RuntimeError(f"trial is not validated: {result_dir}")

        frame_rows = read_rows_after_frame(
            result_dir / "frame_latency.csv", minimum_frame_index
        )
        overall_rows = read_rows_after_frame(
            result_dir / "overall_latency.csv", minimum_frame_index
        )
        packet_rows = read_rows_after_frame(
            result_dir / "packet_latency.csv", minimum_frame_index
        )
        quality_rows = read_rows_after_frame(
            result_dir / "quality_frames.csv", minimum_frame_index
        )

        frame_indices = {int(item["frame_index"]) for item in frame_rows}
        overall_indices = {int(item["frame_index"]) for item in overall_rows}
        quality_indices = {int(item["frame_index"]) for item in quality_rows}
        if frame_indices != overall_indices or frame_indices != quality_indices:
            raise RuntimeError(
                f"trimmed frame indices do not align across latency and quality: "
                f"{result_dir}"
            )

        frame_values = numeric_values(
            frame_rows, "frame_latency", result_dir / "frame_latency.csv"
        )
        overall_values = numeric_values(
            overall_rows, "overall_latency", result_dir / "overall_latency.csv"
        )
        packet_values = numeric_values(
            packet_rows, "packet_latency", result_dir / "packet_latency.csv"
        )
        frame = latency_stats_from_values(frame_values)
        overall = latency_stats_from_values(overall_values)
        packet = latency_stats_from_values(packet_values)

        row.update(
            {
                "result_dir": str(result_dir),
                "frames_decoded": len(quality_rows),
                "frames_compared": len(quality_rows),
                "psnr_y": statistics.fmean(
                    numeric_values(quality_rows, "psnr_y", result_dir / "quality_frames.csv")
                ),
                "psnr_u": statistics.fmean(
                    numeric_values(quality_rows, "psnr_u", result_dir / "quality_frames.csv")
                ),
                "psnr_v": statistics.fmean(
                    numeric_values(quality_rows, "psnr_v", result_dir / "quality_frames.csv")
                ),
                "psnr_avg": statistics.fmean(
                    numeric_values(
                        quality_rows, "psnr_avg", result_dir / "quality_frames.csv"
                    )
                ),
                "vmaf": statistics.fmean(
                    numeric_values(quality_rows, "vmaf", result_dir / "quality_frames.csv")
                ),
                "frame_latency_count": frame["count"],
                "frame_latency_avg_ms": frame["avg"],
                "frame_latency_max_ms": frame["max"],
                "frame_latency_p95_ms": frame["p95"],
                "frame_latency_p99_ms": frame["p99"],
                "frame_latency_p999_ms": frame["p999"],
                "overall_latency_count": overall["count"],
                "overall_latency_avg_ms": overall["avg"],
                "overall_latency_max_ms": overall["max"],
                "overall_latency_p95_ms": overall["p95"],
                "overall_latency_p99_ms": overall["p99"],
                "overall_latency_p999_ms": overall["p999"],
                "overall_stall_100ms_count": sum(
                    value > 100.0 for value in overall_values
                ),
                "overall_stall_200ms_count": sum(
                    value > 200.0 for value in overall_values
                ),
                "packet_latency_count": packet["count"],
                "packet_latency_avg_ms": packet["avg"],
                "packet_latency_max_ms": packet["max"],
                "packet_latency_p95_ms": packet["p95"],
                "packet_latency_p99_ms": packet["p99"],
                "frame_stall_100ms_count": sum(
                    value > 100.0 for value in frame_values
                ),
                "frame_stall_200ms_count": sum(
                    value > 200.0 for value in frame_values
                ),
            }
        )

    temporary = trials_csv.with_name(f".{trials_csv.name}.recalculate.tmp")
    with temporary.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=TRIAL_COLUMNS)
        writer.writeheader()
        for row in rows:
            writer.writerow(format_row(row, TRIAL_COLUMNS, TRIAL_TEXT_COLUMNS))
    temporary.replace(trials_csv)
    print(
        f"Recalculated {trials_csv} after excluding frames before "
        f"{minimum_frame_index} ({args.trim_start_seconds:g} s at {args.fps} fps)"
    )


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
    overall_stall_100_count = read_stall_count(
        result_dir / "overall_latency.csv", "overall_latency", 100.0
    )
    overall_stall_200_count = read_stall_count(
        result_dir / "overall_latency.csv", "overall_latency", 200.0
    )

    row = {
        "experiment_mode": args.experiment_mode,
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
        "frame_latency_p999_ms": frame["p999"],
        "overall_latency_count": overall["count"],
        "overall_latency_avg_ms": overall["avg"],
        "overall_latency_max_ms": overall["max"],
        "overall_latency_p95_ms": overall["p95"],
        "overall_latency_p99_ms": overall["p99"],
        "overall_latency_p999_ms": overall["p999"],
        "overall_stall_100ms_count": overall_stall_100_count,
        "overall_stall_200ms_count": overall_stall_200_count,
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
    upgrade_trials_csv_schema(trials_csv)
    write_header = not trials_csv.exists()
    with trials_csv.open("a", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=TRIAL_COLUMNS)
        if write_header:
            writer.writeheader()
        writer.writerow(format_row(row, TRIAL_COLUMNS, TRIAL_TEXT_COLUMNS))

    print(
        f"{args.experiment_mode} codec={args.codec} "
        f"slice_number={args.slice_number} trial {args.trial}: "
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
        "frame_latency_p999_ms",
        "overall_latency_avg_ms",
        "overall_latency_max_ms",
        "overall_latency_p95_ms",
        "overall_latency_p99_ms",
        "overall_latency_p999_ms",
        "overall_stall_100ms_count",
        "overall_stall_200ms_count",
        "packet_latency_avg_ms",
        "packet_latency_max_ms",
        "packet_latency_p95_ms",
        "packet_latency_p99_ms",
        "frame_stall_100ms_count",
        "frame_stall_200ms_count",
    ]

    groups = []
    for row in rows:
        group = (
            row.get("experiment_mode", row["codec"]),
            row["codec"],
            row.get("slice_number", ""),
        )
        if group not in groups:
            groups.append(group)

    summary_cols = [
        "experiment_mode",
        "codec",
        "slice_number",
        "trials",
        "successful_trials",
    ]
    for col in numeric_cols:
        summary_cols.append(f"avg_{col}")

    summary_rows = []
    for experiment_mode, codec, slice_number in groups:
        group = [
            row for row in rows
            if row.get("experiment_mode", row["codec"]) == experiment_mode
            and row["codec"] == codec
            and row.get("slice_number", "") == slice_number
        ]
        out = {
            "experiment_mode": experiment_mode,
            "codec": codec,
            "slice_number": slice_number,
            "trials": len(group),
            "successful_trials": sum(
                1
                for row in group
                if float(row.get("send_status", "nan")) == 0.0
            ),
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
    parser.add_argument(
        "--upgrade-trials-schema",
        help="atomically backfill P99.9 columns in an existing trials CSV",
    )
    parser.add_argument(
        "--recalculate-trials-csv",
        help="recalculate an existing trials CSV from its per-frame metric files",
    )
    parser.add_argument(
        "--trim-start-seconds",
        type=float,
        default=0.0,
        help="exclude frames before this media-time offset when recalculating",
    )
    parser.add_argument("--summary-csv", default="summary.csv")
    parser.add_argument("--result-dir")
    parser.add_argument("--reference")
    parser.add_argument(
        "--reference-loop-count",
        type=int,
        default=1,
        help="Number of consecutive passes through the raw reference YUV",
    )
    parser.add_argument("--decoded")
    parser.add_argument("--width", type=int)
    parser.add_argument("--height", type=int)
    parser.add_argument("--frames", type=int)
    parser.add_argument("--fps", type=int, default=30)
    parser.add_argument("--codec")
    parser.add_argument("--experiment-mode", default="auto")
    parser.add_argument("--slice-number", default="")
    parser.add_argument("--trial", type=int)
    parser.add_argument("--port", type=int)
    parser.add_argument("--send-status", type=int)
    parser.add_argument("--trials-csv")
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument(
        "--vmaf-method",
        choices=["auto", "ffmpeg", "docker", "none"],
        default="auto",
        help="VMAF backend: auto tries FFmpeg libvmaf, then Docker easyvmaf",
    )
    parser.add_argument("--docker-bin", default="docker")
    parser.add_argument("--vmaf-docker-image", default="gfdavila/easyvmaf")
    parser.add_argument(
        "--keep-vmaf-workdir",
        action="store_true",
        help="Keep temporary Docker VMAF MP4 inputs for debugging",
    )
    args = parser.parse_args()

    if args.reference_loop_count < 1:
        parser.error("--reference-loop-count must be at least 1")
    if args.fps < 1:
        parser.error("--fps must be at least 1")
    if args.trim_start_seconds < 0:
        parser.error("--trim-start-seconds must be non-negative")

    if args.recalculate_trials_csv:
        recalculate_trials(args)
    elif args.upgrade_trials_schema:
        upgrade_trials_csv_schema(args.upgrade_trials_schema)
    elif args.summarize:
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
