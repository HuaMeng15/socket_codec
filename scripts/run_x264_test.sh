#!/usr/bin/env bash
# Focused test script for x264 encoder adaptive logic.
# Only tests the 10Mbps -> 1Mbps capacity drop scenario (single run, no repeat).
#
# Quick codec switching between frame-level and slice-level control:
#   CODEC=x264        ./scripts/run_x264_test.sh   # frame-level control (default)
#   CODEC=x264_slice  ./scripts/run_x264_test.sh   # slice-level control (per-slice QP)
#
# Other quick overrides:
#   PHASE1_SECONDS=8       # change drop timing (default 5s)
#   CC_INITIAL_BITRATE_KBPS=500   # startup bitrate (default 500)
#   CC_MAX_BITRATE_KBPS=10000     # ceiling (default 10000)
#   FRAMES=600             # encode duration (default 600 = 20s @ 30fps)
#   RUN_NAME=my_test       # result directory name
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BINARY="$PROJECT_ROOT/build/socket_codec"

# Prefer the project venv python if present (has matplotlib/numpy)
if [ -x "$PROJECT_ROOT/.venv/bin/python" ]; then
  PYTHON="$PROJECT_ROOT/.venv/bin/python"
else
  PYTHON="python3"
fi

if [ ! -f "$BINARY" ]; then
  echo "Binary not found. Run scripts/build.sh first."
  exit 1
fi

FPS=30
PORT="${PORT:-6100}"

# Use real x264 encoder with actual video unless overridden.
CODEC="${CODEC:-x264_slice}"
INPUT_VIDEO="${INPUT_VIDEO:-$PROJECT_ROOT/input/Lecture_concat.yuv}"
WIDTH="${WIDTH:-1920}"
HEIGHT="${HEIGHT:-1080}"
CC_INITIAL_BITRATE_KBPS="${CC_INITIAL_BITRATE_KBPS:-500}"
CC_MAX_BITRATE_KBPS="${CC_MAX_BITRATE_KBPS:-10000}"
FEEDBACK_MAX_INTERVAL_MS="${FEEDBACK_MAX_INTERVAL_MS:-10}"
RESULT_TAG="${RESULT_TAG:-$CODEC}"

if [ ! -f "$INPUT_VIDEO" ]; then
  echo "Input video not found: $INPUT_VIDEO"
  exit 1
fi

# Single phase: 10s at 10Mbps, then 10s at 1Mbps
PHASE_S=10
TOTAL_DURATION_S=20
FRAMES=$((FPS * TOTAL_DURATION_S))
SCHEDULE="0:10000,${PHASE_S}:1000"

RESULT_DIR="$PROJECT_ROOT/result/x264_test_10to1_${RESULT_TAG}"

echo "=== X264 Encoder Adaptive Test: 10Mbps -> 1Mbps ==="
echo "Duration: ${TOTAL_DURATION_S}s (${PHASE_S}s per phase)"
echo "Schedule: ${SCHEDULE}"
echo "Codec: ${CODEC} $([ "$CODEC" = "x264_slice" ] && echo "(slice-level QP control)" || echo "(frame-level control)")"
echo "Initial bitrate: ${CC_INITIAL_BITRATE_KBPS} kbps"
echo "Max bitrate: ${CC_MAX_BITRATE_KBPS} kbps"
echo "Feedback interval: ${FEEDBACK_MAX_INTERVAL_MS} ms"
echo ""

rm -rf "$RESULT_DIR"
mkdir -p "$RESULT_DIR"

recv_pid=""
cleanup() {
  if [ -n "$recv_pid" ]; then
    kill "$recv_pid" 2>/dev/null || true
    wait "$recv_pid" 2>/dev/null || true
  fi
}
trap cleanup EXIT

# Receiver
"$BINARY" --codec="$CODEC" --fps=$FPS --port="$PORT" \
  --width=$WIDTH --height=$HEIGHT \
  --feedback_max_interval_ms="$FEEDBACK_MAX_INTERVAL_MS" \
  --file=/dev/null > "$RESULT_DIR/recv.log" 2>&1 &
recv_pid=$!
sleep 1

# Sender with 10->1 Mbps schedule
set +e
"$BINARY" --codec="$CODEC" --fps=$FPS --port="$PORT" --ip=127.0.0.1 \
  --width=$WIDTH --height=$HEIGHT \
  --input_video_file=$INPUT_VIDEO \
  --frames_to_encode=$FRAMES \
  --sim_delay_ms=0 \
  --sim_bandwidth_schedule="$SCHEDULE" \
  --encoder_variable_mode=0 \
  --cc_initial_bitrate_kbps="$CC_INITIAL_BITRATE_KBPS" \
  --cc_max_bitrate_kbps="$CC_MAX_BITRATE_KBPS" \
  --cc_cwnd_queue_size_ms=100 \
  --cc_cwnd_min_bitrate_kbps=100 \
  > "$RESULT_DIR/send.log" 2>&1
send_status=$?
set -e

sleep 1
cleanup
trap - EXIT

echo "Sender exit=$send_status"
echo "Logs: $RESULT_DIR/send.log $RESULT_DIR/recv.log"
echo ""

# Analyze + plot
"$PYTHON" "$SCRIPT_DIR/plot_gcc.py" "$RESULT_DIR" || true
"$PYTHON" "$SCRIPT_DIR/calc_latency.py" "$RESULT_DIR" 2>/dev/null || true
"$PYTHON" "$SCRIPT_DIR/draw.py" "$RESULT_DIR" 2>/dev/null || true

echo ""
echo "=== Test complete ==="
echo "Figure: $RESULT_DIR/figs/gcc_behavior.png"
