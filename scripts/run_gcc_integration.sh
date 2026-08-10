#!/usr/bin/env bash
# Phase 2 integration tests: run GCC under controlled bandwidth scenarios,
# record detailed logs, and plot bitrate + delay behavior.
#
# Scenarios (all step the link up/down repeatedly so we see GCC both drain a
# queue on the down-step AND re-ramp on the up-step, over several cycles):
#   1. osc_10to1  — 10<->1 Mbps, repeated (main correctness check)
#   2. step_2to1  — 2<->1 Mbps   (mild drop)
#   3. step_5to1  — 5<->1 Mbps   (moderate drop)
#   4. step_20to1 — 20<->1 Mbps  (severe 20x drop)
#   5. step_20to2 — 20<->2 Mbps  (severe drop, higher floor)
#
# Each level alternates high/low every PHASE_S seconds for CYCLES cycles, so
# the schedule is high,low,high,low,... Defaults to the real x264 encoder
# (CODEC=x264, reading INPUT_VIDEO) so CC behavior reflects a live VBR encoder
# for comparison against sparkrtc. Set CODEC=mock for the synthetic encoder.
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
PORT_BASE=6000

# Codec + input video. Default to the real x264 encoder so CC behavior reflects
# a live VBR encoder (frame sizes fluctuate, encoder undershoots target) for
# comparison against sparkrtc. Override CODEC=mock for the synthetic encoder.
CODEC="${CODEC:-x264}"
INPUT_VIDEO="${INPUT_VIDEO:-$PROJECT_ROOT/input/Lecture_concat.yuv}"
WIDTH="${WIDTH:-1920}"
HEIGHT="${HEIGHT:-1080}"

if [ "$CODEC" != "mock" ] && [ ! -f "$INPUT_VIDEO" ]; then
  echo "Input video not found: $INPUT_VIDEO (set INPUT_VIDEO=... or CODEC=mock)"
  exit 1
fi

# Each phase (one high or one low level) lasts PHASE_S seconds; CYCLES full
# high+low cycles per scenario. Override from the environment if needed.
PHASE_S="${PHASE_S:-10}"
CYCLES="${CYCLES:-3}"

# Total wall-clock length of every scenario (CYCLES full high+low cycles).
SCENARIO_DURATION_S=$((CYCLES * 2 * PHASE_S))

# build_schedule <high_kbps> <low_kbps> -> echoes "0:high,PHASE:low,2PHASE:high,..."
# (Runs in a command-substitution subshell, so it must not rely on side effects;
# the total duration is computed separately as SCENARIO_DURATION_S.)
build_schedule() {
  local high="$1" low="$2"
  local steps="" t=0 i
  local total_phases=$((CYCLES * 2))
  for ((i = 0; i < total_phases; i++)); do
    local level=$high
    (( i % 2 == 1 )) && level=$low
    [ -n "$steps" ] && steps="${steps},"
    steps="${steps}${t}:${level}"
    t=$((t + PHASE_S))
  done
  echo "$steps"
}

# run_scenario <name> <schedule> <port> <duration_s>
run_scenario() {
  local name="$1"
  local schedule="$2"
  local port="$3"
  local duration_s="$4"
  local frames=$((FPS * duration_s))
  local result_dir="$PROJECT_ROOT/result/gcc_${name}"

  echo ""
  echo "=== Scenario: ${name} (${duration_s}s, schedule='${schedule}') ==="
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

  # Receiver (write decoded to /dev/null — avoid multi-GB output for test runs).
  # Must use the same codec as the sender so it can decode the stream.
  "$BINARY" --codec="$CODEC" --fps=$FPS --port="$port" \
    --width=$WIDTH --height=$HEIGHT \
    --feedback_max_interval_ms=30 \
    --file=/dev/null > "$result_dir/recv.log" 2>&1 &
  recv_pid=$!
  sleep 1

  # Sender with bandwidth schedule. Start GCC high so it must converge down.
  # With a real codec the encoder reads INPUT_VIDEO (loops if shorter than the
  # scenario) and tracks the GCC target via SetTargetBitrate.
  local input_flag=""
  [ "$CODEC" != "mock" ] && input_flag="--input_video_file=$INPUT_VIDEO"
  set +e
  "$BINARY" --codec="$CODEC" --fps=$FPS --port="$port" --ip=127.0.0.1 \
    --width=$WIDTH --height=$HEIGHT \
    $input_flag \
    --frames_to_encode=$frames \
    --sim_delay_ms=0 \
    --sim_bandwidth_schedule="$schedule" \
    --encoder_variable_mode=0 \
    --cc_cwnd_queue_size_ms=100 \
    --cc_cwnd_min_bitrate_kbps=100 \
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

  # Per-phase summary: for each PHASE_S window, report the link level, the
  # target GCC converged to by end of phase, and the peak queuing delay. This
  # shows whether GCC tracks each up/down step and drains the queue each cycle.
  if [ -f "$result_dir/gcc_state.csv" ]; then
    echo "  --- per-phase convergence (phase=${PHASE_S}s) ---"
    PHASE_S="$PHASE_S" "$PYTHON" - "$result_dir/gcc_state.csv" <<'PY' | sed 's/^/    /'
import csv, os, sys
phase = float(os.environ["PHASE_S"])
rows = list(csv.DictReader(open(sys.argv[1])))
if not rows:
    print("(no samples)"); sys.exit()
end = float(rows[-1]["t_sec"])
n = int(end // phase) + 1
print(f"{'phase':>5} {'t_range':>12} {'end_target':>10} {'peak_qd_ms':>10} {'end_qd_ms':>9} {'peak_rtt':>8}")
for p in range(n):
    lo, hi = p*phase, (p+1)*phase
    seg = [r for r in rows if lo <= float(r["t_sec"]) < hi]
    if not seg: continue
    qd = [float(r["queuing_delay_ms"]) for r in seg]
    rtt = [float(r["rtt_ms"]) for r in seg]
    tgt = int(seg[-1]["target"])
    print(f"{p:>5} {f'{lo:.0f}-{hi:.0f}s':>12} {tgt:>10} {max(qd):>10.0f} {qd[-1]:>9.0f} {max(rtt):>8.0f}")
PY
  fi
}

# Main correctness check: 10<->1 Mbps oscillating over several cycles.
run_scenario "osc_10to1" "$(build_schedule 10000 1000)" $((PORT_BASE + 0)) "$SCENARIO_DURATION_S"

# Different drop levels, each oscillating high<->low.
run_scenario "step_2to1"  "$(build_schedule 2000 1000)"  $((PORT_BASE + 2)) "$SCENARIO_DURATION_S"
run_scenario "step_5to1"  "$(build_schedule 5000 1000)"  $((PORT_BASE + 4)) "$SCENARIO_DURATION_S"
run_scenario "step_20to1" "$(build_schedule 20000 1000)" $((PORT_BASE + 6)) "$SCENARIO_DURATION_S"
run_scenario "step_20to2" "$(build_schedule 20000 2000)" $((PORT_BASE + 8)) "$SCENARIO_DURATION_S"

echo ""
echo "=== All scenarios complete ==="
echo "Figures: result/gcc_*/figs/gcc_behavior.png"
