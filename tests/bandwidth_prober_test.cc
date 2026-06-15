#include <gtest/gtest.h>
#include <chrono>
#include <thread>

#include "transmission/bandwidth_prober.h"

class BandwidthProberTest : public ::testing::Test {
 protected:
  BandwidthProber prober;

  void SetUp() override {
    prober.SetEstimatedBitrate(5000);
    prober.SetMaxBitrate(30000);
  }
};

TEST_F(BandwidthProberTest, StartsIdle) {
  EXPECT_EQ(prober.GetState(), BandwidthProber::State::kIdle);
}

TEST_F(BandwidthProberTest, InitialExponentialProbeAt3x) {
  // First call should trigger initial exponential probe at 3x
  // Need to wait past kMinTimeBetweenProbesMs (1000ms) since construction
  // sets last_overuse_time_ to now. Let's trigger overuse reset.
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));

  int rate = prober.GetEffectiveBitrateKbps();
  // Should probe at 3x = 15000 kbps
  EXPECT_EQ(rate, 15000);
  EXPECT_EQ(prober.GetState(), BandwidthProber::State::kProbing);
}

TEST_F(BandwidthProberTest, DoesNotProbeAfterRecentOveruse) {
  prober.OnOveruseDetected();

  // Should not probe within kMinTimeBetweenProbesMs
  int rate = prober.GetEffectiveBitrateKbps();
  EXPECT_EQ(rate, 5000);
  EXPECT_EQ(prober.GetState(), BandwidthProber::State::kIdle);
}

TEST_F(BandwidthProberTest, SuccessfulProbeTriggersFewerProbes) {
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));

  // First probe at 3x
  int rate = prober.GetEffectiveBitrateKbps();
  EXPECT_EQ(rate, 15000);

  // Get probe clusters
  auto probes = prober.GetPendingProbes();
  ASSERT_EQ(probes.size(), 1u);
  EXPECT_EQ(probes[0].target_bitrate_kbps, 15000);

  // Report success — estimated 12000 from the probe
  // 12000/15000 = 0.8 > kFurtherProbeThreshold (0.7), so further probe
  prober.OnProbeResult(12000, true);

  // Should have initiated a further probe (but initial_probing_done_ is now true
  // since we used 2 exponential probes limit already reached after 1st...
  // Actually exponential_probe_count_ was 1 after first probe,
  // and kMaxExponentialProbes is 2, so initial_probing_done_ is false.
  // further probe should be initiated at 2x * 12000 = 24000 (capped by max)
  // The state should now be probing again
  EXPECT_EQ(prober.GetState(), BandwidthProber::State::kProbing);
}

TEST_F(BandwidthProberTest, OveruseCancelsProbe) {
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));

  prober.GetEffectiveBitrateKbps();  // starts probe
  EXPECT_EQ(prober.GetState(), BandwidthProber::State::kProbing);

  prober.OnOveruseDetected();
  EXPECT_EQ(prober.GetState(), BandwidthProber::State::kIdle);
}

TEST_F(BandwidthProberTest, ProbeTargetCappedByMaxBitrate) {
  prober.SetMaxBitrate(10000);
  prober.SetEstimatedBitrate(5000);

  std::this_thread::sleep_for(std::chrono::milliseconds(1100));

  int rate = prober.GetEffectiveBitrateKbps();
  // 3x would be 15000, but capped at 10000
  EXPECT_EQ(rate, 10000);
}

TEST_F(BandwidthProberTest, AlrProbingAfterInterval) {
  // Complete initial probing first
  prober.OnOveruseDetected();  // marks initial probing done

  // Signal ALR
  prober.OnApplicationLimited();

  // Wait for ALR interval + min between probes
  std::this_thread::sleep_for(std::chrono::milliseconds(5100));

  int rate = prober.GetEffectiveBitrateKbps();
  // ALR probe at 1.5x = 7500
  EXPECT_EQ(rate, 7500);
}

TEST_F(BandwidthProberTest, DropRecoveryProbe) {
  // Complete initial probing
  prober.OnOveruseDetected();
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));

  // Simulate a big drop: was 5000, drops to 1500 (< 0.66 * 5000 = 3300)
  prober.SetEstimatedBitrate(1500);

  // Should trigger drop recovery probe at 0.85 * 5000 = 4250
  int rate = prober.GetEffectiveBitrateKbps();
  EXPECT_EQ(rate, 4250);
  EXPECT_EQ(prober.GetState(), BandwidthProber::State::kProbing);
}

TEST_F(BandwidthProberTest, ProbeTimesOut) {
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));
  prober.GetEffectiveBitrateKbps();  // starts probe
  EXPECT_EQ(prober.GetState(), BandwidthProber::State::kProbing);

  // Get pending probes to transition to WaitingForResult
  auto probes = prober.GetPendingProbes();
  EXPECT_EQ(prober.GetState(), BandwidthProber::State::kWaitingForResult);

  // Wait for probe timeout (3s)
  std::this_thread::sleep_for(std::chrono::milliseconds(3100));
  int rate = prober.GetEffectiveBitrateKbps();

  // Should have timed out and returned to idle
  EXPECT_EQ(prober.GetState(), BandwidthProber::State::kIdle);
  EXPECT_EQ(rate, 5000);  // back to estimated
}

TEST_F(BandwidthProberTest, FailedProbeStopsInitialProbing) {
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));
  prober.GetEffectiveBitrateKbps();  // starts first probe at 3x
  auto probes = prober.GetPendingProbes();

  // Report failure
  prober.OnProbeResult(4000, false);
  EXPECT_EQ(prober.GetState(), BandwidthProber::State::kIdle);

  // Wait for min time and try again — should NOT do exponential probe
  // because initial_probing_done_ is set on failure
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));
  int rate = prober.GetEffectiveBitrateKbps();
  // Should not be 15000 (3x) or 30000 (6x) — initial probing is done
  EXPECT_EQ(rate, 5000);
}

TEST_F(BandwidthProberTest, NoProbeWhenNearMaxBitrate) {
  prober.SetMaxBitrate(5200);
  prober.SetEstimatedBitrate(5000);  // 5000/5200 = 96% > 95% threshold

  std::this_thread::sleep_for(std::chrono::milliseconds(1100));
  int rate = prober.GetEffectiveBitrateKbps();

  // Should not probe since we're already at 95%+ of max
  EXPECT_EQ(rate, 5000);
  EXPECT_EQ(prober.GetState(), BandwidthProber::State::kIdle);
}

TEST_F(BandwidthProberTest, GetPendingProbesTransitionsState) {
  std::this_thread::sleep_for(std::chrono::milliseconds(1100));
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
