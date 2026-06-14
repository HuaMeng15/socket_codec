#!/usr/bin/env bash
# Run sender + receiver locally on localhost (no mahimahi needed).
# Usage: scripts/run_local.sh [codec] [frames] [fps]
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BINARY="$PROJECT_ROOT/build/socket_codec"

CODEC="${1:-mock}"
FRAMES="${2:-300}"
FPS="${3:-30}"
PORT="${4:-5000}"
RESULT_DIR="$PROJECT_ROOT/result/local_${CODEC}"

if [ ! -f "$BINARY" ]; then
  echo "Binary not found. Run scripts/build.sh first."
  exit 1
fi

rm -rf "$RESULT_DIR"
mkdir -p "$RESULT_DIR"

echo "=== Starting receiver (codec=$CODEC, port=$PORT) ==="
"$BINARY" --codec="$CODEC" --fps="$FPS" --port="$PORT" \
  --file="$RESULT_DIR/rec.yuv" > "$RESULT_DIR/recv.log" 2>&1 &
RECV_PID=$!

# Give receiver time to bind
sleep 1

echo "=== Starting sender (codec=$CODEC, frames=$FRAMES, fps=$FPS) ==="
"$BINARY" --codec="$CODEC" --fps="$FPS" --port="$PORT" \
  --ip=127.0.0.1 --frames_to_encode="$FRAMES" > "$RESULT_DIR/send.log" 2>&1
SEND_STATUS=$?

# Cleanup receiver
sleep 1
kill "$RECV_PID" 2>/dev/null || true
wait "$RECV_PID" 2>/dev/null || true

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
