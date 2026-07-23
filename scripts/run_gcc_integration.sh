#!/usr/bin/env bash
# Phase 2 integration tests: run GCC under controlled bandwidth scenarios,
# record detailed logs, and plot bitrate + delay behavior.
#
# Scenarios:
#   1. static_10mbps  — constant 10 Mbps link, verify GCC converges near cap
#   2. drop_10to1     — 10 Mbps for 10s then drop to 1 Mbps, verify reaction
#   3. static_1mbps   — constant 1 Mbps link, verify stable low-rate operation
#
# Uses the mock codec (zero-filled frames sized exactly by target bitrate) so
# the achieved send rate tracks the encoder target precisely.
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
DURATION_S="${DURATION_S:-25}"
FRAMES=$((FPS * DURATION_S))
PORT_BASE=6000

# run_scenario <name> <schedule> <port>
run_scenario() {
  local name="$1"
  local schedule="$2"
  local port="$3"
  local result_dir="$PROJECT_ROOT/result/gcc_${name}"

  echo ""
  echo "=== Scenario: ${name} (schedule='${schedule}') ==="
  rm -rf "$result_dir"
  mkdir -p "$result_dir"

  local recv_pid=""
  cleanup() {
    if [ -n "$recv_pid" ]; then
      kill "$recv_pid" 2>/dev/null || true
      wait "$recv_pid" 2>/dev/null || true
    fi
  }
  trap cleanup RETURN

  # Receiver (write decoded to /dev/null — avoid multi-GB output for test runs)
  "$BINARY" --codec=mock --fps=$FPS --port="$port" \
    --file=/dev/null > "$result_dir/recv.log" 2>&1 &
  recv_pid=$!
  sleep 1

  # Sender with bandwidth schedule. Start GCC high so it must converge down.
  # SENDER_EXTRA_FLAGS lets callers append flags (e.g. encoder_variable_mode=1
  # and cc_pace_multiplier_x100=250 to exercise the ALR probing path).
  set +e
  "$BINARY" --codec=mock --fps=$FPS --port="$port" --ip=127.0.0.1 \
    --frames_to_encode=$FRAMES \
    --sim_delay_ms=0 \
    --sim_bandwidth_schedule="$schedule" \
    --encoder_variable_mode=0 \
    > "$result_dir/send.log" 2>&1
  local send_status=$?
  set -e

  sleep 1
  cleanup
  trap - RETURN

  echo "  sender exit=$send_status"
  echo "  logs: $result_dir/send.log $result_dir/recv.log"

  # Analyze + plot
  "$PYTHON" "$SCRIPT_DIR/plot_gcc.py" "$result_dir" || true
  "$PYTHON" "$SCRIPT_DIR/calc_latency.py" "$result_dir" 2>/dev/null || true
  "$PYTHON" "$SCRIPT_DIR/draw.py" "$result_dir" 2>/dev/null || true

  # Quick summary from GCC state CSV
  if [ -f "$result_dir/gcc_state.csv" ]; then
    echo "  --- GCC summary (last 5 samples) ---"
    tail -5 "$result_dir/gcc_state.csv" | sed 's/^/    /'
  fi
}

# Scenario 1: static 10 Mbps
run_scenario "static_10mbps" "0:10000" $((PORT_BASE + 0))

# Scenario 2: 10 Mbps -> 1 Mbps drop at t=10s
run_scenario "drop_10to1" "0:10000,10:1000" $((PORT_BASE + 2))

# # Scenario 3: static 1 Mbps
run_scenario "static_1mbps" "0:1000" $((PORT_BASE + 4))

echo ""
echo "=== All scenarios complete ==="
echo "Figures: result/gcc_*/figs/gcc_behavior.png"
