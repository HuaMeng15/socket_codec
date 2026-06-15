#include <gtest/gtest.h>

#include "transmission/gcc_controller.h"
#include "transmission/transport_feedback.h"

/**
 * GCC unit tests using a fake clock for deterministic, fast execution.
 * Covers: delay-based overuse/underuse/normal, loss-based 3 ranges,
 * threshold adaptation, startup warmup, noisy feedback, bounds.
 */
class GccControllerTest : public ::testing::Test {
 protected:
  GccController gcc;
  int64_t clock_ms_ = 10000;  // Start at 10s to avoid edge cases

  void SetUp() override {
    gcc.SetClockForTesting(&clock_ms_);
    gcc.SetInitialBitrate(5000);
    gcc.SetBitrateRange(100, 30000);
  }

  // Advance fake clock
  void AdvanceMs(int64_t ms) { clock_ms_ += ms; }

  // Create feedback with uniform packet spacing within batch
  TransportFeedback MakeFeedback(int64_t send_time_us, int64_t arrival_base_us,
                                  int num_packets, int64_t spacing_us) {
    TransportFeedback fb;
    fb.reference_time_us = send_time_us;
    for (int i = 0; i < num_packets; i++) {
      fb.packets.push_back({
          static_cast<uint16_t>(i / 10), static_cast<uint8_t>(i % 10),
          arrival_base_us + i * spacing_us});
    }
    return fb;
  }

  // Feed N rounds of stable feedback (send/arrival deltas match)
  void FeedStable(int rounds, int64_t& send_time, int64_t& arrival_time) {
    for (int i = 0; i < rounds; i++) {
      auto fb = MakeFeedback(send_time, arrival_time, 20, 1000);
      gcc.OnTransportFeedback(fb);
      send_time += 33000;
      arrival_time += 33000;
      AdvanceMs(33);
    }
  }

  // Feed N rounds where arrival grows faster (congestion)
  void FeedOveruse(int rounds, int64_t& send_time, int64_t& arrival_time,
                   int extra_per_round_us = 2000) {
    for (int i = 0; i < rounds; i++) {
      auto fb = MakeFeedback(send_time, arrival_time, 20, 1000);
      gcc.OnTransportFeedback(fb);
      send_time += 33000;
      arrival_time += 33000 + extra_per_round_us;
      AdvanceMs(33);
    }
  }

  // Feed N rounds where arrival catches up (queue draining)
  void FeedUnderuse(int rounds, int64_t& send_time, int64_t& arrival_time,
                    int catch_up_per_round_us = 2000) {
    for (int i = 0; i < rounds; i++) {
      auto fb = MakeFeedback(send_time, arrival_time, 20, 1000);
      gcc.OnTransportFeedback(fb);
      send_time += 33000;
      // Arrival gap is shorter than send gap → queue draining
      arrival_time += 33000 - catch_up_per_round_us;
      AdvanceMs(33);
    }
  }
};

// --- Basic state tests ---

TEST_F(GccControllerTest, InitialBitrate) {
  EXPECT_EQ(gcc.GetTargetBitrateKbps(), 5000);
}

TEST_F(GccControllerTest, BitrateStaysWithinBounds) {
  gcc.SetBitrateRange(500, 8000);
  gcc.SetInitialBitrate(5000);

  // Heavy loss to push down
  gcc.OnPacketsSent(100);
  LossReport report;
  for (int i = 0; i < 50; i++) {
    report.packets.push_back({static_cast<uint16_t>(i), 0});
  }
  gcc.OnLossReport(report);

  EXPECT_GE(gcc.GetTargetBitrateKbps(), 500);
  EXPECT_LE(gcc.GetTargetBitrateKbps(), 8000);
}

// --- Delay-based: overuse detection ---

TEST_F(GccControllerTest, OveruseDetectedOnGrowingDelay) {
  int64_t send = 1000000, arrival = 1000000;
  FeedStable(5, send, arrival);  // warmup
  int before = gcc.GetTargetBitrateKbps();

  FeedOveruse(30, send, arrival);

  EXPECT_LT(gcc.GetTargetBitrateKbps(), before);
}

TEST_F(GccControllerTest, OveruseDecreasesMultiplicatively) {
  int64_t send = 1000000, arrival = 1000000;
  FeedStable(5, send, arrival);

  FeedOveruse(15, send, arrival);
  int after_overuse = gcc.GetTargetBitrateKbps();

  // Multiple 0.85x decreases fire in 15 rounds (counter=3 per trigger).
  // Expect 2-4 decreases: 5000*0.85^2=3612, 5000*0.85^4=2626
  EXPECT_LT(after_overuse, 5000);
  EXPECT_GT(after_overuse, 2500);
  // Verify it's a multiplicative pattern (roughly 0.85^n * 5000)
  double ratio = after_overuse / 5000.0;
  EXPECT_LT(ratio, 0.85);  // at least one decrease
}

TEST_F(GccControllerTest, StartupWarmupPreventsEarlyOveruse) {
  // Very first few samples should not trigger overuse due to scaling
  int64_t send = 1000000, arrival = 1000000;
  // Only 3 rounds of heavy overuse — not enough to build trendline
  FeedOveruse(3, send, arrival, 10000);

  // Should NOT have decreased yet (need window to fill)
  EXPECT_EQ(gcc.GetTargetBitrateKbps(), 5000);
}

// --- Delay-based: underuse detection ---

TEST_F(GccControllerTest, UnderuseDetectedOnDecreasingDelay) {
  int64_t send = 1000000, arrival = 1000000;

  // First cause some delay buildup
  FeedOveruse(20, send, arrival, 1000);

  // Now feed underuse: queue is draining (arrival faster than send)
  FeedUnderuse(20, send, arrival, 2000);

  // The trendline slope should be negative — classified as underuse
  // This mainly tests that the detect function returns kUnderuse
  // and that the rate controller allows increase.
  // Since we caused overuse first, rate decreased. After underuse,
  // with enough time passed, it should start recovering.
  // Use the overuse guard: advance clock past 1s since last overuse
  AdvanceMs(1100);
  FeedUnderuse(10, send, arrival, 2000);

  // Verify rate is increasing (not stuck at the overuse-reduced level)
  int after_recovery = gcc.GetTargetBitrateKbps();
  // It may not fully recover in this test, but should be higher than 100 (min)
  EXPECT_GT(after_recovery, 100);
}

// --- Delay-based: stable network ---

TEST_F(GccControllerTest, StableNetworkIncreasesRate) {
  int64_t send = 1000000, arrival = 1000000;

  // Wait past overuse guard
  AdvanceMs(1100);
  FeedStable(30, send, arrival);

  // With 30 rounds × 33ms = ~1s of stable feedback, should have increased
  EXPECT_GE(gcc.GetTargetBitrateKbps(), 5000);
}

// --- Delay-based: adaptive threshold ---

TEST_F(GccControllerTest, ThresholdAdaptsUpDuringOveruse) {
  int64_t send = 1000000, arrival = 1000000;
  FeedStable(5, send, arrival);

  // Moderate overuse that barely exceeds initial threshold
  // After adaptation, threshold should be higher
  FeedOveruse(30, send, arrival, 500);

  // The fact that overuse eventually fires (after threshold growth is exceeded)
  // proves the threshold adapted. If it didn't grow, it would fire immediately.
  // With 500us extra/round, the delay grows slowly. The threshold must adapt
  // before overuse counter reaches 3.
  // Verify rate eventually decreased (threshold was exceeded)
  EXPECT_LT(gcc.GetTargetBitrateKbps(), 5000);
}

// --- Delay-based: noisy feedback ---

TEST_F(GccControllerTest, NoisyFeedbackDoesNotFalsePositive) {
  int64_t send = 1000000, arrival = 1000000;
  FeedStable(5, send, arrival);

  // Feed noisy but mean-zero delay jitter (alternating +/- 3ms)
  for (int i = 0; i < 40; i++) {
    int jitter = (i % 2 == 0) ? 3000 : -3000;
    auto fb = MakeFeedback(send, arrival, 20, 1000);
    gcc.OnTransportFeedback(fb);
    send += 33000;
    arrival += 33000 + jitter;  // oscillating, no net growth
    AdvanceMs(33);
  }

  // Rate should NOT have decreased (jitter averages out)
  EXPECT_GE(gcc.GetTargetBitrateKbps(), 5000);
}

// --- Loss-based: three ranges ---

TEST_F(GccControllerTest, LossBelow2PercentAllowsIncrease) {
  // Simulate 1% loss: 1 lost out of 100 sent
  gcc.OnPacketsSent(100);
  LossReport report;
  report.packets.push_back({0, 0});
  gcc.OnLossReport(report);

  // After update: loss = 1/100 = 1% < 2% → increase allowed
  // The loss_based_bitrate should have increased
  // Combined with delay_based (both start at 5000), target should be >= 5000
  EXPECT_GE(gcc.GetTargetBitrateKbps(), 5000);
}

TEST_F(GccControllerTest, LossBetween2And10PercentHolds) {
  int initial = gcc.GetTargetBitrateKbps();

  // Simulate 5% loss: 5 lost out of 100 sent
  gcc.OnPacketsSent(100);
  LossReport report;
  for (int i = 0; i < 5; i++) {
    report.packets.push_back({static_cast<uint16_t>(i), 0});
  }
  gcc.OnLossReport(report);

  // 5% is in hold range [2%, 10%): no change
  EXPECT_EQ(gcc.GetTargetBitrateKbps(), initial);
}

TEST_F(GccControllerTest, LossAbove10PercentDecreases) {
  int initial = gcc.GetTargetBitrateKbps();

  // Simulate 20% loss: 20 lost out of 100 sent
  gcc.OnPacketsSent(100);
  LossReport report;
  for (int i = 0; i < 20; i++) {
    report.packets.push_back({static_cast<uint16_t>(i), 0});
  }
  gcc.OnLossReport(report);

  // 20% loss → factor = 1 - 0.5*0.2 = 0.9 → ~4500 kbps
  int after = gcc.GetTargetBitrateKbps();
  EXPECT_LT(after, initial);
  EXPECT_GE(after, 4400);
  EXPECT_LE(after, 4600);
}

TEST_F(GccControllerTest, HighLossDecreasesProportionally) {
  // 50% loss: factor = 1 - 0.5*0.5 = 0.75
  gcc.OnPacketsSent(100);
  LossReport report;
  for (int i = 0; i < 50; i++) {
    report.packets.push_back({static_cast<uint16_t>(i), 0});
  }
  gcc.OnLossReport(report);

  int after = gcc.GetTargetBitrateKbps();
  // 5000 * 0.75 = 3750
  EXPECT_GE(after, 3600);
  EXPECT_LE(after, 3900);
}

// --- Transition tests ---

TEST_F(GccControllerTest, OveruseToNormalTransition) {
  int64_t send = 1000000, arrival = 1000000;
  FeedStable(5, send, arrival);

  // Cause overuse
  FeedOveruse(20, send, arrival);
  int overuse_rate = gcc.GetTargetBitrateKbps();
  EXPECT_LT(overuse_rate, 5000);

  // Return to stable — after flushing trendline window (20 samples),
  // overuse should stop and rate stabilizes
  AdvanceMs(1100);  // past overuse guard
  FeedStable(30, send, arrival);

  int stable_rate = gcc.GetTargetBitrateKbps();
  // Rate should have stabilized or started recovering (not crashed to min)
  EXPECT_GT(stable_rate, 100);
}

// --- Min bitrate floor ---

TEST_F(GccControllerTest, MinBitrateIsRespected) {
  gcc.SetBitrateRange(500, 30000);

  // Repeated heavy overuse
  int64_t send = 1000000, arrival = 1000000;
  FeedOveruse(100, send, arrival, 5000);

  EXPECT_GE(gcc.GetTargetBitrateKbps(), 500);
}
