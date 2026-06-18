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
    // Default: high acked throughput so the AIMD increase cap (1.5x acked)
    // doesn't bind during ramp/stable tests. Overuse tests override this with
    // a low value to model a saturated bottleneck (acked below the estimate).
    gcc.SetAckedBitrateForTesting(30000);
  }

  // Advance fake clock
  void AdvanceMs(int64_t ms) { clock_ms_ += ms; }

  // Create feedback with per-packet send and arrival times.
  // Send times are spaced uniformly from send_base_us, arrival times from
  // arrival_base_us, both with the given spacing.
  TransportFeedback MakeFeedback(int64_t send_base_us, int64_t arrival_base_us,
                                  int num_packets, int64_t send_spacing_us,
                                  int64_t arrival_spacing_us) {
    TransportFeedback fb;
    fb.reference_time_us = 0;
    for (int i = 0; i < num_packets; i++) {
      TransportFeedback::PacketInfo info;
      info.frame_sequence = static_cast<uint16_t>(i / 10);
      info.packet_index = static_cast<uint8_t>(i % 10);
      info.send_time_us = send_base_us + i * send_spacing_us;
      info.arrival_time_us = arrival_base_us + i * arrival_spacing_us;
      fb.packets.push_back(info);
    }
    return fb;
  }

  // Convenience: equal send and arrival spacing (no queuing delay change)
  TransportFeedback MakeFeedback(int64_t send_base_us, int64_t arrival_base_us,
                                  int num_packets, int64_t spacing_us) {
    return MakeFeedback(send_base_us, arrival_base_us, num_packets,
                        spacing_us, spacing_us);
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

  // Feed N rounds modelling a saturated bottleneck: arrival spacing exceeds
  // send spacing (queue building → growing one-way delay), and is wide enough
  // that the acknowledged throughput is below the current estimate — so the
  // AIMD decrease (beta * acked) actually lowers the rate, as on a real
  // congested link. spacing chosen so acked ~ a few Mbps.
  void FeedOveruse(int rounds, int64_t& send_time, int64_t& arrival_time,
                   int extra_per_round_us = 2000) {
    // A saturated bottleneck means the acknowledged throughput sits below the
    // current estimate; pin it low so the AIMD decrease (beta * acked) lowers
    // the rate, as WebRTC does on real congestion.
    gcc.SetAckedBitrateForTesting(2000);
    for (int i = 0; i < rounds; i++) {
      int64_t arrival_spacing = 1000 + extra_per_round_us / 20;
      auto fb = MakeFeedback(send_time, arrival_time, 20,
                              1000, arrival_spacing);
      gcc.OnTransportFeedback(fb);
      send_time += 33000;
      arrival_time += 33000 + extra_per_round_us;
      AdvanceMs(33);
    }
  }

  // Feed N rounds where arrival spacing is less than send spacing (queue draining)
  void FeedUnderuse(int rounds, int64_t& send_time, int64_t& arrival_time,
                    int catch_up_per_round_us = 2000) {
    for (int i = 0; i < rounds; i++) {
      // Within batch: arrive faster than sent (queue draining)
      int64_t arrival_spacing = 1000 - catch_up_per_round_us / 20;
      if (arrival_spacing < 100) arrival_spacing = 100;
      auto fb = MakeFeedback(send_time, arrival_time, 20,
                              1000, arrival_spacing);
      gcc.OnTransportFeedback(fb);
      send_time += 33000;
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
  AdvanceMs(600);
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

  // Multiple 0.85x decreases fire. With per-packet processing (20 packets
  // per batch × 15 batches = 300 samples), overuse triggers frequently.
  EXPECT_LT(after_overuse, 5000);
  EXPECT_GT(after_overuse, 1000);  // Not crashed to min
  // Verify multiplicative pattern
  double ratio = after_overuse / 5000.0;
  EXPECT_LT(ratio, 0.85);  // at least one 0.85x decrease
}

TEST_F(GccControllerTest, StartupWarmupPreventsEarlyOveruse) {
  // The first 10 delay samples have reduced sensitivity (scaled by
  // num_deltas/10). Feed a small batch (5 packets) with heavy overuse;
  // should not trigger because scale factor < 1.
  int64_t send = 1000000, arrival = 1000000;
  auto fb = MakeFeedback(send, arrival, 5, 1000, 2000);  // 2x arrival spacing
  gcc.OnTransportFeedback(fb);

  // With only ~4 deltas, scaling reduces modified_trend significantly, so no
  // overuse fires. Assert on the delay-based component (the startup prober may
  // raise the combined target independently of overuse detection).
  EXPECT_EQ(gcc.GetDelayBasedBitrateKbps(), 5000);
  EXPECT_EQ(gcc.GetOveruseCounter(), 0);
}

// --- Delay-based: underuse detection ---

TEST_F(GccControllerTest, UnderuseDetectedOnDecreasingDelay) {
  int64_t send = 1000000, arrival = 1000000;

  // Warm up so num_deltas builds (matches WebRTC modified_trend scaling).
  FeedStable(5, send, arrival);

  // Cause overuse with the default sustained congestion signal (proven to
  // trigger in OveruseDetectedOnGrowingDelay).
  FeedOveruse(30, send, arrival);
  int post_overuse = gcc.GetDelayBasedBitrateKbps();
  EXPECT_LT(post_overuse, 5000);  // confirm overuse happened

  // Now feed underuse: queue draining. Throughput has recovered, so the acked
  // estimate is high again (no longer capping the increase).
  AdvanceMs(1100);  // past overuse guard
  gcc.SetAckedBitrateForTesting(30000);
  FeedUnderuse(40, send, arrival, 3000);

  // After underuse with time passed, rate should have increased from post-overuse
  int after_underuse = gcc.GetDelayBasedBitrateKbps();
  EXPECT_GT(after_underuse, post_overuse);
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

  // After stable period, threshold has decayed toward kMinThreshold (6ms)
  double pre_overuse_threshold = gcc.GetAdaptiveThreshold();
  EXPECT_LE(pre_overuse_threshold, 12.5);  // decayed from initial
  EXPECT_GE(pre_overuse_threshold, 6.0);   // but not below min

  // Feed overuse — threshold should grow back up
  FeedOveruse(20, send, arrival, 2000);

  double adapted_threshold = gcc.GetAdaptiveThreshold();
  EXPECT_GT(adapted_threshold, pre_overuse_threshold);
  EXPECT_LE(adapted_threshold, 600.0);
}

// --- Delay-based: noisy feedback ---

TEST_F(GccControllerTest, NoisyFeedbackDoesNotFalsePositive) {
  int64_t send = 1000000, arrival = 1000000;
  FeedStable(10, send, arrival);  // warmup to fill trendline window

  int before = gcc.GetDelayBasedBitrateKbps();

  // Feed noisy but mean-zero delay jitter (alternating +/- 1ms)
  // This is well within the adaptive threshold (initial 12.5ms)
  for (int i = 0; i < 40; i++) {
    int jitter = (i % 2 == 0) ? 1000 : -1000;
    auto fb = MakeFeedback(send, arrival, 20, 1000);
    gcc.OnTransportFeedback(fb);
    send += 33000;
    arrival += 33000 + jitter;
    AdvanceMs(33);
  }

  // Delay-based rate should NOT have decreased (no overuse triggered)
  EXPECT_GE(gcc.GetDelayBasedBitrateKbps(), before);
  EXPECT_EQ(gcc.GetOveruseCounter(), 0);
}

// --- Loss-based: three ranges ---

TEST_F(GccControllerTest, LossBelow2PercentAllowsIncrease) {
  // Simulate 1% loss: 1 lost out of 100 sent
  gcc.OnPacketsSent(100);
  LossReport report;
  report.packets.push_back({0, 0});
  AdvanceMs(600);  // pass the periodic loss-update interval (500ms)
  gcc.OnLossReport(report);

  // After update: loss = 1/100 = 1% < 2% → loss-based tracks delay-based.
  // Assert on the loss-based component (the combined target may be raised by
  // the startup prober independently of the loss path).
  EXPECT_GE(gcc.GetLossBasedBitrateKbps(), 5000);
}

TEST_F(GccControllerTest, LossBetween2And10PercentHolds) {
  int initial = gcc.GetLossBasedBitrateKbps();

  // Simulate 5% loss: 5 lost out of 100 sent
  gcc.OnPacketsSent(100);
  LossReport report;
  for (int i = 0; i < 5; i++) {
    report.packets.push_back({static_cast<uint16_t>(i), 0});
  }
  AdvanceMs(600);
  gcc.OnLossReport(report);

  // 5% is in hold range [2%, 10%): loss-based unchanged.
  EXPECT_EQ(gcc.GetLossBasedBitrateKbps(), initial);
}

TEST_F(GccControllerTest, LossAbove10PercentDecreases) {
  int initial = gcc.GetLossBasedBitrateKbps();

  // Simulate 20% loss: 20 lost out of 100 sent
  gcc.OnPacketsSent(100);
  LossReport report;
  for (int i = 0; i < 20; i++) {
    report.packets.push_back({static_cast<uint16_t>(i), 0});
  }
  AdvanceMs(600);
  gcc.OnLossReport(report);

  // 20% loss → factor = 1 - 0.5*0.2 = 0.9 → ~4500 kbps
  int after = gcc.GetLossBasedBitrateKbps();
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
  AdvanceMs(600);
  gcc.OnLossReport(report);

  int after = gcc.GetLossBasedBitrateKbps();
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
  int overuse_rate = gcc.GetDelayBasedBitrateKbps();
  EXPECT_LT(overuse_rate, 5000);

  // Return to stable — flush trendline window (need >20 stable samples)
  // then verify no further overuse and rate starts recovering
  AdvanceMs(1100);  // past overuse guard
  FeedStable(25, send, arrival);  // flush window with zero-growth data

  int rate_after_flush = gcc.GetDelayBasedBitrateKbps();

  // Feed more stable data — rate should now increase (no more overuse)
  FeedStable(15, send, arrival);
  int rate_after_recovery = gcc.GetDelayBasedBitrateKbps();

  // After window flush, rate should be recovering (not decreasing)
  EXPECT_GE(rate_after_recovery, rate_after_flush);
  // And overuse counter should be back to 0
  EXPECT_EQ(gcc.GetOveruseCounter(), 0);
}

// --- Min bitrate floor ---

TEST_F(GccControllerTest, MinBitrateIsRespected) {
  gcc.SetBitrateRange(500, 30000);

  // Repeated heavy overuse
  int64_t send = 1000000, arrival = 1000000;
  FeedOveruse(100, send, arrival, 5000);

  EXPECT_GE(gcc.GetTargetBitrateKbps(), 500);
}

// --- Probe resolution (WebRTC: commit from received rate, no fixed window) ---

TEST_F(GccControllerTest, ProbeCommitsFromReceivedRateWithoutWaiting) {
  gcc.SetInitialBitrate(1000);
  gcc.SetBitrateRange(100, 30000);
  // Received rate well above the probe target → the link can carry the probe,
  // so it should commit at the full probe target (3x = 3000).
  gcc.SetAckedBitrateForTesting(30000);

  int before = gcc.GetDelayBasedBitrateKbps();  // 1000

  // Probe initiates on batch 1, registers on batch 2, commits on batch 3 —
  // 3 batches × 33ms = 99ms, far under the old 300ms fixed window. The point:
  // commit is event-driven (first received-rate sample), not time-driven.
  int64_t send = 1000000, arrival = 1000000;
  FeedStable(3, send, arrival);

  EXPECT_GT(gcc.GetDelayBasedBitrateKbps(), before);
}

TEST_F(GccControllerTest, ProbeCommitsCapacityWhenLinkSaturated) {
  gcc.SetInitialBitrate(1000);
  gcc.SetBitrateRange(100, 30000);
  // Received rate BELOW the 3x probe target (3000): the link is saturated by
  // the probe. We should commit ~0.95 × received (≈ 1900), i.e. measured
  // capacity backed off slightly — never the over-target 3000 send rate.
  gcc.SetAckedBitrateForTesting(2000);

  int64_t send = 1000000, arrival = 1000000;
  FeedStable(3, send, arrival);

  int committed = gcc.GetDelayBasedBitrateKbps();
  EXPECT_GT(committed, 1000);   // a real gain over the start
  EXPECT_LE(committed, 2000);   // capped at measured capacity, not 3000
  EXPECT_GE(committed, 1800);   // ≈ 0.95 × 2000
}
