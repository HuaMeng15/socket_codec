#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#include "transmission/pacer.h"
#include "transmission/packet_header.h"

// Helpers ------------------------------------------------------------------

static std::vector<uint8_t> MakePacket(size_t size,
                                       uint16_t frame_seq = 1,
                                       uint8_t pkt_idx = 0) {
  std::vector<uint8_t> p(size, 0xAB);
  auto* h = reinterpret_cast<FramePacketHeader*>(p.data());
  h->frame_sequence = htons(frame_seq);
  h->packet_index = pkt_idx;
  h->total_packets = 1;
  h->payload_size = htons(static_cast<uint16_t>(size - sizeof(FramePacketHeader)));
  return p;
}

// --------------------------------------------------------------------------
// Token-bucket bound: a 5ms cap at 1 Mbps = 625 bytes of burst credit.
// After a long idle gap (100 ms) the first packet must still be delayed only
// by at most burst_cap_ms worth of credit, not by the full 100ms backlog.
// --------------------------------------------------------------------------
TEST(PacerTest, BurstIsBoundedAfterIdleGap) {
  Pacer pacer;
  pacer.SetBurstCapMs(5.0);   // max burst = 5ms of data
  pacer.SetPaceMultiplier(1.0);
  pacer.SetTargetBitrate(1000);  // 1 Mbps

  std::atomic<int64_t> first_send_us{-1}, second_send_us{-1};
  pacer.SetSendCallback([&](const uint8_t*, size_t) {
    auto t = std::chrono::duration_cast<std::chrono::microseconds>(
                 std::chrono::steady_clock::now().time_since_epoch())
                 .count();
    if (first_send_us.load() == -1) first_send_us.store(t);
    else if (second_send_us.load() == -1) second_send_us.store(t);
  });
  pacer.Start();

  // Idle for 100ms so tokens would accumulate to 100ms without a cap.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // 1460-byte packet → 11680 bits / 1e6 bps = 11.68 ms to pace at 1x.
  auto p1 = MakePacket(1460, 1, 0);
  auto p2 = MakePacket(1460, 1, 1);
  pacer.Enqueue(p1.data(), p1.size(), 1, 0);
  pacer.Enqueue(p2.data(), p2.size(), 1, 1);

  // Give the pacer thread time to drain both packets.
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  pacer.Stop();

  ASSERT_NE(first_send_us.load(), -1) << "first packet never sent";
  ASSERT_NE(second_send_us.load(), -1) << "second packet never sent";

  // Gap between p1 and p2 must be >= 11ms (paced) — not a burst.
  int64_t gap_us = second_send_us.load() - first_send_us.load();
  EXPECT_GE(gap_us, 8000) << "second packet sent too soon — burst not bounded";
}

// --------------------------------------------------------------------------
// Throughput: average send rate must be within ±20% of the target.
// --------------------------------------------------------------------------
TEST(PacerTest, AverageThroughputMatchesTarget) {
  Pacer pacer;
  pacer.SetBurstCapMs(5.0);
  pacer.SetPaceMultiplier(1.0);
  pacer.SetTargetBitrate(2000);  // 2 Mbps

  std::atomic<int64_t> bytes_sent{0};
  pacer.SetSendCallback([&](const uint8_t*, size_t sz) {
    bytes_sent.fetch_add(static_cast<int64_t>(sz));
  });
  pacer.Start();

  // Enqueue 300 packets of 1460 bytes = 3.5 Mbits → ~1.75s at 2 Mbps. That
  // keeps the queue backlogged for the entire 800ms measurement window, so the
  // measured rate reflects the pace rate (not an artifact of the queue
  // draining early and leaving idle time in the window).
  auto pkt = MakePacket(1460, 1, 0);
  for (int i = 0; i < 300; i++) {
    pacer.Enqueue(pkt.data(), pkt.size(), 1, static_cast<uint8_t>(i % 256));
  }

  auto t0 = std::chrono::steady_clock::now();
  std::this_thread::sleep_for(std::chrono::milliseconds(800));
  pacer.Stop();
  double elapsed_s =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
          .count();

  double actual_kbps = (bytes_sent.load() * 8.0 / 1000.0) / elapsed_s;
  EXPECT_GE(actual_kbps, 2000.0 * 0.80) << "throughput too low";
  EXPECT_LE(actual_kbps, 2000.0 * 1.20) << "throughput too high";
}

// --------------------------------------------------------------------------
// Padding: while probing and the queue is empty, padding packets must be
// emitted at approximately the probe rate.
// --------------------------------------------------------------------------
TEST(PacerTest, PaddingFillesPipeWhenProbing) {
  Pacer pacer;
  pacer.SetBurstCapMs(5.0);
  pacer.SetPaceMultiplier(1.0);
  pacer.SetTargetBitrate(2000);  // 2 Mbps
  pacer.SetMaxPacketSize(1460);

  std::atomic<int> padding_count{0};
  std::atomic<int64_t> padding_bytes{0};
  pacer.SetSendCallback([&](const uint8_t* data, size_t sz) {
    const auto* h = reinterpret_cast<const FramePacketHeader*>(data);
    if (ntohs(h->frame_sequence) == kPaddingFrameSequence) {
      padding_count++;
      padding_bytes.fetch_add(static_cast<int64_t>(sz));
    }
  });
  pacer.Start();
  pacer.SetProbing(true);  // no real packets in queue → pure padding

  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  pacer.SetProbing(false);
  pacer.Stop();

  // At 2 Mbps with 1460B packets: ~171 pkts/s → ~34 in 200ms.
  // Allow generous slack for scheduler jitter.
  EXPECT_GT(padding_count.load(), 10) << "too few padding packets emitted";
  // Padding bytes must track probe rate within 40%.
  double expected_bits = 2000.0 * 1000.0 * 0.200;  // 2Mbps × 200ms
  double actual_bits = padding_bytes.load() * 8.0;
  EXPECT_GE(actual_bits, expected_bits * 0.60);
  EXPECT_LE(actual_bits, expected_bits * 1.40);
}

// --------------------------------------------------------------------------
// Padding sentinel: emitted padding must use kPaddingFrameSequence.
// --------------------------------------------------------------------------
TEST(PacerTest, PaddingUsesReservedSentinelFrameSequence) {
  Pacer pacer;
  pacer.SetBurstCapMs(5.0);
  pacer.SetPaceMultiplier(1.0);
  pacer.SetTargetBitrate(1000);
  pacer.SetMaxPacketSize(200);

  bool saw_non_padding = false;
  pacer.SetSendCallback([&](const uint8_t* data, size_t sz) {
    if (sz < sizeof(FramePacketHeader)) return;
    const auto* h = reinterpret_cast<const FramePacketHeader*>(data);
    if (ntohs(h->frame_sequence) != kPaddingFrameSequence)
      saw_non_padding = true;
  });
  pacer.Start();
  pacer.SetProbing(true);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  pacer.SetProbing(false);
  pacer.Stop();

  EXPECT_FALSE(saw_non_padding) << "non-padding packet appeared with no real data";
}

// --------------------------------------------------------------------------
// No padding when not probing.
// --------------------------------------------------------------------------
TEST(PacerTest, NoPaddingWhenNotProbing) {
  Pacer pacer;
  pacer.SetBurstCapMs(5.0);
  pacer.SetPaceMultiplier(1.0);
  pacer.SetTargetBitrate(1000);

  std::atomic<int> sent{0};
  pacer.SetSendCallback([&](const uint8_t*, size_t) { sent++; });
  pacer.Start();
  // probing_ defaults to false; idle for 100ms with empty queue.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  pacer.Stop();

  EXPECT_EQ(sent.load(), 0) << "pacer sent packets with empty queue and no probe";
}

// --------------------------------------------------------------------------
// Record callback is invoked for real packets but NOT for padding.
// --------------------------------------------------------------------------
TEST(PacerTest, RecordCallbackCalledForRealPacketsNotPadding) {
  Pacer pacer;
  pacer.SetBurstCapMs(5.0);
  pacer.SetPaceMultiplier(1.0);
  pacer.SetTargetBitrate(5000);
  pacer.SetMaxPacketSize(200);

  std::atomic<int> record_calls{0};
  pacer.SetRecordCallback(
      [&](uint16_t, uint8_t) { record_calls++; });

  int real_count = 0;
  pacer.SetSendCallback([&](const uint8_t* data, size_t sz) {
    if (sz < sizeof(FramePacketHeader)) return;
    const auto* h = reinterpret_cast<const FramePacketHeader*>(data);
    if (ntohs(h->frame_sequence) != kPaddingFrameSequence) real_count++;
  });

  pacer.Start();

  // Enqueue 5 real packets, then let probing emit a few padding.
  auto pkt = MakePacket(200, 7, 0);
  for (uint8_t i = 0; i < 5; i++) {
    pacer.Enqueue(pkt.data(), pkt.size(), 7, i);
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  pacer.SetProbing(true);
  std::this_thread::sleep_for(std::chrono::milliseconds(40));
  pacer.SetProbing(false);
  pacer.Stop();

  EXPECT_EQ(record_calls.load(), 5) << "record callback should fire exactly once per real packet";
}
