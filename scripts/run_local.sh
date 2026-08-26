#!/usr/bin/env bash
# Run sender + receiver locally on localhost (no mahimahi needed).
# Usage: scripts/run_local.sh [codec] [frames] [fps] [port] [extra_sender_flags...]
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BINARY="$PROJECT_ROOT/build/socket_codec"

CODEC="${1:-mock}"
FRAMES="${2:-800}"
FPS="${3:-30}"
PORT="${4:-5000}"
shift 4 2>/dev/null || true  # remaining args passed to sender
RESULT_DIR="$PROJECT_ROOT/result/local_${CODEC}"

if [ ! -f "$BINARY" ]; then
  echo "Binary not found. Run scripts/build.sh first."
  exit 1
fi

rm -rf "$RESULT_DIR"
mkdir -p "$RESULT_DIR"

RECV_PID=""
cleanup() {
  if [ -n "$RECV_PID" ]; then
    kill "$RECV_PID" 2>/dev/null || true
    wait "$RECV_PID" 2>/dev/null || true
  fi
}
trap cleanup EXIT

echo "=== Starting receiver (codec=$CODEC, port=$PORT) ==="
"$BINARY" --codec="$CODEC" --fps="$FPS" --port="$PORT" \
  --file="$RESULT_DIR/rec.yuv" > "$RESULT_DIR/recv.log" 2>&1 &
RECV_PID=$!

# Give receiver time to bind
sleep 1

echo "=== Starting sender (codec=$CODEC, frames=$FRAMES, fps=$FPS) ==="
set +e
"$BINARY" --codec="$CODEC" --fps="$FPS" --port="$PORT" \
  --ip=127.0.0.1 --frames_to_encode="$FRAMES" "$@" > "$RESULT_DIR/send.log" 2>&1
SEND_STATUS=$?
set -e

sleep 1

echo ""
echo "=== Done (sender exit=$SEND_STATUS) ==="
echo "Logs: $RESULT_DIR/send.log  $RESULT_DIR/recv.log"

# Run analysis scripts if available
if command -v python3 >/dev/null 2>&1; then
  for script in calc_latency.py extract_framesize_rate.py draw.py; do
    if [ -f "$PROJECT_ROOT/scripts/$script" ]; then
      python3 "$PROJECT_ROOT/scripts/$script" "$RESULT_DIR" 2>/dev/null || true
    fi
  done
fi
