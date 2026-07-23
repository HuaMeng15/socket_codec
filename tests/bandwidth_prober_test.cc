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

TEST_F(BandwidthProberTest, AlrProbeChainsUpToHopCap) {
  // An ALR (periodic) probe MAY chain (unlike a seed), but only up to
  // kMaxChainHops (2) further hops, and each hop explores at most 1.5x the
  // measured rate (WebRTC AimdRateControl increase limit). Bounding both the
  // hop count and the per-hop multiple stops one probe session from running to
  // ~2x the link capacity and flooding the pipe. Fires only while app-limited.
  prober.SetMaxBitrate(30000);
  prober.SetEstimatedBitrate(2000);
  prober.OnOveruseDetected();     // finish initial probing
  prober.OnApplicationLimited();  // encoder under-producing -> ALR

  // Trigger the ALR probe: 1.5x estimate = 3000.
  AdvanceMs(5100);
  int r = prober.GetEffectiveBitrateKbps();
  EXPECT_EQ(r, 3000);
  prober.GetPendingProbes();

  // Hop 1: strong result 2700/3000 = 0.9 → chain to 1.5x = 4050.
  prober.OnProbeResult(2700, true);
  EXPECT_EQ(prober.GetState(), BandwidthProber::State::kProbing);
  auto p2 = prober.GetPendingProbes();
  ASSERT_EQ(p2.size(), 1u);
  EXPECT_EQ(p2[0].target_bitrate_kbps, 4050);

  // Hop 2: strong result 3645/4050 = 0.9 → chain to 1.5x = 5467 (last hop).
  prober.OnProbeResult(3645, true);
  EXPECT_EQ(prober.GetState(), BandwidthProber::State::kProbing);
  auto p3 = prober.GetPendingProbes();
  ASSERT_EQ(p3.size(), 1u);
  EXPECT_EQ(p3[0].target_bitrate_kbps, 5467);

  // Hop cap reached: even a strong result 4920/5467 = 0.9 must NOT chain.
  prober.OnProbeResult(4920, true);
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

TEST_F(BandwidthProberTest, AlrProbingOnlyWhileApplicationLimited) {
  // After initial probing, a periodic probe fires ONLY while the sender is
  // application-limited (WebRTC-faithful). Without the ALR flag, no probe fires
  // no matter how much time passes — a greedy encoder that fills the pipe gets
  // pure AIMD, never a periodic probe.
  prober.OnOveruseDetected();  // completes initial probing

  // Not application-limited: even well past the interval, no probe.
  AdvanceMs(6100);
  int rate = prober.GetEffectiveBitrateKbps();
  EXPECT_EQ(rate, 5000);
  EXPECT_EQ(prober.GetState(), BandwidthProber::State::kIdle);

  // Now the AlrDetector reports application-limited (encoder under-producing).
  prober.OnApplicationLimited();

  // Interval elapsed since the last probe → ALR probe at 1.5x = 7500.
  AdvanceMs(5100);
  rate = prober.GetEffectiveBitrateKbps();
  EXPECT_EQ(rate, 7500);  // 1.5x estimate
  EXPECT_EQ(prober.GetState(), BandwidthProber::State::kProbing);
}

TEST_F(BandwidthProberTest, BitrateDropDoesNotProbe) {
  // WebRTC-faithful: a bitrate drop is recovered by AIMD, never a probe. The
  // only drop-recovery probe in WebRTC (ProbeController::RequestProbe) is ALR-
  // gated, and our ALR probe already covers the app-limited case. So a plain
  // estimate drop must NOT arm any probe — that self-induced re-probe loop is
  // exactly what we removed.
  prober.OnOveruseDetected();
  AdvanceMs(1100);

  // Sharp drop (5000 -> 1500). No ALR flag set.
  prober.SetEstimatedBitrate(1500);
  prober.OnUnderuseDetected();  // queue draining — must still not probe

  int rate = prober.GetEffectiveBitrateKbps();
  EXPECT_EQ(rate, 1500);  // just track the estimate; AIMD handles recovery
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
