#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$SCRIPT_DIR"
BINARY="$PROJECT_ROOT/build/socket_codec"
PYTHON="${PYTHON:-python3}"
FFMPEG_BIN="${FFMPEG_BIN:-ffmpeg}"

PREFIX="${1:-mahimahi}"
codec="${2:-x264_slice}"
experiment_mode="${EXPERIMENT_MODE:-auto}"
periodic_alr_probing="${PERIODIC_ALR_PROBING:--1}"
ip="${3:-}"
frames_to_encode="${4:-800}"
bandwidth_file="${5:-$PROJECT_ROOT/input/traces/15s_10to1_until_300s.log}"
fps="${6:-30}"
input_video_file="${7:-/home/menghua/Research/VideoResources/Lecture.yuv}"
width="${WIDTH:-1920}"
height="${HEIGHT:-1080}"
port="${PORT:-8888}"
feedback_max_interval_ms="${FEEDBACK_MAX_INTERVAL_MS:-1}"
feedback_bandwidth_file="${FEEDBACK_BANDWIDTH_FILE:-}"
feedback_trace_interval_ms="${FEEDBACK_TRACE_INTERVAL_MS:-1}"
cc_cwnd_queue_size_ms="${CC_CWND_QUEUE_SIZE_MS:-50}"
reference_loop_count="${REFERENCE_LOOP_COUNT:-1}"
strict_run_validation="${STRICT_RUN_VALIDATION:-0}"
min_decoded_ratio="${MIN_DECODED_RATIO:-0.9}"
archive_decoded_video="${ARCHIVE_DECODED_VIDEO:-1}"
link_drain_ms="${LINK_DRAIN_MS:-0}"
max_send_duration_ms="${MAX_SEND_DURATION_MS:-0}"
trial_id="${TRIAL_ID:-1}"
trials_csv="${TRIALS_CSV:-}"
result_base="${RESULT_BASE:-$PROJECT_ROOT/result}"
sender_output_file="${SENDER_OUTPUT_FILE:-}"
receiver_io_check="${RECEIVER_IO_CHECK:-1}"
receiver_io_lag_threshold_ms="${RECEIVER_IO_LAG_THRESHOLD_MS:-1000}"
receiver_io_min_lagged_packets="${RECEIVER_IO_MIN_LAGGED_PACKETS:-50}"
receiver_io_min_lagged_fraction="${RECEIVER_IO_MIN_LAGGED_FRACTION:-0.001}"
receiver_io_max_retries="${RECEIVER_IO_MAX_RETRIES:-5}"
receiver_io_retry_attempt="${RECEIVER_IO_RETRY_ATTEMPT:-0}"
receiver_io_retry_cooldown_sec="${RECEIVER_IO_RETRY_COOLDOWN_SEC:-30}"
sender_max_retries="${SENDER_MAX_RETRIES:-3}"
sender_retry_attempt="${SENDER_RETRY_ATTEMPT:-0}"
sender_retry_cooldown_sec="${SENDER_RETRY_COOLDOWN_SEC:-30}"

case "$experiment_mode" in
  auto)
    experiment_label="$codec"
    ;;
  default|salsify|cbr|webrtc_no_mae|webrtc_disable_mae)
    codec="x264"
    experiment_label="$experiment_mode"
    if [ "$experiment_mode" = "webrtc_disable_mae" ]; then
      experiment_mode="webrtc_no_mae"
      experiment_label="$experiment_mode"
    fi
    ;;
  x264_slice)
    codec="x264_slice"
    experiment_label="$experiment_mode"
    ;;
  *)
    echo "EXPERIMENT_MODE must be auto, default, x264_slice, salsify, cbr, or webrtc_no_mae"
    exit 1
    ;;
esac
if [ "$periodic_alr_probing" != "-1" ] &&
   [ "$periodic_alr_probing" != "0" ] &&
   [ "$periodic_alr_probing" != "1" ]; then
  echo "PERIODIC_ALR_PROBING must be -1, 0, or 1"
  exit 1
fi

RESULT_DIR="${RESULT_DIR:-${result_base}/${PREFIX}_${experiment_label}}"
if [ -z "$trials_csv" ]; then
  trials_csv="${RESULT_DIR}/trials.csv"
fi
RECV_LOG="${RESULT_DIR}/recv.log"
SEND_LOG="${RESULT_DIR}/send.log"
MMLINK_IP_FILE="${RESULT_DIR}/mmlink_ip"
MAHIMAHI_BASE_FILE="${RESULT_DIR}/mahimahi_base"

if [ ! -x "$BINARY" ]; then
  echo "Binary not found or not executable: $BINARY"
  echo "Run scripts/build.sh first."
  exit 1
fi
if ! command -v mm-link >/dev/null 2>&1; then
  echo "mahimahi not found. Install it first, then rerun this script."
  exit 1
fi
if [ ! -f "$bandwidth_file" ]; then
  echo "Mahimahi trace not found: $bandwidth_file"
  exit 1
fi
if [ -n "$feedback_bandwidth_file" ] && [ ! -f "$feedback_bandwidth_file" ]; then
  echo "Mahimahi feedback trace not found: $feedback_bandwidth_file"
  exit 1
fi
if ! [[ "$feedback_trace_interval_ms" =~ ^[1-9][0-9]*$ ]]; then
  echo "FEEDBACK_TRACE_INTERVAL_MS must be a positive integer"
  exit 1
fi
if ! [[ "$reference_loop_count" =~ ^[1-9][0-9]*$ ]]; then
  echo "REFERENCE_LOOP_COUNT must be a positive integer"
  exit 1
fi
if [ "$strict_run_validation" != "0" ] && [ "$strict_run_validation" != "1" ]; then
  echo "STRICT_RUN_VALIDATION must be 0 or 1"
  exit 1
fi
if [ "$archive_decoded_video" != "0" ] && [ "$archive_decoded_video" != "1" ]; then
  echo "ARCHIVE_DECODED_VIDEO must be 0 or 1"
  exit 1
fi
if [ "$receiver_io_check" != "0" ] && [ "$receiver_io_check" != "1" ]; then
  echo "RECEIVER_IO_CHECK must be 0 or 1"
  exit 1
fi
if ! [[ "$receiver_io_min_lagged_packets" =~ ^[1-9][0-9]*$ ]]; then
  echo "RECEIVER_IO_MIN_LAGGED_PACKETS must be a positive integer"
  exit 1
fi
if ! awk -v value="$receiver_io_lag_threshold_ms" \
  'BEGIN { exit !(value ~ /^[0-9]+([.][0-9]+)?$/ && value > 0) }'; then
  echo "RECEIVER_IO_LAG_THRESHOLD_MS must be a positive number"
  exit 1
fi
if ! awk -v value="$receiver_io_min_lagged_fraction" \
  'BEGIN { exit !(value ~ /^[0-9]+([.][0-9]+)?$/ && value > 0 && value <= 1) }'; then
  echo "RECEIVER_IO_MIN_LAGGED_FRACTION must be in (0, 1]"
  exit 1
fi
if ! [[ "$receiver_io_max_retries" =~ ^[0-9]+$ ]]; then
  echo "RECEIVER_IO_MAX_RETRIES must be a non-negative integer"
  exit 1
fi
if ! [[ "$receiver_io_retry_attempt" =~ ^[0-9]+$ ]]; then
  echo "RECEIVER_IO_RETRY_ATTEMPT must be a non-negative integer"
  exit 1
fi
if ! [[ "$receiver_io_retry_cooldown_sec" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
  echo "RECEIVER_IO_RETRY_COOLDOWN_SEC must be a non-negative number"
  exit 1
fi
if ! [[ "$sender_max_retries" =~ ^[0-9]+$ ]]; then
  echo "SENDER_MAX_RETRIES must be a non-negative integer"
  exit 1
fi
if ! [[ "$sender_retry_attempt" =~ ^[0-9]+$ ]]; then
  echo "SENDER_RETRY_ATTEMPT must be a non-negative integer"
  exit 1
fi
if ! [[ "$sender_retry_cooldown_sec" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
  echo "SENDER_RETRY_COOLDOWN_SEC must be a non-negative number"
  exit 1
fi
if ! [[ "$link_drain_ms" =~ ^[0-9]+$ ]]; then
  echo "LINK_DRAIN_MS must be a non-negative integer"
  exit 1
fi
if ! [[ "$max_send_duration_ms" =~ ^[0-9]+$ ]]; then
  echo "MAX_SEND_DURATION_MS must be a non-negative integer"
  exit 1
fi
if [ "$max_send_duration_ms" -gt 0 ] && ! command -v timeout >/dev/null 2>&1; then
  echo "MAX_SEND_DURATION_MS requires the timeout command"
  exit 1
fi
link_drain_seconds="$(awk -v milliseconds="$link_drain_ms" \
  'BEGIN { printf "%.3f", milliseconds / 1000.0 }')"
# Bound the whole mm-link process as well as validating timestamps afterward.
# The allowance covers sender initialization and the requested post-send drain;
# it does not permit a second pass through the network trace.
mm_link_timeout_seconds="$(awk \
  -v media_ms="$max_send_duration_ms" \
  -v drain_ms="$link_drain_ms" \
  'BEGIN {
     if (media_ms <= 0) print 0;
     else print int((media_ms + drain_ms + 5000 + 999) / 1000);
   }')"
if [ ! -f "$input_video_file" ]; then
  echo "Input YUV video not found: $input_video_file"
  exit 1
fi

if [ -d "${RESULT_DIR}" ]; then
  rm -rf "${RESULT_DIR}"
fi
mkdir -p "${RESULT_DIR}"

# The sender-to-receiver media path uses the requested bandwidth trace. Keep
# receiver-to-sender transport feedback independent from that bottleneck by
# default: one reverse delivery opportunity per millisecond is about 12 Mbps
# for Mahimahi's 1500-byte packet slots and is far above the feedback load.
# A caller can supply FEEDBACK_BANDWIDTH_FILE to emulate a constrained reverse
# path, or change FEEDBACK_TRACE_INTERVAL_MS to control the generated trace.
if [ -z "$feedback_bandwidth_file" ]; then
  feedback_bandwidth_file="${RESULT_DIR}/feedback_reverse_${feedback_trace_interval_ms}ms.log"
  trace_end_ms="$(tail -n 1 "$bandwidth_file")"
  if ! [[ "$trace_end_ms" =~ ^[0-9]+$ ]]; then
    echo "Invalid final timestamp in Mahimahi trace: $bandwidth_file"
    exit 1
  fi
  awk -v end_ms="$trace_end_ms" -v step_ms="$feedback_trace_interval_ms" \
    'BEGIN { for (timestamp = 0; timestamp <= end_ms; timestamp += step_ms) print timestamp }' \
    > "$feedback_bandwidth_file"
fi

RECV_PID=""
cleanup() {
  if [ -n "$RECV_PID" ]; then
    kill "$RECV_PID" 2>/dev/null || true
    wait "$RECV_PID" 2>/dev/null || true
  fi
}
trap cleanup EXIT

# Helper script run inside mm-link: use MAHIMAHI_BASE as the host receiver IP,
# matching the sparkrtc experiment's mm-link shell pattern.
SENDER_WRAPPER="${RESULT_DIR}/.run_sender.sh"
cat > "${SENDER_WRAPPER}" << SENDER_EOF
#!/bin/bash
set -euo pipefail
MMLINK_IP=\$(ip -4 addr show ingress 2>/dev/null | awk '/inet /{print \$2}' | cut -d/ -f1)
[ -z "\$MMLINK_IP" ] && MMLINK_IP=\$(ip -4 addr show 2>/dev/null | awk '/inet /{print \$2}' | cut -d/ -f1 | grep -v '^127\.' | head -1)
echo "\$MMLINK_IP" > ${MMLINK_IP_FILE}
echo "\${MAHIMAHI_BASE:-}" > ${MAHIMAHI_BASE_FILE}
RECEIVER_IP="${ip}"
if [ -z "\$RECEIVER_IP" ]; then
  RECEIVER_IP="\${MAHIMAHI_BASE:?MAHIMAHI_BASE is not set inside mm-link}"
fi
SENDER_ARGS=(
  --frames_to_encode=${frames_to_encode}
  --ip="\$RECEIVER_IP"
  --port=${port}
  --codec=${codec}
  --experiment_mode=${experiment_mode}
  --periodic_alr_probing=${periodic_alr_probing}
  --fps=${fps}
  --width=${width}
  --height=${height}
  --cc_cwnd_queue_size_ms=${cc_cwnd_queue_size_ms}
  --input_video_file="${input_video_file}"
)
if [ -n "${sender_output_file}" ]; then
  SENDER_ARGS+=(--output_video_file="${sender_output_file}")
fi
set +e
${BINARY} "\${SENDER_ARGS[@]}" > ${SEND_LOG} 2>&1
SENDER_STATUS=\$?
set -e
# Keep the Mahimahi namespace alive briefly after the pacer has flushed so
# packets already queued in the emulated link can reach the host receiver.
if [ ${link_drain_ms} -gt 0 ]; then
  sleep ${link_drain_seconds}
fi
exit \$SENDER_STATUS
SENDER_EOF
chmod +x "${SENDER_WRAPPER}"

# Run receiver on the host side. Traffic from the mm-link sender to
# MAHIMAHI_BASE crosses the emulated link without host route injection.
"$BINARY" --file="${RESULT_DIR}/rec.yuv" \
  --codec="${codec}" \
  --fps="${fps}" \
  --port="${port}" \
  --width="${width}" \
  --height="${height}" \
  --feedback_max_interval_ms="${feedback_max_interval_ms}" \
  > "${RECV_LOG}" 2>&1 &
RECV_PID=$!

sleep 2

# Run sender inside Mahimahi. Shape sender-to-receiver media with the experiment
# trace, while the receiver-to-sender feedback path uses its independent trace.
set +e
if [ "$mm_link_timeout_seconds" -gt 0 ]; then
  timeout --signal=TERM --kill-after=5s "${mm_link_timeout_seconds}s" \
    mm-link "${bandwidth_file}" "${feedback_bandwidth_file}" -- \
    bash "${SENDER_WRAPPER}"
else
  mm-link "${bandwidth_file}" "${feedback_bandwidth_file}" -- \
    bash "${SENDER_WRAPPER}"
fi
SEND_STATUS=$?
set -e
cleanup
trap - EXIT
rm -f "${SENDER_WRAPPER}"

echo "Sender exit: ${SEND_STATUS}"
if [ "$SEND_STATUS" -eq 124 ]; then
  echo "Sender exceeded the ${mm_link_timeout_seconds}s one-pass wall deadline"
fi
echo "Logs: ${RECV_LOG} ${SEND_LOG}"

if [ "$strict_run_validation" = "1" ] && [ "$SEND_STATUS" -ne 0 ]; then
  next_sender_retry_attempt=$((sender_retry_attempt + 1))
  sender_retry_archive_dir="${result_base}/sender_retries/${PREFIX//\//_}_${experiment_label}_attempt_${next_sender_retry_attempt}"
  mkdir -p "$sender_retry_archive_dir"
  cp "$SEND_LOG" "$sender_retry_archive_dir/send.log"
  cp "$RECV_LOG" "$sender_retry_archive_dir/recv.log"
  {
    echo "sender_status=$SEND_STATUS"
    echo "attempt=$next_sender_retry_attempt"
    echo "time=$(date --iso-8601=seconds)"
  } > "$sender_retry_archive_dir/diagnostic.env"

  if [ "$next_sender_retry_attempt" -gt "$sender_max_retries" ]; then
    echo "Strict validation: sender failed after ${sender_max_retries} retries; skipping analysis"
    echo "Last diagnostic: $sender_retry_archive_dir"
    exit "$SEND_STATUS"
  fi

  rm -rf "$RESULT_DIR"
  echo "Deleted sender-failed attempt before analysis"
  echo "Diagnostic: $sender_retry_archive_dir"
  echo "Retrying ${PREFIX}_${codec}: sender attempt ${next_sender_retry_attempt}/${sender_max_retries} after ${sender_retry_cooldown_sec}s"
  sleep "$sender_retry_cooldown_sec"
  exec env SENDER_RETRY_ATTEMPT="$next_sender_retry_attempt" \
    "$0" "$@"
fi

# Detect receiver-side userspace/I/O stalls before VMAF and PSNR read the
# roughly 25 GB raw decode. An invalid attempt has not appended trials.csv yet,
# so it can be deleted and rerun without contaminating paired statistics.
if [ "$receiver_io_check" = "1" ]; then
  RECEIVER_IO_HEALTH_JSON="${RESULT_DIR}/receiver_io_health.json"
  set +e
  "$PYTHON" "$PROJECT_ROOT/scripts/check_receiver_io_health.py" \
    --recv-log "$RECV_LOG" \
    --output-json "$RECEIVER_IO_HEALTH_JSON" \
    --lag-threshold-ms "$receiver_io_lag_threshold_ms" \
    --min-lagged-packets "$receiver_io_min_lagged_packets" \
    --min-lagged-fraction "$receiver_io_min_lagged_fraction"
  RECEIVER_IO_STATUS=$?
  set -e

  if [ "$RECEIVER_IO_STATUS" -eq 42 ]; then
    next_retry_attempt=$((receiver_io_retry_attempt + 1))
    retry_archive_dir="${result_base}/receiver_io_retries"
    mkdir -p "$retry_archive_dir"
    retry_record="${retry_archive_dir}/${PREFIX//\//_}_${experiment_label}_attempt_${next_retry_attempt}.json"
    cp "$RECEIVER_IO_HEALTH_JSON" "$retry_record"
    rm -rf "$RESULT_DIR"

    if [ "$next_retry_attempt" -gt "$receiver_io_max_retries" ]; then
      echo "Receiver-I/O invalid after ${receiver_io_max_retries} retries; aborting"
      echo "Last diagnostic: $retry_record"
      exit 42
    fi

    echo "Deleted receiver-I/O-invalid attempt before quality analysis"
    echo "Diagnostic: $retry_record"
    echo "Retrying ${PREFIX}_${codec}: attempt ${next_retry_attempt}/${receiver_io_max_retries} after ${receiver_io_retry_cooldown_sec}s"
    sleep "$receiver_io_retry_cooldown_sec"
    exec env RECEIVER_IO_RETRY_ATTEMPT="$next_retry_attempt" \
      "$0" "$@"
  elif [ "$RECEIVER_IO_STATUS" -ne 0 ]; then
    echo "Receiver-I/O health check failed with status $RECEIVER_IO_STATUS"
    exit "$RECEIVER_IO_STATUS"
  fi
fi

# Extract data and draw plots
if command -v "$PYTHON" >/dev/null 2>&1; then
  run_analysis_step() {
    if [ "$strict_run_validation" = "1" ]; then
      "$@"
    else
      "$@" || true
    fi
  }

  run_analysis_step "$PYTHON" "$PROJECT_ROOT/scripts/calc_latency.py" "${RESULT_DIR}"
  run_analysis_step "$PYTHON" "$PROJECT_ROOT/scripts/extract_framesize_rate.py" "${RESULT_DIR}"
  run_analysis_step "$PYTHON" "$PROJECT_ROOT/scripts/plot_gcc.py" "${RESULT_DIR}"

  slice_number="1"
  if [ "$experiment_mode" = "x264_slice" ] ||
     { [ "$experiment_mode" = "auto" ] && [ "$codec" = "x264_slice" ]; }; then
    slice_number="adaptive_1_4_9"
  elif [ "$codec" = "mock" ]; then
    slice_number="mock"
  fi
  run_analysis_step "$PYTHON" "$PROJECT_ROOT/scripts/collect_x264_trial_metrics.py" \
    --result-dir "${RESULT_DIR}" \
    --reference "${input_video_file}" \
    --reference-loop-count "${reference_loop_count}" \
    --decoded "${RESULT_DIR}/rec.yuv" \
    --width "${width}" \
    --height "${height}" \
    --frames "${frames_to_encode}" \
    --fps "${fps}" \
    --codec "${codec}" \
    --experiment-mode "${experiment_label}" \
    --slice-number "${slice_number}" \
    --trial "${trial_id}" \
    --port "${port}" \
    --send-status "${SEND_STATUS}" \
    --trials-csv "${trials_csv}" \
    --ffmpeg "${FFMPEG_BIN}"

  run_analysis_step "$PYTHON" "$PROJECT_ROOT/scripts/draw.py" "${RESULT_DIR}"

  if [ "$strict_run_validation" = "1" ]; then
    "$PYTHON" "$PROJECT_ROOT/scripts/validate_real_trace_trial.py" \
      --result-dir "${RESULT_DIR}" \
      --trials-csv "${trials_csv}" \
      --codec "${codec}" \
      --trial "${trial_id}" \
      --expected-frames "${frames_to_encode}" \
      --expected-loops "${reference_loop_count}" \
      --min-decoded-ratio "${min_decoded_ratio}" \
      --max-send-duration-ms "${max_send_duration_ms}"
  fi
fi

if [ "$archive_decoded_video" = "0" ] && [ -s "${RESULT_DIR}/rec.yuv" ]; then
  # Long real-trace runs create a roughly 25 GB raw decode. Quality and latency
  # analysis has already completed (and strict validation has passed), so the
  # raw intermediate can be discarded without keeping another lossless copy.
  rm -f "${RESULT_DIR}/rec.yuv"
elif command -v "$FFMPEG_BIN" >/dev/null 2>&1 && [ -s "${RESULT_DIR}/rec.yuv" ]; then
  set +e
  "$FFMPEG_BIN" -hide_banner \
    -f rawvideo \
    -pix_fmt yuv420p \
    -s "${width}x${height}" \
    -r "${fps}" \
    -i "${RESULT_DIR}/rec.yuv" \
    -c:v libx264 \
    -preset ultrafast \
    -qp 0 \
    -pix_fmt yuv420p \
    -y "${RESULT_DIR}/rec.mp4" \
    > "${RESULT_DIR}/rec_mp4_ffmpeg.log" 2>&1
  RECODE_STATUS=$?
  set -e
  if [ "$RECODE_STATUS" -eq 0 ]; then
    rm -f "${RESULT_DIR}/rec.yuv"
  else
    echo "Warning: failed to encode rec.yuv to rec.mp4; kept raw YUV. See ${RESULT_DIR}/rec_mp4_ffmpeg.log"
    if [ "$strict_run_validation" = "1" ]; then
      exit "$RECODE_STATUS"
    fi
  fi
fi
