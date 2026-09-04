#include <gtest/gtest.h>

#include "transmission/gcc_controller.h"
#include "transmission/packet_header.h"
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
      info.recv_size = 1454;  // full-MTU packet (real wire size)
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

TEST_F(GccControllerTest, CongestionWindowRttBaselineIsFeedbackFrequencyInvariant) {
  gcc.SetBitrateRange(100, 5000);
  gcc.SetInitialBitrate(5000);
  gcc.SetCongestionWindowConfig(50, 30);

  // Establish a 10 ms propagation-RTT sample. Then provide more than the old
  // 100-sample cap's worth of high queued-RTT feedback within one second.
  // A count-limited history discards the baseline and inflates the data window;
  // an elapsed-time history retains it regardless of callback frequency.
  auto feed_rtt = [&](int64_t rtt_ms, int sequence) {
    TransportFeedback fb;
    TransportFeedback::PacketInfo packet;
    packet.frame_sequence = static_cast<uint16_t>(sequence);
    packet.packet_index = 0;
    packet.send_time_us = (clock_ms_ - rtt_ms) * 1000;
    packet.arrival_time_us = clock_ms_ * 1000;
    packet.recv_size = 1454;
    fb.packets.push_back(packet);
    gcc.OnTransportFeedback(fb);
    AdvanceMs(1);
  };

  feed_rtt(10, 0);
  int64_t baseline_window = gcc.GetDataWindowBytesForTesting();
  ASSERT_GT(baseline_window, 0);
  for (int i = 1; i <= 150; ++i) {
    feed_rtt(500, i);
  }

  EXPECT_LT(gcc.GetDataWindowBytesForTesting(), baseline_window * 2);
}

// --- Delay-based: overuse detection ---

TEST_F(GccControllerTest, OveruseDetectedOnGrowingDelay) {
  int64_t send = 1000000, arrival = 1000000;
  FeedStable(5, send, arrival);  // warmup
  int before = gcc.GetTargetBitrateKbps();

  FeedOveruse(30, send, arrival);

  EXPECT_LT(gcc.GetTargetBitrateKbps(), before);
}

TEST_F(GccControllerTest, DelayOnlyOveruseUsesSmoothedAckedRate) {
  int64_t send = 1000000, arrival = 1000000;
  FeedStable(5, send, arrival);

  // No OnBytesSent calls means this is a delay-only signal, not a confirmed
  // byte-delivery cliff. Pin the smoothed throughput at 4 Mbps; the reduction
  // should be beta * 4 Mbps rather than a burst-sensitive feedback-batch rate.
  gcc.SetAckedBitrateForTesting(4000);
  for (int i = 0; i < 30 && gcc.GetDelayBasedBitrateKbps() >= 5000; ++i) {
    auto fb = MakeFeedback(send, arrival, 3, 1000, 5000);
    gcc.OnTransportFeedback(fb);
    send += 33000;
    arrival += 45000;  // queue grows by another 12ms each round
    AdvanceMs(33);
  }

  EXPECT_GE(gcc.GetDelayBasedBitrateKbps(), 3300);
  EXPECT_LE(gcc.GetDelayBasedBitrateKbps(), 3400);
}

TEST_F(GccControllerTest, SourceLimitedTrendDoesNotRatchetBitrateDown) {
  int64_t send = 1000000, arrival = 1000000;
  gcc.SetAckedBitrateForTesting(3500);

  // Produce about 3.4 Mbps against a 5 Mbps estimate.  The receiver keeps up,
  // so this is encoder/content limitation rather than a capacity shortage.
  // Build enough real sent-byte history to make that distinction observable.
  for (int i = 0; i < 8; ++i) {
    gcc.OnBytesSent(14000);
    auto fb = MakeFeedback(send, arrival, 12, 2000, 2000);
    for (auto& packet : fb.packets) packet.recv_size = 1166;
    gcc.OnTransportFeedback(fb);
    send += 33000;
    arrival += 33000;
    AdvanceMs(33);
  }
  int before = gcc.GetDelayBasedBitrateKbps();

  // Exercise a codec-shaped positive delay trend while the aggregate sent and
  // acknowledged rates remain aligned.  It may classify timing as overuse,
  // but must not feed beta*application_rate back into the target.
  for (int i = 0; i < 30; ++i) {
    gcc.OnBytesSent(14000);
    auto fb = MakeFeedback(send, arrival, 12, 2000, 3000);
    for (auto& packet : fb.packets) packet.recv_size = 1166;
    gcc.OnTransportFeedback(fb);
    send += 33000;
    arrival += 33000;
    AdvanceMs(33);
  }

  EXPECT_GE(gcc.GetDelayBasedBitrateKbps(), before);
}

TEST_F(GccControllerTest, SourceLimitedGuardDoesNotHideDeliveryShortage) {
  int64_t send = 1000000, arrival = 1000000;
  gcc.SetAckedBitrateForTesting(1000);

  // The sender is still below 80% of target, but ACK delivery is far below
  // even the actual sent rate.  This is real congestion, so trend overuse must
  // remain actionable rather than being mistaken for source limitation.
  for (int i = 0; i < 8; ++i) {
    gcc.OnBytesSent(14000);
    auto fb = MakeFeedback(send, arrival, 12, 2000, 2000);
    for (auto& packet : fb.packets) packet.recv_size = 1166;
    gcc.OnTransportFeedback(fb);
    send += 33000;
    arrival += 33000;
    AdvanceMs(33);
  }
  int before = gcc.GetDelayBasedBitrateKbps();

  for (int i = 0; i < 80 && gcc.GetDelayBasedBitrateKbps() >= before; ++i) {
    gcc.OnBytesSent(14000);
    auto fb = MakeFeedback(send, arrival, 12, 2000, 5000);
    for (auto& packet : fb.packets) packet.recv_size = 500;
    gcc.OnTransportFeedback(fb);
    send += 33000;
    arrival += 55000;
    AdvanceMs(55);
  }

  EXPECT_LT(gcc.GetDelayBasedBitrateKbps(), before);
}

TEST_F(GccControllerTest, CapacityDropDetectedWithinTwoFeedbackIntervals) {
  int64_t send = 1000000, arrival = 1000000;
  FeedStable(8, send, arrival);
  int before = gcc.GetDelayBasedBitrateKbps();

  // Model the 10 -> 1 Mbps cliff: send spacing remains 1ms while arrivals
  // serialize roughly every 12ms. With a 50ms trendline horizon, GCC should
  // react within two 33ms feedback intervals instead of waiting ~100ms just
  // to obtain its first fitted slope.
  gcc.SetAckedBitrateForTesting(1000);
  int detected_after_ms = -1;
  for (int round = 0; round < 2; ++round) {
    auto fb = MakeFeedback(send, arrival, 20, 1000, 12000);
    gcc.OnTransportFeedback(fb);
    send += 33000;
    arrival += 240000;
    AdvanceMs(33);
    if (gcc.GetDelayBasedBitrateKbps() < before) {
      detected_after_ms = (round + 1) * 33;
      break;
    }
  }

  EXPECT_GT(detected_after_ms, 0);
  EXPECT_LE(detected_after_ms, 66);
}

TEST_F(GccControllerTest, SevereCapacityCliffUsesFirstDecisiveGroup) {
  // Keep the controller at a high adaptive estimate without starting probes.
  // The source below sends only ~7 Mbps (<80% of the 10 Mbps target), modelling
  // a VBR/default-x264 encoder that is application-limited relative to target
  // but can still badly overload a collapsed 1 Mbps path.
  gcc.SetBitrateRange(100, 10000);
  gcc.SetInitialBitrate(10000);
  gcc.EnableAckedEstimatorForTesting();
  int64_t send = 1000000, arrival = 1000000;

  // Build stable byte-rate and trendline histories while the sender is filling
  // its target. OnBytesSent is required to arm the byte-delivery detector.
  for (int round = 0; round < 8; ++round) {
    gcc.OnBytesSent(20 * 1454);
    auto fb = MakeFeedback(send, arrival, 20, 1000, 1000);
    gcc.OnTransportFeedback(fb);
    send += 33000;
    arrival += 33000;
    AdvanceMs(33);
  }
  int before = gcc.GetDelayBasedBitrateKbps();

  // The first post-drop group models packets sent 1ms apart but serialized at
  // 12ms by a 1Mbps link. It simultaneously establishes a severe byte-rate
  // collapse and a large positive delay trend. That is enough evidence for the
  // cliff fast path; it must not wait for the ordinary 200ms timer.
  gcc.OnBytesSent(7 * 1454);
  auto cliff = MakeFeedback(send, arrival, 7, 1000, 12000);
  gcc.OnTransportFeedback(cliff);

  EXPECT_LT(gcc.GetDelayBasedBitrateKbps(), before);
}

TEST_F(GccControllerTest, SinglePacketFeedbackUsesWindowRateOnOveruse) {
  gcc.EnableAckedEstimatorForTesting();
  int64_t send = 1000000, arrival = 1000000;

  // Establish a stable, high-rate history using ordinary multi-packet
  // feedback. This also leaves a high instantaneous batch rate behind.
  FeedStable(8, send, arrival);
  int before = gcc.GetDelayBasedBitrateKbps();

  // A strict 10ms feedback timer at a 1Mbps bottleneck commonly reports one
  // packet at a time. Arrival spacing expands to 12ms while send spacing stays
  // at 3ms. The controller must not reuse the stale pre-drop instantaneous
  // batch rate when overuse is detected.
  for (int i = 0; i < 30 && gcc.GetDelayBasedBitrateKbps() >= before; ++i) {
    TransportFeedback fb;
    TransportFeedback::PacketInfo pkt;
    pkt.frame_sequence = static_cast<uint16_t>(100 + i / 10);
    pkt.packet_index = static_cast<uint8_t>(i % 10);
    pkt.send_time_us = send;
    pkt.arrival_time_us = arrival;
    pkt.recv_size = 1454;
    fb.packets.push_back(pkt);
    gcc.OnTransportFeedback(fb);
    send += 3000;
    arrival += 12000;
    AdvanceMs(12);
  }

  EXPECT_LT(gcc.GetDelayBasedBitrateKbps(), before);
  // Delay-only overuse uses the smoothed 200ms ACK window. The instantaneous
  // suffix is around 1 Mbps here, but is intentionally reserved for a
  // byte-confirmed severe cliff.
  EXPECT_GT(gcc.GetDelayBasedBitrateKbps(), 3000);
  EXPECT_LT(gcc.GetDelayBasedBitrateKbps(), 4500);
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

  // Probe initiates on batch 1 and registers on batch 2. Batch 2 is excluded
  // because it was already in flight when activation was observed; batches
  // 3-4 provide >30ms of actual post-activation traffic. Four feedback rounds
  // are still far under the old 300ms fixed window.
  int64_t send = 1000000, arrival = 1000000;
  FeedStable(4, send, arrival);

  EXPECT_GT(gcc.GetDelayBasedBitrateKbps(), before);
}

TEST_F(GccControllerTest, ProbeExcludesTrafficSentBeforeActivation) {
  gcc.SetInitialBitrate(1000);
  gcc.SetBitrateRange(100, 30000);
  gcc.SetAckedBitrateForTesting(30000);

  int64_t send = 1000000, arrival = 1000000;
  // Batch 1 initiates the 3x probe.
  auto first = MakeFeedback(send, arrival, 20, 1000);
  gcc.OnTransportFeedback(first);
  send += 33000;
  arrival += 33000;
  AdvanceMs(33);

  // This large batch spans enough arrival time to satisfy the probe estimator,
  // but it is the batch on which activation is first observed. It must be
  // treated as pre-probe traffic and must not commit a codec-dependent rate.
  auto in_flight = MakeFeedback(send, arrival, 40, 1000);
  gcc.OnTransportFeedback(in_flight);
  // Ordinary AIMD may add a few kbps while the network is stable, but the
  // 3x probe target must not have been committed from this in-flight batch.
  EXPECT_LT(gcc.GetDelayBasedBitrateKbps(), 1100);

  // Freshly sent traffic after the boundary may resolve the probe normally.
  send += 50000;
  arrival += 50000;
  AdvanceMs(50);
  FeedStable(2, send, arrival);
  EXPECT_GT(gcc.GetDelayBasedBitrateKbps(), 1000);
}

TEST_F(GccControllerTest, ProbeMeasuresExplicitPaddingWithoutSendTimestamp) {
  gcc.SetInitialBitrate(1000);
  gcc.SetBitrateRange(100, 30000);
  gcc.SetAckedBitrateForTesting(30000);

  int64_t send = 1000000, arrival = 1000000;
  // Initiate, then observe activation on an already-in-flight media batch.
  auto first = MakeFeedback(send, arrival, 20, 1000);
  gcc.OnTransportFeedback(first);
  send += 33000;
  arrival += 33000;
  AdvanceMs(33);
  auto activation = MakeFeedback(send, arrival, 20, 1000);
  gcc.OnTransportFeedback(activation);

  // Pacer padding is reported by the receiver but deliberately has no stored
  // send timestamp, because it must not enter the delay trendline. Its reserved
  // frame sequence still identifies it as actual probe traffic.
  for (int round = 0; round < 2; ++round) {
    TransportFeedback padding;
    for (int i = 0; i < 20; ++i) {
      TransportFeedback::PacketInfo pkt;
      pkt.frame_sequence = kPaddingFrameSequence;
      pkt.packet_index = static_cast<uint8_t>(round * 20 + i);
      pkt.send_time_us = -1;
      pkt.arrival_time_us = arrival + i * 1000;
      pkt.recv_size = 1454;
      padding.packets.push_back(pkt);
    }
    gcc.OnTransportFeedback(padding);
    arrival += 33000;
    AdvanceMs(33);
  }

  EXPECT_GT(gcc.GetDelayBasedBitrateKbps(), 1000);
}

TEST_F(GccControllerTest, ProbeIntoSaturatedLinkAbortsWithoutOvershoot) {
  gcc.SetInitialBitrate(1000);
  gcc.SetBitrateRange(100, 30000);
  gcc.EnableAckedEstimatorForTesting();  // probe measures real bytes/span

  // Model a link that cannot carry the probe: arrivals lag sends (arrival
  // spacing 5816us vs send spacing 1000us), so every packet adds ~4.8ms of
  // queuing delay — a growing queue. The 3x probe target is 3000, but the link
  // only delivers ~2000. With the overuse timer driven by arrival-time deltas,
  // the measured probe receive rate stays below its send target. Startup must
  // use that byte measurement rather than a codec-shaped delay-only abort, and
  // still must not commit the over-target 3000 kbps send rate.
  const int64_t kArrivalSpacingUs = 5816;
  int64_t send = 1000000, arrival = 1000000;
  for (int i = 0; i < 4; i++) {
    auto fb = MakeFeedback(send, arrival, 20, 1000, kArrivalSpacingUs);
    gcc.OnTransportFeedback(fb);
    send += 33000;
    arrival += 20 * kArrivalSpacingUs;
    AdvanceMs(33);
  }

  // Saturated measurement → no overshoot beyond the 3000 probe target.
  EXPECT_LE(gcc.GetDelayBasedBitrateKbps(), 3000);
}

// --- Acked throughput: sliding window robust to bursty arrivals ---

TEST_F(GccControllerTest, AckedWindowNotInflatedByBurstyArrivals) {
  // Don't freeze acked — exercise the real sliding-window estimator.
  gcc.SetInitialBitrate(5000);
  gcc.SetBitrateRange(100, 30000);
  gcc.EnableAckedEstimatorForTesting();  // undo SetUp's frozen acked

  // Feed 1s of feedback where packets are paced ~1ms apart (≈ 11.6 Mbps of
  // full-MTU packets), BUT inject one batch whose packets all arrive bunched
  // within a tiny span (the pathological case that used to read ~26 Mbps from
  // a single batch). With a 1s window the burst can't dominate: the windowed
  // rate must stay near the real ~11-12 Mbps, not spike to 25+.
  int64_t send = 1000000, arrival = 1000000;
  // Normal paced batches: 20 pkts, 1ms arrival spacing.
  for (int r = 0; r < 20; r++) {
    auto fb = MakeFeedback(send, arrival, 20, 1000, 1000);
    gcc.OnTransportFeedback(fb);
    send += 20000;
    arrival += 20000;
    AdvanceMs(20);
  }
  double steady = gcc.GetAckedBitrateKbpsForTesting();
  EXPECT_GT(steady, 8000);    // measuring a real multi-Mbps rate
  EXPECT_LT(steady, 16000);   // ~11.6 Mbps for 1454B @ 1ms, not inflated

  // Now a bunched batch: 20 pkts arriving only 50us apart (≈ 232 Mbps if taken
  // alone). The window must absorb it without exploding.
  auto burst = MakeFeedback(send, arrival, 20, 1000, 50);
  gcc.OnTransportFeedback(burst);
  double after_burst = gcc.GetAckedBitrateKbpsForTesting();
  EXPECT_LT(after_burst, 20000);  // not the ~232 Mbps the burst alone implies
}

TEST_F(GccControllerTest, MatchedPacketCohortDoesNotInventByteShortfall) {
  gcc.SetInitialBitrate(5000);
  gcc.SetBitrateRange(100, 30000);
  gcc.EnableAckedEstimatorForTesting();

  int before = gcc.GetDelayBasedBitrateKbps();
  int64_t send = 1000000, arrival = 1000000;
  for (int round = 0; round < 10; ++round) {
    // Deliberately make the aggregate OnBytesSent accounting disagree with
    // the acknowledged packet sizes while preserving identical send/arrival
    // timing. The delivery detector now compares the same acknowledged packet
    // cohort, so aggregation differences alone are not network congestion.
    gcc.OnBytesSent(20 * 1454);
    auto fb = MakeFeedback(send, arrival, 20, 1000, 1000);
    for (auto& packet : fb.packets) {
      packet.recv_size = 500;
    }
    gcc.OnTransportFeedback(fb);
    send += 33000;
    arrival += 33000;
    AdvanceMs(33);
  }

  EXPECT_EQ(gcc.GetDelayBasedBitrateKbps(), before);
}

TEST_F(GccControllerTest, PersistentBacklogCausesOneReductionPerEpisode) {
  gcc.SetInitialBitrate(5000);
  gcc.SetBitrateRange(100, 5000);  // keep startup probing out of the test
  gcc.SetCongestionWindowConfig(0, 30);
  gcc.EnableAckedEstimatorForTesting();

  int64_t send = 1000000, arrival = 1000000;
  for (int round = 0; round < 10; ++round) {
    gcc.OnBytesSent(20 * 1454);
    gcc.OnTransportFeedback(MakeFeedback(send, arrival, 20, 1000, 1000));
    send += 33000;
    arrival += 33000;
    AdvanceMs(33);
  }

  const int before = gcc.GetDelayBasedBitrateKbps();
  int after_first_reduction = before;
  for (int round = 0; round < 30; ++round) {
    gcc.OnBytesSent(20 * 1454);
    gcc.OnTransportFeedback(MakeFeedback(send, arrival, 20, 1000, 2200));
    send += 33000;
    arrival += 44000;
    AdvanceMs(44);
    if (gcc.GetDelayBasedBitrateKbps() < before) {
      after_first_reduction = gcc.GetDelayBasedBitrateKbps();
      break;
    }
  }
  ASSERT_LT(after_first_reduction, before);

  // Continue feeding packets from the same growing queue for well beyond the
  // byte detector's old 100ms rearm interval. The permanent estimate must not
  // ratchet down again; temporary draining belongs to cwnd pushback.
  for (int round = 0; round < 20; ++round) {
    gcc.OnBytesSent(20 * 1454);
    gcc.OnTransportFeedback(MakeFeedback(send, arrival, 20, 1000, 2200));
    send += 33000;
    arrival += 44000;
    AdvanceMs(44);
  }
  EXPECT_EQ(gcc.GetDelayBasedBitrateKbps(), after_first_reduction);

  // Once the same packet cohort is delivered normally for the recovery
  // hysteresis interval, the next independently growing queue is a new
  // congestion episode and may reduce the permanent estimate once more.
  for (int round = 0; round < 20; ++round) {
    gcc.OnBytesSent(20 * 1454);
    gcc.OnTransportFeedback(MakeFeedback(send, arrival, 20, 1000, 1000));
    send += 33000;
    arrival += 33000;
    AdvanceMs(33);
  }
  for (int round = 0; round < 30 &&
                      gcc.GetDelayBasedBitrateKbps() >= after_first_reduction;
       ++round) {
    gcc.OnBytesSent(20 * 1454);
    gcc.OnTransportFeedback(MakeFeedback(send, arrival, 20, 1000, 2200));
    send += 33000;
    arrival += 44000;
    AdvanceMs(44);
  }
  EXPECT_LT(gcc.GetDelayBasedBitrateKbps(), after_first_reduction);
}

TEST_F(GccControllerTest, InflightBytesRetireByAckLossAndTimeout) {
  // Two packets are retired deterministically by their transport identity,
  // independent of their unequal wire sizes.
  gcc.OnPacketSent(10, 0, 1460);
  gcc.OnPacketSent(10, 1, 500);
  EXPECT_EQ(gcc.GetOutstandingBytesForTesting(), 1960);

  auto ack = MakeFeedback(clock_ms_ * 1000 - 50000, 1000000, 1, 1000);
  ack.packets[0].frame_sequence = 10;
  ack.packets[0].packet_index = 1;
  ack.packets[0].recv_size = 500;
  gcc.OnTransportFeedback(ack);
  EXPECT_EQ(gcc.GetOutstandingBytesForTesting(), 1460);

  LossReport loss;
  loss.packets.push_back({10, 0});
  gcc.OnLossReport(loss);
  EXPECT_EQ(gcc.GetOutstandingBytesForTesting(), 0);

  // Probe padding has no frame-completion loss report. If it disappears, an
  // RTT-scaled liveness timeout must release it instead of pinning cwnd
  // pushback forever (the failure observed in real-trace slice trial 5).
  gcc.OnPacketSent(kPaddingFrameSequence, 7, 1460);
  EXPECT_EQ(gcc.GetOutstandingBytesForTesting(), 1460);
  AdvanceMs(1999);
  gcc.OnTransportFeedback(TransportFeedback{});
  EXPECT_EQ(gcc.GetOutstandingBytesForTesting(), 1460);
  AdvanceMs(1);
  gcc.OnTransportFeedback(TransportFeedback{});
  EXPECT_EQ(gcc.GetOutstandingBytesForTesting(), 0);
}
