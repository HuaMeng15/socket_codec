#include <gtest/gtest.h>
#include <chrono>
#include <thread>

#include "transmission/gcc_controller.h"
#include "transmission/transport_feedback.h"

class GccControllerTest : public ::testing::Test {
 protected:
  GccController gcc;

  void SetUp() override {
    gcc.SetInitialBitrate(5000);  // 5 Mbps
    gcc.SetBitrateRange(100, 30000);
  }

  // Create a feedback batch with evenly spaced arrival times
  TransportFeedback MakeFeedback(int num_packets, int64_t base_time_us,
                                  int64_t spacing_us) {
    TransportFeedback fb;
    fb.reference_time_us = base_time_us;
    for (int i = 0; i < num_packets; i++) {
      TransportFeedback::PacketInfo info;
      info.frame_sequence = static_cast<uint16_t>(i / 10);
      info.packet_index = static_cast<uint8_t>(i % 10);
      info.arrival_time_us = base_time_us + i * spacing_us;
      fb.packets.push_back(info);
    }
    return fb;
  }
};

TEST_F(GccControllerTest, InitialBitrate) {
  EXPECT_EQ(gcc.GetTargetBitrateKbps(), 5000);
}

TEST_F(GccControllerTest, StableNetworkMaintainsOrIncreasesRate) {
  // Feed stable feedback (no queuing delay buildup)
  int64_t time = 1000000;
  int prev_bitrate = gcc.GetTargetBitrateKbps();

  for (int round = 0; round < 10; round++) {
    // 20 packets arriving at expected rate (~1ms spacing for ~10Mbps)
    auto fb = MakeFeedback(20, time, 1000);  // 1ms between packets
    gcc.OnTransportFeedback(fb);
    time += 30000;  // next feedback in 30ms
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  // Rate should not have decreased
  EXPECT_GE(gcc.GetTargetBitrateKbps(), prev_bitrate);
}

TEST_F(GccControllerTest, IncreasingDelayTriggersDecrease) {
  int64_t time = 1000000;

  // First: establish baseline with normal feedback
  for (int round = 0; round < 5; round++) {
    auto fb = MakeFeedback(20, time, 1000);
    gcc.OnTransportFeedback(fb);
    time += 40000;  // 40ms between feedback batches
  }

  int bitrate_before = gcc.GetTargetBitrateKbps();

  // Now: simulate congestion — inter-feedback gap grows rapidly
  // (packets arrive later and later = queuing delay increasing)
  for (int round = 0; round < 30; round++) {
    auto fb = MakeFeedback(20, time, 1000);
    gcc.OnTransportFeedback(fb);
    // Gap between feedbacks grows: simulates one-way delay increasing
    time += 40000 + round * 5000;
  }

  // Rate should have decreased due to overuse detection
  EXPECT_LT(gcc.GetTargetBitrateKbps(), bitrate_before);
}

TEST_F(GccControllerTest, LossTriggersDecrease) {
  int initial = gcc.GetTargetBitrateKbps();

  // Report losses
  for (int i = 0; i < 5; i++) {
    LossReport report;
    for (int j = 0; j < 5; j++) {
      report.packets.push_back({static_cast<uint16_t>(i), static_cast<uint8_t>(j)});
    }
    gcc.OnLossReport(report);
  }

  // Should have decreased
  EXPECT_LT(gcc.GetTargetBitrateKbps(), initial);
}

TEST_F(GccControllerTest, BitrateStaysWithinBounds) {
  gcc.SetBitrateRange(500, 8000);
  gcc.SetInitialBitrate(5000);

  // Heavy loss — push rate down
  for (int i = 0; i < 50; i++) {
    LossReport report;
    for (int j = 0; j < 10; j++) {
      report.packets.push_back({static_cast<uint16_t>(i), static_cast<uint8_t>(j)});
    }
    gcc.OnLossReport(report);
  }

  EXPECT_GE(gcc.GetTargetBitrateKbps(), 500);
  EXPECT_LE(gcc.GetTargetBitrateKbps(), 8000);
}

TEST_F(GccControllerTest, RecoveryAfterCongestion) {
  int64_t time = 1000000;

  // Establish baseline
  for (int round = 0; round < 5; round++) {
    auto fb = MakeFeedback(20, time, 1000);
    gcc.OnTransportFeedback(fb);
    time += 40000;
  }

  // Cause overuse with growing inter-feedback gaps
  for (int round = 0; round < 30; round++) {
    auto fb = MakeFeedback(20, time, 1000);
    gcc.OnTransportFeedback(fb);
    time += 40000 + round * 5000;
  }

  int low_bitrate = gcc.GetTargetBitrateKbps();
  EXPECT_LT(low_bitrate, 5000);  // should have dropped

  // Wait >1s for the overuse guard to expire
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));

  // Feed underuse signals — short inter-group gap relative to expected
  // With bitrate ~2200kbps, expected spacing for 20*1400B = ~100ms
  // We use 20ms gap => strong underuse signal
  for (int round = 0; round < 10; round++) {
    auto fb = MakeFeedback(20, time, 500);
    gcc.OnTransportFeedback(fb);
    time += 20000;
    std::this_thread::sleep_for(std::chrono::milliseconds(210));
  }

  EXPECT_GT(gcc.GetTargetBitrateKbps(), low_bitrate);
}
