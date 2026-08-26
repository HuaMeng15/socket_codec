#!/usr/bin/env bash
# Compare baseline x264 against adaptive x264_slice across repeated runs.
#
# Defaults:
#   CODECS="x264 x264_slice"
#   TRIALS=5
#   RESULT_ROOT=result/x264_compare_10to1_<timestamp>
#   RUN_PLOTS=1  # per-trial plots are expensive/noisy; set 0 to skip all figures
#
# Common overrides are forwarded to run_x264_test.sh:
#   INPUT_VIDEO WIDTH HEIGHT FPS FRAMES PHASE_S TOTAL_DURATION_S SCHEDULE
#   CC_INITIAL_BITRATE_KBPS CC_MAX_BITRATE_KBPS FEEDBACK_MAX_INTERVAL_MS

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
RUN_ONE="$SCRIPT_DIR/run_x264_test.sh"
METRICS="$SCRIPT_DIR/collect_x264_trial_metrics.py"

if [ -x "$PROJECT_ROOT/.venv/bin/python" ]; then
  PYTHON="$PROJECT_ROOT/.venv/bin/python"
else
  PYTHON="python3"
fi

if [ ! -x "$RUN_ONE" ]; then
  echo "Missing executable: $RUN_ONE"
  exit 1
fi

TRIALS="${TRIALS:-5}"
CODECS_STR="${CODECS:-x264 x264_slice}"
BASE_PORT="${BASE_PORT:-6300}"
PORT_STRIDE="${PORT_STRIDE:-20}"
RUN_PLOTS="${RUN_PLOTS:-1}"
if [ -z "${FFMPEG_BIN:-}" ]; then
  if [ -x "/opt/homebrew/bin/ffmpeg" ]; then
    FFMPEG_BIN="/opt/homebrew/bin/ffmpeg"
  else
    FFMPEG_BIN="ffmpeg"
  fi
fi

FPS="${FPS:-30}"
VIDEO_DIR="${VIDEO_DIR:-/home/menghua/Research/VideoResources}"
INPUT_VIDEO="${INPUT_VIDEO:-$VIDEO_DIR/Lecture.yuv}"
WIDTH="${WIDTH:-1920}"
HEIGHT="${HEIGHT:-1080}"
CC_INITIAL_BITRATE_KBPS="${CC_INITIAL_BITRATE_KBPS:-500}"
CC_MAX_BITRATE_KBPS="${CC_MAX_BITRATE_KBPS:-10000}"
FEEDBACK_MAX_INTERVAL_MS="${FEEDBACK_MAX_INTERVAL_MS:-10}"
PHASE_S="${PHASE_S:-10}"
FRAMES="${FRAMES:-800}"
TOTAL_DURATION_S="${TOTAL_DURATION_S:-$(((FRAMES + FPS - 1) / FPS))}"
SCHEDULE="${SCHEDULE:-0:10000,${PHASE_S}:1000}"

timestamp="$(date +%Y%m%d_%H%M%S)"
RESULT_ROOT="${RESULT_ROOT:-$PROJECT_ROOT/result/x264_compare_10to1_$timestamp}"
TRIALS_CSV="$RESULT_ROOT/trials.csv"
SUMMARY_CSV="$RESULT_ROOT/summary.csv"

read -r -a CODECS <<< "$CODECS_STR"

mkdir -p "$RESULT_ROOT"
rm -f "$TRIALS_CSV" "$SUMMARY_CSV"

echo "=== x264 vs x264_slice repeated comparison ==="
echo "Result root: $RESULT_ROOT"
echo "Codecs: ${CODECS[*]}"
echo "Trials per codec: $TRIALS"
echo "Input: $INPUT_VIDEO"
echo "Resolution/FPS: ${WIDTH}x${HEIGHT} @ ${FPS}"
echo "Frames: $FRAMES"
echo "Schedule: $SCHEDULE"
echo ""

codec_index=0
for codec in "${CODECS[@]}"; do
  if [ "$codec" = "x264" ]; then
    slice_number="1"
  else
    slice_number="adaptive_1_4_9"
  fi

  for trial in $(seq 1 "$TRIALS"); do
    port=$((BASE_PORT + codec_index * PORT_STRIDE + trial * 2))
    result_dir="$RESULT_ROOT/$codec/trial_$trial"
    decoded_yuv="$result_dir/decoded.yuv"

    echo "----------------------------------------"
    echo "Codec=$codec slice_number=$slice_number trial=$trial/$TRIALS port=$port"

    env \
      CODEC="$codec" \
      RESULT_TAG="${codec}_trial_${trial}" \
      RESULT_DIR="$result_dir" \
      RECEIVER_OUTPUT_FILE="$decoded_yuv" \
      PORT="$port" \
      INPUT_VIDEO="$INPUT_VIDEO" \
      WIDTH="$WIDTH" \
      HEIGHT="$HEIGHT" \
      FPS="$FPS" \
      FRAMES="$FRAMES" \
      PHASE_S="$PHASE_S" \
      TOTAL_DURATION_S="$TOTAL_DURATION_S" \
      SCHEDULE="$SCHEDULE" \
      CC_INITIAL_BITRATE_KBPS="$CC_INITIAL_BITRATE_KBPS" \
      CC_MAX_BITRATE_KBPS="$CC_MAX_BITRATE_KBPS" \
      FEEDBACK_MAX_INTERVAL_MS="$FEEDBACK_MAX_INTERVAL_MS" \
      RUN_LATENCY=1 \
      RUN_PLOTS=0 \
      "$RUN_ONE"

    send_status="1"
    if [ -f "$result_dir/send_status.txt" ]; then
      send_status="$(tr -d '[:space:]' < "$result_dir/send_status.txt")"
    fi

    "$PYTHON" "$METRICS" \
      --result-dir "$result_dir" \
      --reference "$INPUT_VIDEO" \
      --decoded "$decoded_yuv" \
      --width "$WIDTH" \
      --height "$HEIGHT" \
      --frames "$FRAMES" \
      --codec "$codec" \
      --slice-number "$slice_number" \
      --trial "$trial" \
      --port "$port" \
      --send-status "$send_status" \
      --trials-csv "$TRIALS_CSV" \
      --ffmpeg "$FFMPEG_BIN"

    if [ "$RUN_PLOTS" != "0" ]; then
      "$PYTHON" "$SCRIPT_DIR/plot_gcc.py" "$result_dir" || true
      "$PYTHON" "$SCRIPT_DIR/draw.py" "$result_dir" || true
    fi
  done

  codec_index=$((codec_index + 1))
done

"$PYTHON" "$METRICS" \
  --summarize "$TRIALS_CSV" \
  --summary-csv "$SUMMARY_CSV"

echo ""
echo "=== Comparison complete ==="
echo "Trials CSV: $TRIALS_CSV"
echo "Summary CSV: $SUMMARY_CSV"
