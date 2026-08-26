#!/usr/bin/env python3
import re
import sys
from datetime import datetime
from pathlib import Path

import numpy as np

# Only match our app's timestamp (YYYY-MM-DD HH:MM:SS.fff) so x264 "[info]" etc. are not captured when logs are interleaved.
_TS = r"(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d+)"


def parse_ts(s: str) -> float:
    return datetime.strptime(s.strip("[]"), "%Y-%m-%d %H:%M:%S.%f").timestamp()


def parse_send_log(path: Path):
    # Whole-frame path: "Sending frame N size=.. in M packets" / "Successfully sent frame N in M packets"
    # Sliced path:      "Sending frame N fragment size=.. in M packets" /
    #                   "Successfully sent frame N fragment in M packets"
    # Match both by making "fragment " optional and accumulating packet counts
    # across all fragments of a frame.
    re_send = re.compile(
        r"\[" + _TS + r"\].*\[DataSender\] Sending frame (\d+) (?:fragment )?.* in (\d+) packets"
    )
    re_sent = re.compile(
        r"\[" + _TS + r"\].*\[DataSender\] Successfully sent frame (\d+) (?:fragment )?in (\d+) packets"
    )
    re_pacer_sent = re.compile(
        r"\[" + _TS + r"\].*\[Pacer\] Pacer sent all packets for frame "
        r"(\d+) sent_packets=(\d+) total_packets=(\d+)"
    )
    re_capture = re.compile(
        r"\[" + _TS + r"\].*\[VideoCaptureAndSend\] Read frame (\d+)"
    )
    text = path.read_text()
    # Accumulate across fragments: keep the FIRST "Sending" timestamp as the frame
    # start, the LAST "Successfully sent" timestamp as the frame end, and sum
    # packet counts. For the whole-frame path there is exactly one of each, so
    # this reduces to the original behavior.
    starts = {}       # frame -> first send ts
    ends = {}         # frame -> last sent ts
    pacer_ends = {}   # frame -> timestamp when final packet leaves pacer
    npackets = {}     # frame -> total packets summed across fragments
    for m in re_send.finditer(text):
        ts, frame, np = parse_ts(m.group(1)), int(m.group(2)), int(m.group(3))
        if frame not in starts:
            starts[frame] = ts
        npackets[frame] = npackets.get(frame, 0) + np
    for m in re_sent.finditer(text):
        ts, frame = parse_ts(m.group(1)), int(m.group(2))
        ends[frame] = ts  # keep overwriting -> last fragment wins
    for m in re_pacer_sent.finditer(text):
        ts, frame = parse_ts(m.group(1)), int(m.group(2))
        pacer_ends[frame] = ts
    sends = {}
    for frame, start in starts.items():
        sends[frame] = (start, ends.get(frame), npackets.get(frame, 0))
    capture_times = {}
    for m in re_capture.finditer(text):
        ts, frame = m.group(1), int(m.group(2))
        capture_times[frame] = parse_ts(ts)
    return sends, capture_times, pacer_ends


def parse_recv_log(path: Path):
    re_pkt = re.compile(
        r"\[" + _TS + r"\].*\[ReceivedFrameDataHandler\] Received packet (\d+) for frame (\d+)"
    )
    re_dec = re.compile(
        r"\[" + _TS + r"\].*\[ReceivedFrameDataHandler\] Successfully decoded frame (\d+)"
    )
    text = path.read_text()
    recv_packets = {}
    decode_frame = {}
    for m in re_pkt.finditer(text):
        ts, pkt, frame = m.group(1), int(m.group(2)), int(m.group(3))
        recv_packets[(frame, pkt)] = parse_ts(ts)
    for m in re_dec.finditer(text):
        ts, frame = m.group(1), int(m.group(2))
        decode_frame[frame] = parse_ts(ts)
    return recv_packets, decode_frame


def main():
    if len(sys.argv) < 2:
        print("Usage: python3 scripts/calc_latency.py <result_dir>", file=sys.stderr)
        sys.exit(1)
    result_dir = Path(sys.argv[1])
    send_log = result_dir / "send.log"
    recv_log = result_dir / "recv.log"
    if not send_log.exists() or not recv_log.exists():
        print(f"Missing {send_log} or {recv_log}", file=sys.stderr)
        sys.exit(1)

    sends, capture_times, pacer_ends = parse_send_log(send_log)
    recv_packets, decode_frame = parse_recv_log(recv_log)

    packet_latencies = []
    frame_latencies = []
    pacer_frame_latencies = []
    pacer_queue_delays = []
    overall_latencies = []  # capture -> decode (per frame)
    stall_100 = 0
    stall_200 = 0

    for frame, (start_ts, end_ts, num_packets) in sorted(sends.items()):
        if end_ts is None:
            continue
        decode_ts = decode_frame.get(frame)
        if decode_ts is not None:
            frame_latency = (decode_ts - end_ts) * 1000
            frame_latencies.append((frame, frame_latency))
            if frame_latency > 100.0:
                stall_100 += 1
            if frame_latency > 200.0:
                stall_200 += 1
        capture_ts = capture_times.get(frame)
        if decode_ts is not None and capture_ts is not None:
            overall_latencies.append((frame, (decode_ts - capture_ts) * 1000))
        pacer_end_ts = pacer_ends.get(frame)
        if pacer_end_ts is not None:
            pacer_queue_delays.append((frame, (pacer_end_ts - end_ts) * 1000))
            if decode_ts is not None:
                pacer_frame_latencies.append(
                    (frame, (decode_ts - pacer_end_ts) * 1000)
                )

        for p in range(num_packets):
            recv_ts = recv_packets.get((frame, p))
            if recv_ts is None:
                continue
            if num_packets <= 1:
                send_ts = start_ts
            else:
                send_ts = start_ts + (end_ts - start_ts) * p / (num_packets - 1)
            packet_latencies.append((frame, p, (recv_ts - send_ts) * 1000))

    # Write frame_latency.csv: frame_index, frame_latency
    frame_csv = result_dir / "frame_latency.csv"
    with open(frame_csv, "w") as f:
        f.write("frame_index,frame_latency\n")
        for fidx, lat in frame_latencies:
            f.write(f"{fidx},{lat:.2f}\n")

    # Write packet_latency.csv: frame_index, packet_index, packet_latency
    packet_csv = result_dir / "packet_latency.csv"
    with open(packet_csv, "w") as f:
        f.write("frame_index,packet_index,packet_latency\n")
        for fidx, pidx, lat in packet_latencies:
            f.write(f"{fidx},{pidx},{lat:.2f}\n")

    # Write pacer_queue_delay.csv: DataSender enqueue done -> final packet leaves pacer
    pacer_queue_csv = result_dir / "pacer_queue_delay.csv"
    with open(pacer_queue_csv, "w") as f:
        f.write("frame_index,pacer_queue_delay\n")
        for fidx, lat in pacer_queue_delays:
            f.write(f"{fidx},{lat:.2f}\n")

    # Write pacer_frame_latency.csv: final packet leaves pacer -> decode done
    pacer_frame_csv = result_dir / "pacer_frame_latency.csv"
    with open(pacer_frame_csv, "w") as f:
        f.write("frame_index,pacer_frame_latency\n")
        for fidx, lat in pacer_frame_latencies:
            f.write(f"{fidx},{lat:.2f}\n")

    # Write overall_latency.csv: frame_index, overall_latency (capture -> decode, ms)
    overall_csv = result_dir / "overall_latency.csv"
    with open(overall_csv, "w") as f:
        f.write("frame_index,overall_latency\n")
        for fidx, lat in overall_latencies:
            f.write(f"{fidx},{lat:.2f}\n")

    def tail_mean(data, ratio):
        """Mean of the tail (1 - ratio) of data. ratio=0.99 -> mean of top 1%."""
        if not data:
            return 0.0
        data = np.array(data)
        data.sort()
        data_len = len(data)
        print(f"Data length: {data_len} tail_length: {int(data_len * (1 - ratio))}")
        tail_data = data[int(data_len * ratio):data_len]
        print(f"tail_data_length: {len(tail_data)}")
        if len(tail_data) < 20:
          print(f"Tail data for ratio {ratio}: {tail_data}")
        return float(np.mean(tail_data))

    # Terminal: average, max, tail 99 (mean of top 1%), tail 99.9 (mean of top 0.1%)
    if packet_latencies:
        pkt_lats = [x[2] for x in packet_latencies]
        n = len(pkt_lats)
        avg_pkt = sum(pkt_lats) / n
        print(f"Packet latencies:")
        print(f"Packet latency (ms): avg={avg_pkt:.2f} max={max(pkt_lats):.2f} "
              f"tail99={tail_mean(pkt_lats, 0.99):.2f} tail99.9={tail_mean(pkt_lats, 0.999):.2f} (n={n})")
    if frame_latencies:
        frm_lats = [x[1] for x in frame_latencies]
        n = len(frm_lats)
        avg_frm = sum(frm_lats) / n
        print(f"Frame latencies:")
        print(f"Frame latency (ms):   avg={avg_frm:.2f} max={max(frm_lats):.2f} "
              f"tail99={tail_mean(frm_lats, 0.99):.2f} tail99.9={tail_mean(frm_lats, 0.999):.2f} (n={n})")
        print(f"Frame stalls:         >100ms={stall_100} >200ms={stall_200}")
    if overall_latencies:
        ov_lats = [x[1] for x in overall_latencies]
        n = len(ov_lats)
        avg_ov = sum(ov_lats) / n
        print(f"Overall latencies:")
        print(f"Overall latency (ms):  avg={avg_ov:.2f} max={max(ov_lats):.2f} "
              f"tail99={tail_mean(ov_lats, 0.99):.2f} tail99.9={tail_mean(ov_lats, 0.999):.2f} (n={n})")
    if pacer_queue_delays:
        pq_lats = [x[1] for x in pacer_queue_delays]
        n = len(pq_lats)
        avg_pq = sum(pq_lats) / n
        print(f"Pacer queue delays:")
        print(f"Pacer queue delay (ms): avg={avg_pq:.2f} max={max(pq_lats):.2f} "
              f"tail99={tail_mean(pq_lats, 0.99):.2f} tail99.9={tail_mean(pq_lats, 0.999):.2f} (n={n})")
    if pacer_frame_latencies:
        pfl_lats = [x[1] for x in pacer_frame_latencies]
        n = len(pfl_lats)
        avg_pfl = sum(pfl_lats) / n
        print(f"Pacer frame latencies:")
        print(f"Pacer frame latency (ms): avg={avg_pfl:.2f} max={max(pfl_lats):.2f} "
              f"tail99={tail_mean(pfl_lats, 0.99):.2f} tail99.9={tail_mean(pfl_lats, 0.999):.2f} (n={n})")
    print(f"Wrote {frame_csv}, {packet_csv}, {overall_csv}, {pacer_queue_csv}, {pacer_frame_csv}")


if __name__ == "__main__":
    main()
