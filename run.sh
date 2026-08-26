#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$SCRIPT_DIR"
BINARY="$PROJECT_ROOT/build/socket_codec"
PYTHON="${PYTHON:-python3}"
FFMPEG_BIN="${FFMPEG_BIN:-ffmpeg}"

PREFIX="${1:-mahimahi}"
codec="${2:-x264_slice}"
ip="${3:-}"
frames_to_encode="${4:-800}"
bandwidth_file="${5:-$PROJECT_ROOT/input/traces/15s_10to1_until_300s.log}"
fps="${6:-30}"
input_video_file="${7:-/home/menghua/Research/VideoResources/Lecture.yuv}"
width="${WIDTH:-1920}"
height="${HEIGHT:-1080}"
port="${PORT:-8888}"
feedback_max_interval_ms="${FEEDBACK_MAX_INTERVAL_MS:-25}"
trial_id="${TRIAL_ID:-1}"
trials_csv="${TRIALS_CSV:-}"
result_base="${RESULT_BASE:-$PROJECT_ROOT/result}"
sender_output_file="${SENDER_OUTPUT_FILE:-}"

RESULT_DIR="${result_base}/${PREFIX}_${codec}"
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
if [ ! -f "$input_video_file" ]; then
  echo "Input YUV video not found: $input_video_file"
  exit 1
fi

if [ -d "${RESULT_DIR}" ]; then
  rm -rf "${RESULT_DIR}"
fi
mkdir -p "${RESULT_DIR}"

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
  --fps=${fps}
  --width=${width}
  --height=${height}
  --input_video_file="${input_video_file}"
)
if [ -n "${sender_output_file}" ]; then
  SENDER_ARGS+=(--output_video_file="${sender_output_file}")
fi
exec ${BINARY} "\${SENDER_ARGS[@]}" > ${SEND_LOG} 2>&1
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

# Run sender inside mahimahi so its outbound packets are shaped by mm-link.
set +e
mm-link "${bandwidth_file}" "${bandwidth_file}" -- bash "${SENDER_WRAPPER}"
SEND_STATUS=$?
set -e
cleanup
trap - EXIT
rm -f "${SENDER_WRAPPER}"

echo "Sender exit: ${SEND_STATUS}"
echo "Logs: ${RECV_LOG} ${SEND_LOG}"

# Extract data and draw plots
if command -v "$PYTHON" >/dev/null 2>&1; then
  "$PYTHON" "$PROJECT_ROOT/scripts/calc_latency.py" "${RESULT_DIR}" || true
  "$PYTHON" "$PROJECT_ROOT/scripts/extract_framesize_rate.py" "${RESULT_DIR}" || true
  "$PYTHON" "$PROJECT_ROOT/scripts/plot_gcc.py" "${RESULT_DIR}" || true

  slice_number="adaptive_1_4_9"
  if [ "$codec" = "x264" ]; then
    slice_number="1"
  elif [ "$codec" = "mock" ]; then
    slice_number="mock"
  fi
  "$PYTHON" "$PROJECT_ROOT/scripts/collect_x264_trial_metrics.py" \
    --result-dir "${RESULT_DIR}" \
    --reference "${input_video_file}" \
    --decoded "${RESULT_DIR}/rec.yuv" \
    --width "${width}" \
    --height "${height}" \
    --frames "${frames_to_encode}" \
    --fps "${fps}" \
    --codec "${codec}" \
    --slice-number "${slice_number}" \
    --trial "${trial_id}" \
    --port "${port}" \
    --send-status "${SEND_STATUS}" \
    --trials-csv "${trials_csv}" \
    --ffmpeg "${FFMPEG_BIN}" || true

  "$PYTHON" "$PROJECT_ROOT/scripts/draw.py" "${RESULT_DIR}" || true
fi

if command -v "$FFMPEG_BIN" >/dev/null 2>&1 && [ -s "${RESULT_DIR}/rec.yuv" ]; then
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
  fi
fi
