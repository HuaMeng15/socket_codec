#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "transmission/network_simulator.h"

// Helper: collect delivered packets with their delivery timestamps.
class Collector {
 public:
  void OnDeliver(const uint8_t*, size_t size) {
    std::lock_guard<std::mutex> lock(m_);
    times_.push_back(std::chrono::steady_clock::now());
    sizes_.push_back(size);
  }
  size_t count() {
    std::lock_guard<std::mutex> lock(m_);
    return times_.size();
  }
  std::vector<std::chrono::steady_clock::time_point> times() {
    std::lock_guard<std::mutex> lock(m_);
    return times_;
  }
 private:
  std::mutex m_;
  std::vector<std::chrono::steady_clock::time_point> times_;
  std::vector<size_t> sizes_;
};

class NetworkSimulatorTest : public ::testing::Test {
 protected:
  NetworkSimulator sim;
  Collector collector;

  void SetUp() override {
    sim.SetDeliverCallback(
        [this](const uint8_t* d, size_t s) { collector.OnDeliver(d, s); });
  }
  void TearDown() override { sim.Stop(); }

  void EnqueueN(int n, size_t size) {
    std::vector<uint8_t> buf(size, 0xAB);
    for (int i = 0; i < n; i++) sim.Enqueue(buf.data(), size);
  }
};

TEST_F(NetworkSimulatorTest, NoBandwidthDeliversAllQuickly) {
  // Default config: no bandwidth limit, no delay. All packets pass.
  sim.Start();
  EnqueueN(100, 1400);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_EQ(collector.count(), 100u);
}

TEST_F(NetworkSimulatorTest, FullLossDropsAll) {
  NetworkSimulator::Config cfg;
  cfg.loss_rate = 1.0;
  sim.SetConfig(cfg);
  sim.Start();
  EnqueueN(100, 1400);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_EQ(collector.count(), 0u);
}

TEST_F(NetworkSimulatorTest, PartialLossDropsSome) {
  NetworkSimulator::Config cfg;
  cfg.loss_rate = 0.5;
  sim.SetConfig(cfg);
  sim.Start();

  // Enqueue returns false on loss; count accepted directly.
  std::vector<uint8_t> buf(1400, 0xAB);
  int accepted = 0;
  for (int i = 0; i < 1000; i++) {
    if (sim.Enqueue(buf.data(), buf.size())) accepted++;
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  // ~50% accepted
  EXPECT_GT(accepted, 350);
  EXPECT_LT(accepted, 650);
}

TEST_F(NetworkSimulatorTest, PropagationDelayShiftsDelivery) {
  NetworkSimulator::Config cfg;
  cfg.propagation_delay_ms = 50;
  sim.SetConfig(cfg);
  sim.Start();

  auto t0 = std::chrono::steady_clock::now();
  std::vector<uint8_t> buf(100, 0xAB);
  sim.Enqueue(buf.data(), buf.size());

  // Wait for delivery
  for (int i = 0; i < 50 && collector.count() == 0; i++) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_EQ(collector.count(), 1u);
  auto delivered = collector.times()[0];
  auto delay_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      delivered - t0).count();
  EXPECT_GE(delay_ms, 45);
  EXPECT_LE(delay_ms, 90);
}

TEST_F(NetworkSimulatorTest, BandwidthQueuesPacketsWithoutBlockingSender) {
  // 1 Mbps link. Enqueue 10 packets of 1250 bytes = 12500 bytes = 100ms
  // of transmit time. Sender (Enqueue) must NOT block — it returns fast.
  NetworkSimulator::Config cfg;
  cfg.bandwidth_kbps = 1000;
  cfg.max_queue_ms = 5000;
  sim.SetConfig(cfg);
  sim.Start();

  auto t0 = std::chrono::steady_clock::now();
  EnqueueN(10, 1250);
  auto enqueue_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - t0).count();
  // Enqueue is non-blocking → returns almost immediately
  EXPECT_LT(enqueue_ms, 30);

  // But delivery is paced by bandwidth: ~100ms for all 10
  std::this_thread::sleep_for(std::chrono::milliseconds(250));
  EXPECT_EQ(collector.count(), 10u);

  // Last packet should arrive ~100ms after first (serialization)
  auto times = collector.times();
  ASSERT_EQ(times.size(), 10u);
  auto spread_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      times.back() - times.front()).count();
  EXPECT_GE(spread_ms, 70);
}

TEST_F(NetworkSimulatorTest, QueueOverflowDropsPackets) {
  // Tiny queue limit. Oversubscribe: enqueue many packets faster than the
  // link can drain → some dropped by overflow.
  NetworkSimulator::Config cfg;
  cfg.bandwidth_kbps = 1000;   // 125 KB/s
  cfg.max_queue_ms = 100;      // only 100ms of buffering
  sim.SetConfig(cfg);
  sim.Start();

  std::vector<uint8_t> buf(1250, 0xAB);
  int accepted = 0;
  for (int i = 0; i < 50; i++) {
    if (sim.Enqueue(buf.data(), buf.size())) accepted++;
  }
  // 50 packets * 1250 bytes = 62500 bytes = 500ms transmit, but only 100ms
  // buffer → many dropped.
  EXPECT_LT(accepted, 50);
  EXPECT_GT(accepted, 0);
}

TEST_F(NetworkSimulatorTest, DynamicBandwidthChange) {
  NetworkSimulator::Config cfg;
  cfg.bandwidth_kbps = 10000;  // fast
  cfg.max_queue_ms = 5000;
  sim.SetConfig(cfg);
  sim.Start();

  EnqueueN(5, 1250);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  size_t after_fast = collector.count();
  EXPECT_EQ(after_fast, 5u);

  // Drop to slow
  sim.SetBandwidthKbps(500);
  EnqueueN(5, 1250);
  // 5 * 1250 = 6250 bytes at 500kbps = 100ms; give it time
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT_EQ(collector.count(), 10u);
}
