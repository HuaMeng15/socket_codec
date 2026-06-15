#include <gtest/gtest.h>
#include <chrono>
#include <thread>

#include "transmission/bandwidth_prober.h"

class BandwidthProberTest : public ::testing::Test {
 protected:
  BandwidthProber prober;

  void SetUp() override {
    prober.SetCurrentBitrate(5000);
    prober.SetStableTimeBeforeProbeMs(100);  // fast for tests
    prober.SetProbeDurationMs(100);
    prober.SetEvalDurationMs(100);
  }
};

TEST_F(BandwidthProberTest, StartsIdle) {
  EXPECT_EQ(prober.GetState(), BandwidthProber::State::kIdle);
}

TEST_F(BandwidthProberTest, DoesNotProbeAfterRecentOveruse) {
  prober.OnOveruseDetected();

  // Immediately after overuse — should stay idle
  int rate = prober.GetEffectiveBitrateKbps();
  EXPECT_EQ(rate, 5000);
  EXPECT_EQ(prober.GetState(), BandwidthProber::State::kIdle);
}

TEST_F(BandwidthProberTest, StartsProbeAfterStablePeriod) {
  // Wait for stable_time_before_probe_ms (100ms in test)
  std::this_thread::sleep_for(std::chrono::milliseconds(110));

  int rate = prober.GetEffectiveBitrateKbps();
  EXPECT_EQ(prober.GetState(), BandwidthProber::State::kProbing);
  EXPECT_EQ(rate, 7500);  // 5000 * 1.5
}

TEST_F(BandwidthProberTest, ProbeCommitsOnSuccess) {
  // Wait for probe to start
  std::this_thread::sleep_for(std::chrono::milliseconds(110));
  prober.GetEffectiveBitrateKbps();  // triggers start
  EXPECT_EQ(prober.GetState(), BandwidthProber::State::kProbing);

  // Wait for probe duration to elapse
  std::this_thread::sleep_for(std::chrono::milliseconds(110));
  prober.GetEffectiveBitrateKbps();  // transitions to evaluating
  EXPECT_EQ(prober.GetState(), BandwidthProber::State::kEvaluating);

  // Send stable signals
  prober.OnStableSignal();
  prober.OnStableSignal();

  // Wait for eval duration
  std::this_thread::sleep_for(std::chrono::milliseconds(110));
  int rate = prober.GetEffectiveBitrateKbps();  // triggers evaluation decision

  // Should have committed or transitioned
  // On next call it settles to committed rate
  rate = prober.GetEffectiveBitrateKbps();
  EXPECT_EQ(rate, 7500);
  EXPECT_EQ(prober.GetState(), BandwidthProber::State::kIdle);
}

TEST_F(BandwidthProberTest, ProbeAbortsOnOveruse) {
  // Start probe
  std::this_thread::sleep_for(std::chrono::milliseconds(110));
  prober.GetEffectiveBitrateKbps();
  EXPECT_EQ(prober.GetState(), BandwidthProber::State::kProbing);

  // Overuse detected during probe
  prober.OnOveruseDetected();
  prober.OnOveruseDetected();

  // Should abort
  EXPECT_EQ(prober.GetState(), BandwidthProber::State::kAborted);

  // Rate reverts on next call
  int rate = prober.GetEffectiveBitrateKbps();
  EXPECT_EQ(rate, 5000);  // back to pre-probe
  EXPECT_EQ(prober.GetState(), BandwidthProber::State::kIdle);
}

TEST_F(BandwidthProberTest, ProbeRateUsesMultiplier) {
  prober.SetProbeMultiplier(2.0);
  prober.SetCurrentBitrate(3000);

  std::this_thread::sleep_for(std::chrono::milliseconds(110));
  int rate = prober.GetEffectiveBitrateKbps();
  EXPECT_EQ(rate, 6000);  // 3000 * 2.0
}
