#!/usr/bin/env bash
set -e

PREFIX="${1:-prefix}"

codec="${2:-mock}"
ip="${3:-10.0.0.2}"
frames_to_encode="${4:-1600}"
bandwidth_file="${5:-input/10s_10to1_until_300s.log}"
fps="${6:-120}"

RESULT_DIR="result/${PREFIX}_${codec}"
RECV_LOG="${RESULT_DIR}/recv.log"
SEND_LOG="${RESULT_DIR}/send.log"
MMLINK_IP_FILE="${RESULT_DIR}/mmlink_ip"
# Gateway on the delay link (mahimahi default); override with MM_VIA if needed
MM_VIA="${MM_VIA:-10.0.0.2}"

if [ -d "${RESULT_DIR}" ]; then
  rm -rf "${RESULT_DIR}"
fi
mkdir -p "${RESULT_DIR}"

# Helper script run inside mm-link: write this container's IP to file, then start receiver
RECEIVER_WRAPPER="${RESULT_DIR}/.run_receiver.sh"
cat > "${RECEIVER_WRAPPER}" << RECEIVER_EOF
#!/bin/bash
MMLINK_IP=\$(ip -4 addr show ingress 2>/dev/null | awk '/inet /{print \$2}' | cut -d/ -f1)
[ -z "\$MMLINK_IP" ] && MMLINK_IP=\$(ip -4 addr show 2>/dev/null | awk '/inet /{print \$2}' | cut -d/ -f1 | grep -v '^127\.' | head -1)
echo "\$MMLINK_IP" > ${MMLINK_IP_FILE}
exec ./build/socket_codec --file=${RESULT_DIR}/rec.yuv --codec=${codec} --fps=${fps} > ${RECV_LOG} 2>&1
RECEIVER_EOF
chmod +x "${RECEIVER_WRAPPER}"

# Run receiver inside mahimahi: mm-delay then mm-link
mm-delay 1 bash -c "mm-link ${bandwidth_file} ${bandwidth_file} -- bash ${RECEIVER_WRAPPER}" &
RECV_PID=$!

sleep 2

# Discover mm-link container IP and delay interface; add route so host can reach receiver
MMLINK_IP=""
if [ -f "${MMLINK_IP_FILE}" ]; then
  MMLINK_IP=$(cat "${MMLINK_IP_FILE}" | tr -d '\n' | grep -E '^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$')
fi
RECEIVER_IP="${MMLINK_IP:-$ip}"
echo "Receiver IP: ${RECEIVER_IP}"
echo "MMLINK_IP: ${MMLINK_IP}"
if [ -n "${MMLINK_IP}" ]; then
  DELAY_IF=$(ls /sys/class/net 2>/dev/null | grep '^delay-' | head -1)
  echo "Mahimahi interface: ${DELAY_IF}"
  if [ -n "${DELAY_IF}" ]; then
    sudo ip route add "${MMLINK_IP}/32" via "${MM_VIA}" dev "${DELAY_IF}" 2>/dev/null || true
    # if ! ip route get "${MMLINK_IP}" &>/dev/null; then
    #   sudo ip route add "${MMLINK_IP}/32" via "${MM_VIA}" dev "${DELAY_IF}" 2>/dev/null || true
    # fi
  fi
fi

# Run sender outside mahimahi so traffic goes through the delayed link
if ./build/socket_codec --frames_to_encode=${frames_to_encode} --ip=${RECEIVER_IP} --codec=${codec} --fps=${fps} > ${SEND_LOG} 2>&1; then
  :
fi
kill "${RECV_PID}" 2>/dev/null || true
rm -f "${RECEIVER_WRAPPER}"

echo "Logs: ${RECV_LOG} ${SEND_LOG}"

# Extract data and draw plots
if command -v python3 >/dev/null 2>&1; then
  python3 "$(dirname "$0")/scripts/calc_latency.py" "${RESULT_DIR}"
  python3 "$(dirname "$0")/scripts/extract_framesize_rate.py" "${RESULT_DIR}"
  python3 "$(dirname "$0")/scripts/draw.py" "${RESULT_DIR}"
fi

