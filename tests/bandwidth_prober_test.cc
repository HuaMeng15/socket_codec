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

// --- Startup stage (post-seed accelerator) ------------------------------------

class StartupStageTest : public BandwidthProberTest {
 protected:
  // Drive both seed probes to completion so initial_probing_done_ is set and
  // the prober is in the startup stage with the estimate committed at `est`.
  void FinishSeeds(int est) {
    prober.GetEffectiveBitrateKbps();  // seed #1
    prober.OnProbeResult(est, true);
    AdvanceMs(1100);
    prober.GetEffectiveBitrateKbps();  // seed #2 -> initial_probing_done_
    prober.OnProbeResult(est, true);
    prober.SetEstimatedBitrate(est);   // controller commits the measured rate
  }
};

TEST_F(StartupStageTest, FiresStartupProbeAfterSettle) {
  // Low estimate so seeds (3x/6x) stay well under the 30000 cap.
  prober.SetEstimatedBitrate(1000);
  FinishSeeds(2000);

  // Before the settle interval elapses, no startup probe fires.
  int rate = prober.GetEffectiveBitrateKbps();
  EXPECT_EQ(prober.GetState(), BandwidthProber::State::kIdle);
  EXPECT_EQ(rate, 2000);

  // After the settle interval, a startup probe fires at 1.5x the estimate.
  AdvanceMs(1000);
  rate = prober.GetEffectiveBitrateKbps();
  EXPECT_EQ(prober.GetState(), BandwidthProber::State::kProbing);
  EXPECT_EQ(rate, 3000);  // 1.5 * 2000
}

TEST_F(StartupStageTest, StrongStartupProbeKeepsProbing) {
  prober.SetEstimatedBitrate(1000);
  FinishSeeds(2000);
  AdvanceMs(1000);
  prober.GetEffectiveBitrateKbps();      // startup probe at 3000
  ASSERT_EQ(prober.GetState(), BandwidthProber::State::kProbing);

  // Strong result (2800/3000 = 0.93 >= 0.7): stage continues, no chain fires
  // immediately (cadence is the settle timer).
  prober.OnProbeResult(2800, true);
  EXPECT_EQ(prober.GetState(), BandwidthProber::State::kIdle);
  prober.SetEstimatedBitrate(2800);

  // Next settle interval -> another startup probe at 1.5 * 2800 = 4200.
  AdvanceMs(1000);
  int rate = prober.GetEffectiveBitrateKbps();
  EXPECT_EQ(prober.GetState(), BandwidthProber::State::kProbing);
  EXPECT_EQ(rate, 4200);
}

TEST_F(StartupStageTest, WeakStartupProbeEndsStage) {
  prober.SetEstimatedBitrate(1000);
  FinishSeeds(2000);
  AdvanceMs(1000);
  prober.GetEffectiveBitrateKbps();      // startup probe at 3000
  ASSERT_EQ(prober.GetState(), BandwidthProber::State::kProbing);

  // Weak result (1900/3000 = 0.63 < 0.7): link can't sustain it -> end stage.
  prober.OnProbeResult(1900, true);
  EXPECT_EQ(prober.GetState(), BandwidthProber::State::kIdle);
  prober.SetEstimatedBitrate(1900);

  // No further startup probe fires even after the settle interval.
  AdvanceMs(2000);
  int rate = prober.GetEffectiveBitrateKbps();
  EXPECT_EQ(prober.GetState(), BandwidthProber::State::kIdle);
  EXPECT_EQ(rate, 1900);
}

TEST_F(StartupStageTest, OveruseEndsStage) {
  prober.SetEstimatedBitrate(1000);
  FinishSeeds(2000);

  // Latency rising during startup ends the stage immediately.
  prober.OnOveruseDetected();

  AdvanceMs(2000);  // well past a settle interval
  int rate = prober.GetEffectiveBitrateKbps();
  EXPECT_EQ(prober.GetState(), BandwidthProber::State::kIdle);
  EXPECT_EQ(rate, 2000);  // no startup probe — AIMD owns it now
}

TEST_F(BandwidthProberTest, PeriodicProbeIsQualifiedAndDoesNotChain) {
  // Periodic discovery takes one conservative, measurement-based step per ALR
  // interval. It must not compound several increases in one probe session.
  prober.SetMaxBitrate(30000);
  prober.SetEstimatedBitrate(2000);
  prober.OnOveruseDetected();     // finish initial probing
  prober.SetApplicationLimited(true);  // encoder under-producing -> ALR

  // Trigger the ALR probe after both the interval and ALR qualification time:
  // 1.25x estimate = 2500.
  AdvanceMs(5100);
  int r = prober.GetEffectiveBitrateKbps();
  EXPECT_EQ(r, 2500);
  prober.GetPendingProbes();

  prober.OnProbeResult(2400, true);
  EXPECT_EQ(prober.GetState(), BandwidthProber::State::kIdle);
  EXPECT_TRUE(prober.GetPendingProbes().empty());
}

TEST_F(BandwidthProberTest, TransientAlrDoesNotArmDelayedProbe) {
  prober.OnOveruseDetected();
  AdvanceMs(5100);  // periodic interval is ready

  prober.SetApplicationLimited(true);
  AdvanceMs(250);  // shorter than qualification time
  EXPECT_EQ(prober.GetEffectiveBitrateKbps(), 5000);
  EXPECT_EQ(prober.GetState(), BandwidthProber::State::kIdle);

  prober.SetApplicationLimited(false);
  AdvanceMs(1000);
  EXPECT_EQ(prober.GetEffectiveBitrateKbps(), 5000);
  EXPECT_EQ(prober.GetState(), BandwidthProber::State::kIdle);
}

TEST_F(BandwidthProberTest, PeriodicProbeRequiresHealthyNetworkState) {
  prober.OnOveruseDetected();
  prober.SetApplicationLimited(true);
  prober.SetPeriodicProbingAllowed(false);
  AdvanceMs(5100);

  EXPECT_EQ(prober.GetEffectiveBitrateKbps(), 5000);
  EXPECT_EQ(prober.GetState(), BandwidthProber::State::kIdle);

  // Once the measured cwnd/queue state recovers, the already-qualified ALR
  // may take exactly one conservative measurement-driven step.
  prober.SetPeriodicProbingAllowed(true);
  EXPECT_EQ(prober.GetEffectiveBitrateKbps(), 6250);
  EXPECT_EQ(prober.GetState(), BandwidthProber::State::kProbing);
}

TEST_F(BandwidthProberTest, UnhealthyStateCancelsOnlyPeriodicProbe) {
  prober.OnOveruseDetected();
  prober.SetApplicationLimited(true);
  AdvanceMs(5100);
  prober.GetEffectiveBitrateKbps();
  ASSERT_EQ(prober.GetState(), BandwidthProber::State::kProbing);

  prober.SetPeriodicProbingAllowed(false);
  EXPECT_EQ(prober.GetState(), BandwidthProber::State::kIdle);
  EXPECT_TRUE(prober.GetPendingProbes().empty());
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
  prober.SetApplicationLimited(true);

  // Interval and qualification elapsed → conservative 1.25x probe.
  AdvanceMs(5100);
  rate = prober.GetEffectiveBitrateKbps();
  EXPECT_EQ(rate, 6250);  // 1.25x estimate
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
