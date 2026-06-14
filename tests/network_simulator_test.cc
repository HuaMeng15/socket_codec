#include <gtest/gtest.h>
#include <chrono>
#include <thread>

#include "transmission/network_simulator.h"

class NetworkSimulatorTest : public ::testing::Test {
 protected:
  NetworkSimulator simulator;
};

TEST_F(NetworkSimulatorTest, NoConfigPassesAll) {
  // Default config: no bandwidth limit, no delay, no loss
  for (int i = 0; i < 100; i++) {
    EXPECT_TRUE(simulator.ProcessPacket(1400));
  }
}

TEST_F(NetworkSimulatorTest, FullLossDropsAll) {
  NetworkSimulator::Config config;
  config.loss_rate = 1.0;
  simulator.SetConfig(config);

  int passed = 0;
  for (int i = 0; i < 100; i++) {
    if (simulator.ProcessPacket(1400)) passed++;
  }
  EXPECT_EQ(passed, 0);
}

TEST_F(NetworkSimulatorTest, PartialLossDropsSome) {
  NetworkSimulator::Config config;
  config.loss_rate = 0.5;
  simulator.SetConfig(config);

  int passed = 0;
  for (int i = 0; i < 1000; i++) {
    if (simulator.ProcessPacket(1400)) passed++;
  }
  // With 50% loss over 1000 packets, expect 400-600 to pass
  EXPECT_GT(passed, 350);
  EXPECT_LT(passed, 650);
}

TEST_F(NetworkSimulatorTest, BandwidthLimitSlowsDown) {
  NetworkSimulator::Config config;
  config.bandwidth_kbps = 1000;  // 1 Mbps = 125 KB/s
  simulator.SetConfig(config);

  // Send 10 packets of 1250 bytes = 12500 bytes
  // At 125 KB/s, should take ~100ms
  auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < 10; i++) {
    simulator.ProcessPacket(1250);
  }
  auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start).count();

  // Should take at least ~80ms (allowing tolerance for first packet being free)
  EXPECT_GE(elapsed_ms, 60);
  // But not more than 200ms
  EXPECT_LE(elapsed_ms, 200);
}

TEST_F(NetworkSimulatorTest, DelayAddsLatency) {
  NetworkSimulator::Config config;
  config.propagation_delay_ms = 50;
  simulator.SetConfig(config);

  auto start = std::chrono::steady_clock::now();
  simulator.ProcessPacket(100);
  auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start).count();

  EXPECT_GE(elapsed_ms, 40);
  EXPECT_LE(elapsed_ms, 80);
}

TEST_F(NetworkSimulatorTest, DynamicBandwidthChange) {
  NetworkSimulator::Config config;
  config.bandwidth_kbps = 10000;  // 10 Mbps — fast
  simulator.SetConfig(config);

  // First packet should be nearly instant
  auto start = std::chrono::steady_clock::now();
  simulator.ProcessPacket(1400);
  auto fast_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start).count();
  EXPECT_LE(fast_ms, 10);

  // Drop to 100 kbps
  simulator.SetBandwidthKbps(100);

  // 1400 bytes at 100kbps (12.5 KB/s) should take ~112ms
  start = std::chrono::steady_clock::now();
  simulator.ProcessPacket(1400);
  auto slow_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start).count();
  EXPECT_GE(slow_ms, 80);
}
