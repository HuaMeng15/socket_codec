#include <gtest/gtest.h>

#include "transmission/bandwidth_prober.h"

class BandwidthProberTest : public ::testing::Test {
 protected:
  BandwidthProber prober;
  int64_t clock_ms_ = 10000;

  void SetUp() override {
    prober.SetClockForTesting(&clock_ms_);
    prober.SetEstimatedBitrate(5000);
    prober.SetMaxBitrate(30000);
  }

  void AdvanceMs(int64_t ms) { clock_ms_ += ms; }
};

TEST_F(BandwidthProberTest, StartsIdle) {
  EXPECT_EQ(prober.GetState(), BandwidthProber::State::kIdle);
}

TEST_F(BandwidthProberTest, InitialExponentialProbeAt3x) {
  // SetClockForTesting sets last_overuse to clock-2000, so probing is allowed
  int rate = prober.GetEffectiveBitrateKbps();
  EXPECT_EQ(rate, 15000);  // 3x
  EXPECT_EQ(prober.GetState(), BandwidthProber::State::kProbing);
}

TEST_F(BandwidthProberTest, DoesNotProbeAfterRecentOveruse) {
  prober.OnOveruseDetected();  // sets last_overuse to now

  int rate = prober.GetEffectiveBitrateKbps();
  EXPECT_EQ(rate, 5000);
  EXPECT_EQ(prober.GetState(), BandwidthProber::State::kIdle);

  // After 1s it can probe again
  AdvanceMs(1100);
  rate = prober.GetEffectiveBitrateKbps();
  EXPECT_EQ(rate, 5000);  // initial probing done after overuse
  EXPECT_EQ(prober.GetState(), BandwidthProber::State::kIdle);
}

TEST_F(BandwidthProberTest, SuccessfulProbeTriggersNextProbe) {
  prober.GetEffectiveBitrateKbps();  // 3x probe at 15000
  auto probes = prober.GetPendingProbes();
  ASSERT_EQ(probes.size(), 1u);
  EXPECT_EQ(probes[0].target_bitrate_kbps, 15000);

  // Report success: estimated 12000 (12000/15000 = 0.8 > 0.7 threshold)
  prober.OnProbeResult(12000, true);
  // Should trigger further probe at 2x 12000 = 24000
  EXPECT_EQ(prober.GetState(), BandwidthProber::State::kProbing);

  auto probes2 = prober.GetPendingProbes();
  ASSERT_EQ(probes2.size(), 1u);
  EXPECT_EQ(probes2[0].target_bitrate_kbps, 24000);
}

TEST_F(BandwidthProberTest, SecondExponentialProbeAt6x) {
  // First probe at 3x = 15000
  prober.GetEffectiveBitrateKbps();
  auto probes1 = prober.GetPendingProbes();
  ASSERT_EQ(probes1.size(), 1u);
  EXPECT_EQ(probes1[0].target_bitrate_kbps, 15000);

  // Report success but below further_probe_threshold: 9000/15000 = 0.6 < 0.7
  // This means no further probe triggered, but initial probing continues
  // since exponential_probe_count_ (1) < kMaxExponentialProbes (2)
  prober.OnProbeResult(9000, true);
  // The result was success but ratio < 0.7, so no "further" probe.
  // But initial_probing_done_ is NOT set since count < max.
  // Actually looking at code: on success with ratio < threshold,
  // it falls through without setting initial_probing_done_.
  // State goes back to idle. Next GetEffective should trigger 2nd exponential.
  EXPECT_EQ(prober.GetState(), BandwidthProber::State::kIdle);

  // Second exponential probe at 6x (since probe_count is now 1)
  // Wait for min time
  AdvanceMs(1100);
  int rate = prober.GetEffectiveBitrateKbps();
  EXPECT_EQ(rate, 30000);  // 6 * 5000 = 30000 (or capped at max)
  EXPECT_EQ(prober.GetState(), BandwidthProber::State::kProbing);

  auto probes2 = prober.GetPendingProbes();
  ASSERT_EQ(probes2.size(), 1u);
  EXPECT_EQ(probes2[0].target_bitrate_kbps, 30000);
}

TEST_F(BandwidthProberTest, ExponentialChainRunsPastSeedProbesToSaturation) {
  // Regression: the further-probe chain must keep doubling from each measured
  // rate until the link saturates or we approach max — it must NOT stop after
  // the 2 seed probes. Previously initial_probing_done_ (set once the seed
  // count hit the cap) gated the chain off, capping convergence at the seeds.
  prober.SetMaxBitrate(30000);
  prober.SetEstimatedBitrate(1000);

  // Seed probe #1 at 3x = 3000.
  int r = prober.GetEffectiveBitrateKbps();
  EXPECT_EQ(r, 3000);
  prober.GetPendingProbes();
  // Strong result (2700/3000 = 0.9 >= 0.7) → chain to 2x = 5400.
  prober.OnProbeResult(2700, true);
  EXPECT_EQ(prober.GetState(), BandwidthProber::State::kProbing);
  auto p2 = prober.GetPendingProbes();
  ASSERT_EQ(p2.size(), 1u);
  EXPECT_EQ(p2[0].target_bitrate_kbps, 5400);

  // Chain continues: 4860/5400 = 0.9 → 2x = 9720.
  prober.OnProbeResult(4860, true);
  EXPECT_EQ(prober.GetState(), BandwidthProber::State::kProbing);
  auto p3 = prober.GetPendingProbes();
  ASSERT_EQ(p3.size(), 1u);
  EXPECT_EQ(p3[0].target_bitrate_kbps, 9720);

  // And again: 8748/9720 = 0.9 → 2x = 17496. This is the 4th probe overall,
  // well past the old 2-seed cap — proving the chain is uncapped.
  prober.OnProbeResult(8748, true);
  EXPECT_EQ(prober.GetState(), BandwidthProber::State::kProbing);
  auto p4 = prober.GetPendingProbes();
  ASSERT_EQ(p4.size(), 1u);
  EXPECT_EQ(p4[0].target_bitrate_kbps, 17496);
}

TEST_F(BandwidthProberTest, ChainStopsWhenLinkSaturates) {
  // When a probe result falls below the further-probe threshold (link can't
  // sustain the elevated rate), the chain must stop rather than keep probing.
  prober.SetMaxBitrate(30000);
  prober.SetEstimatedBitrate(1000);

  prober.GetEffectiveBitrateKbps();  // seed #1 at 3000
  prober.GetPendingProbes();
  // Weak result: 1800/3000 = 0.6 < 0.7 → no further chain probe.
  prober.OnProbeResult(1800, true);
  EXPECT_EQ(prober.GetState(), BandwidthProber::State::kIdle);
}

TEST_F(BandwidthProberTest, ChainStopsNearMax) {
  // The chain must not probe once the measured rate is within 5% of max.
  prober.SetMaxBitrate(10000);
  prober.SetEstimatedBitrate(1000);

  prober.GetEffectiveBitrateKbps();  // seed at 3000
  prober.GetPendingProbes();
  // 9600 >= 10000*0.95 = 9500 → at ceiling, no further probe.
  prober.OnProbeResult(9600, true);
  EXPECT_EQ(prober.GetState(), BandwidthProber::State::kIdle);
}

TEST_F(BandwidthProberTest, OveruseCancelsProbe) {
  prober.GetEffectiveBitrateKbps();
  EXPECT_EQ(prober.GetState(), BandwidthProber::State::kProbing);

  prober.OnOveruseDetected();
  EXPECT_EQ(prober.GetState(), BandwidthProber::State::kIdle);
}

TEST_F(BandwidthProberTest, ProbeTargetCappedByMax) {
  prober.SetMaxBitrate(10000);
  prober.SetEstimatedBitrate(5000);

  int rate = prober.GetEffectiveBitrateKbps();
  EXPECT_EQ(rate, 10000);  // 3x=15000, capped to 10000
}

TEST_F(BandwidthProberTest, NoProbeWhenNearMax) {
  prober.SetMaxBitrate(5200);
  prober.SetEstimatedBitrate(5000);  // 96% of max

  int rate = prober.GetEffectiveBitrateKbps();
  EXPECT_EQ(rate, 5000);  // no probe
  EXPECT_EQ(prober.GetState(), BandwidthProber::State::kIdle);
}

TEST_F(BandwidthProberTest, AlrProbingAfterInterval) {
  // Complete initial probing
  prober.OnOveruseDetected();
  AdvanceMs(1100);

  // Enter ALR
  prober.OnApplicationLimited();

  // Not enough time yet
  int rate = prober.GetEffectiveBitrateKbps();
  EXPECT_EQ(rate, 5000);

  // Advance past ALR interval
  AdvanceMs(5100);
  rate = prober.GetEffectiveBitrateKbps();
  EXPECT_EQ(rate, 7500);  // 1.5x
  EXPECT_EQ(prober.GetState(), BandwidthProber::State::kProbing);
}

TEST_F(BandwidthProberTest, DropRecoveryProbe) {
  prober.OnOveruseDetected();
  AdvanceMs(1100);

  // Simulate drop: was 5000, now 1500 (< 0.66*5000=3300)
  prober.SetEstimatedBitrate(1500);

  // The queue must be draining (underuse) before we dare probe back up —
  // otherwise we'd re-probe into a still-congested link.
  prober.OnUnderuseDetected();

  int rate = prober.GetEffectiveBitrateKbps();
  EXPECT_EQ(rate, 4250);  // 0.85 * 5000
  EXPECT_EQ(prober.GetState(), BandwidthProber::State::kProbing);
}

TEST_F(BandwidthProberTest, NoDropRecoveryProbeWhileStillCongested) {
  prober.OnOveruseDetected();
  AdvanceMs(1100);

  // Bitrate dropped sharply, but no underuse signal — the link is still
  // congested (queue not draining). Probing now would worsen congestion.
  prober.SetEstimatedBitrate(1500);

  int rate = prober.GetEffectiveBitrateKbps();
  EXPECT_EQ(rate, 1500);  // no probe — just track the estimate
  EXPECT_EQ(prober.GetState(), BandwidthProber::State::kIdle);
}

TEST_F(BandwidthProberTest, ProbeTimesOut) {
  prober.GetEffectiveBitrateKbps();  // starts probe
  auto probes = prober.GetPendingProbes();
  EXPECT_EQ(prober.GetState(), BandwidthProber::State::kWaitingForResult);

  // Advance past timeout (3s)
  AdvanceMs(3100);
  int rate = prober.GetEffectiveBitrateKbps();
  EXPECT_EQ(prober.GetState(), BandwidthProber::State::kIdle);
  EXPECT_EQ(rate, 5000);
}

TEST_F(BandwidthProberTest, FailedProbeStopsInitialProbing) {
  prober.GetEffectiveBitrateKbps();
  auto probes = prober.GetPendingProbes();
  prober.OnProbeResult(4000, false);

  EXPECT_EQ(prober.GetState(), BandwidthProber::State::kIdle);

  // Next call should NOT do exponential probe
  AdvanceMs(1100);
  int rate = prober.GetEffectiveBitrateKbps();
  EXPECT_EQ(rate, 5000);
  EXPECT_EQ(prober.GetState(), BandwidthProber::State::kIdle);
}

TEST_F(BandwidthProberTest, GetPendingProbesTransitionsToWaiting) {
  prober.GetEffectiveBitrateKbps();
  EXPECT_EQ(prober.GetState(), BandwidthProber::State::kProbing);

  auto probes = prober.GetPendingProbes();
  ASSERT_EQ(probes.size(), 1u);
  EXPECT_EQ(probes[0].target_bitrate_kbps, 15000);
  EXPECT_EQ(prober.GetState(), BandwidthProber::State::kWaitingForResult);

  // Second call returns empty
  auto probes2 = prober.GetPendingProbes();
  EXPECT_TRUE(probes2.empty());
}
