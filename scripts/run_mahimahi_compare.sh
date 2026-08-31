#!/usr/bin/env bash
# Run repeated mahimahi trials through ../run.sh and summarize codec results.
#
# Defaults:
#   CODECS="x264 x264_slice"
#   TRIALS=5
#   FRAMES=800
#   FPS=30
#   TRACE_FILE=input/traces/15s_10to1_until_300s.log
#   INPUT_VIDEO=/home/menghua/Research/VideoResources/Lecture.yuv
#   FEEDBACK_MAX_INTERVAL_MS=1
#   FEEDBACK_TRACE_INTERVAL_MS=1
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
RUN_ONE="$PROJECT_ROOT/run.sh"
METRICS="$SCRIPT_DIR/collect_x264_trial_metrics.py"

PYTHON="${PYTHON:-python3}"
CODECS_STR="${CODECS:-x264 x264_slice}"
TRIALS="${TRIALS:-5}"
FRAMES="${FRAMES:-800}"
FPS="${FPS:-30}"
TRACE_FILE="${TRACE_FILE:-$PROJECT_ROOT/input/traces/15s_10to1_until_300s.log}"
VIDEO_DIR="${VIDEO_DIR:-/home/menghua/Research/VideoResources}"
INPUT_VIDEO="${INPUT_VIDEO:-$VIDEO_DIR/Lecture.yuv}"
WIDTH="${WIDTH:-1920}"
HEIGHT="${HEIGHT:-1080}"
BASE_PORT="${BASE_PORT:-8800}"
PORT_STRIDE="${PORT_STRIDE:-100}"
FEEDBACK_MAX_INTERVAL_MS="${FEEDBACK_MAX_INTERVAL_MS:-1}"
FEEDBACK_TRACE_INTERVAL_MS="${FEEDBACK_TRACE_INTERVAL_MS:-1}"
FEEDBACK_BANDWIDTH_FILE="${FEEDBACK_BANDWIDTH_FILE:-}"
CC_CWND_QUEUE_SIZE_MS="${CC_CWND_QUEUE_SIZE_MS:-50}"

timestamp="$(date +%Y%m%d_%H%M%S)"
RESULT_ROOT="${RESULT_ROOT:-$PROJECT_ROOT/result/mahimahi_compare_$timestamp}"
TRIALS_CSV="$RESULT_ROOT/trials.csv"
SUMMARY_CSV="$RESULT_ROOT/summary.csv"

if [ ! -x "$RUN_ONE" ]; then
  echo "Missing executable: $RUN_ONE"
  exit 1
fi
if [ ! -f "$TRACE_FILE" ]; then
  echo "Mahimahi trace not found: $TRACE_FILE"
  exit 1
fi
if [ ! -f "$INPUT_VIDEO" ]; then
  echo "Input YUV video not found: $INPUT_VIDEO"
  exit 1
fi

read -r -a CODECS <<< "$CODECS_STR"

if ! [[ "$TRIALS" =~ ^[1-9][0-9]*$ ]]; then
  echo "TRIALS must be a positive integer"
  exit 1
fi
for codec in "${CODECS[@]}"; do
  if [ "$codec" != "x264" ] && [ "$codec" != "x264_slice" ]; then
    echo "Unsupported comparison codec: $codec (expected x264 or x264_slice)"
    exit 1
  fi
done

mkdir -p "$RESULT_ROOT"
rm -f "$TRIALS_CSV" "$SUMMARY_CSV"

echo "=== Mahimahi codec comparison ==="
echo "Result root: $RESULT_ROOT"
echo "Codecs: ${CODECS[*]}"
echo "Trials per codec: $TRIALS"
echo "Input: $INPUT_VIDEO"
echo "Trace: $TRACE_FILE"
echo "Feedback batching: ${FEEDBACK_MAX_INTERVAL_MS} ms"
echo "Congestion-window queue allowance: ${CC_CWND_QUEUE_SIZE_MS} ms"
if [ -n "$FEEDBACK_BANDWIDTH_FILE" ]; then
  echo "Feedback trace: $FEEDBACK_BANDWIDTH_FILE"
else
  echo "Feedback trace: generated ${FEEDBACK_TRACE_INTERVAL_MS} ms reverse slots"
fi
echo "Resolution/FPS: ${WIDTH}x${HEIGHT} @ ${FPS}"
echo "Frames: $FRAMES"
echo ""

# Interleave codecs within each trial number. This keeps paired x264 and
# x264_slice runs close in time instead of putting all baseline runs first.
for trial in $(seq 1 "$TRIALS"); do
  codec_index=0
  for codec in "${CODECS[@]}"; do
    port=$((BASE_PORT + codec_index * PORT_STRIDE + trial * 2))
    prefix="$codec/trial_$trial"

    echo "----------------------------------------"
    echo "Codec=$codec trial=$trial/$TRIALS port=$port"

    env \
      WIDTH="$WIDTH" \
      HEIGHT="$HEIGHT" \
      PORT="$port" \
      RESULT_BASE="$RESULT_ROOT" \
      TRIAL_ID="$trial" \
      TRIALS_CSV="$TRIALS_CSV" \
      PYTHON="$PYTHON" \
      FFMPEG_BIN="${FFMPEG_BIN:-ffmpeg}" \
      FEEDBACK_MAX_INTERVAL_MS="$FEEDBACK_MAX_INTERVAL_MS" \
      FEEDBACK_TRACE_INTERVAL_MS="$FEEDBACK_TRACE_INTERVAL_MS" \
      FEEDBACK_BANDWIDTH_FILE="$FEEDBACK_BANDWIDTH_FILE" \
      CC_CWND_QUEUE_SIZE_MS="$CC_CWND_QUEUE_SIZE_MS" \
      "$RUN_ONE" "$prefix" "$codec" "" "$FRAMES" "$TRACE_FILE" "$FPS" "$INPUT_VIDEO"
    codec_index=$((codec_index + 1))
  done
done

"$PYTHON" "$METRICS" \
  --summarize "$TRIALS_CSV" \
  --summary-csv "$SUMMARY_CSV"

echo ""
echo "=== Comparison complete ==="
echo "Trials CSV: $TRIALS_CSV"
echo "Summary CSV: $SUMMARY_CSV"
