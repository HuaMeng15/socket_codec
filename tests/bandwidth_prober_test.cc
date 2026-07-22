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

TEST_F(BandwidthProberTest, SeedProbeDoesNotChain) {
  // Seed (startup) probes commit their measured rate and hand off to AIMD.
  // They must NOT fire a further probe even on a strong result — matching the
  // WebRTC reference, where the initial 3x/6x cluster jumps once and then lets
  // AIMD climb, rather than chaining straight to the ceiling. (Chaining is
  // reserved for periodic / drop-recovery probes; see PeriodicProbeChains*.)
  prober.GetEffectiveBitrateKbps();  // seed #1 at 3x = 15000
  auto probes = prober.GetPendingProbes();
  ASSERT_EQ(probes.size(), 1u);
  EXPECT_EQ(probes[0].target_bitrate_kbps, 15000);

  // Strong result 12000/15000 = 0.8 >= 0.7, but a seed still does not chain.
  prober.OnProbeResult(12000, true);
  EXPECT_EQ(prober.GetState(), BandwidthProber::State::kIdle);
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

TEST_F(BandwidthProberTest, PeriodicProbeChainsUpToHopCap) {
  // A periodic probe MAY chain (unlike a seed), but only up to kMaxChainHops
  // (2) further hops. Bounding the hop count is what stops one probe session
  // from doubling all the way to 2x the link capacity and flooding the pipe.
  prober.SetMaxBitrate(30000);
  prober.SetEstimatedBitrate(2000);
  prober.OnOveruseDetected();  // finish initial probing

  // Trigger the periodic probe: 2x estimate = 4000.
  AdvanceMs(5100);
  int r = prober.GetEffectiveBitrateKbps();
  EXPECT_EQ(r, 4000);
  prober.GetPendingProbes();

  // Hop 1: strong result 3600/4000 = 0.9 → chain to 2x = 7200.
  prober.OnProbeResult(3600, true);
  EXPECT_EQ(prober.GetState(), BandwidthProber::State::kProbing);
  auto p2 = prober.GetPendingProbes();
  ASSERT_EQ(p2.size(), 1u);
  EXPECT_EQ(p2[0].target_bitrate_kbps, 7200);

  // Hop 2: strong result 6480/7200 = 0.9 → chain to 2x = 12960 (last hop).
  prober.OnProbeResult(6480, true);
  EXPECT_EQ(prober.GetState(), BandwidthProber::State::kProbing);
  auto p3 = prober.GetPendingProbes();
  ASSERT_EQ(p3.size(), 1u);
  EXPECT_EQ(p3[0].target_bitrate_kbps, 12960);

  // Hop cap reached: even a strong result 11664/12960 = 0.9 must NOT chain.
  prober.OnProbeResult(11664, true);
  EXPECT_EQ(prober.GetState(), BandwidthProber::State::kIdle);
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

TEST_F(BandwidthProberTest, PeriodicProbingAfterInterval) {
  // After initial probing completes, the prober re-probes every
  // kPeriodicProbeIntervalMs at 2x the current estimate — independent of the
  // ALR flag (a greedy encoder is never application-limited, so ALR alone
  // would never fire and convergence would stall on AIMD).
  prober.OnOveruseDetected();  // completes initial probing

  // Not enough time elapsed since the last probe yet.
  AdvanceMs(1100);
  int rate = prober.GetEffectiveBitrateKbps();
  EXPECT_EQ(rate, 5000);
  EXPECT_EQ(prober.GetState(), BandwidthProber::State::kIdle);

  // Past the periodic interval → probe at 2x = 10000.
  AdvanceMs(5100);
  rate = prober.GetEffectiveBitrateKbps();
  EXPECT_EQ(rate, 10000);  // 2x estimate
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
