#include "network_simulator.h"

#include <algorithm>
#include <thread>

NetworkSimulator::NetworkSimulator()
    : tokens_(0.0),
      bucket_initialized_(false),
      rng_(std::random_device{}()) {
}

void NetworkSimulator::SetConfig(const Config& config) {
  std::lock_guard<std::mutex> lock(mutex_);
  config_ = config;
  bucket_initialized_ = false;  // reset token bucket on config change
}

NetworkSimulator::Config NetworkSimulator::GetConfig() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return config_;
}

void NetworkSimulator::SetBandwidthKbps(int bandwidth_kbps) {
  std::lock_guard<std::mutex> lock(mutex_);
  config_.bandwidth_kbps = bandwidth_kbps;
}

void NetworkSimulator::RefillTokens() {
  auto now = std::chrono::steady_clock::now();
  if (!bucket_initialized_) {
    last_refill_time_ = now;
    tokens_ = 0.0;
    bucket_initialized_ = true;
    return;
  }

  auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
      now - last_refill_time_).count();
  last_refill_time_ = now;

  if (config_.bandwidth_kbps > 0 && elapsed_us > 0) {
    // bytes per microsecond = (kbps * 1000) / (8 * 1000000) = kbps / 8000
    double bytes_per_us = config_.bandwidth_kbps / 8000.0;
    tokens_ += bytes_per_us * elapsed_us;
    // Cap bucket at 2x max packet size to avoid huge bursts after idle
    double max_tokens = 2.0 * 1500.0;
    tokens_ = std::min(tokens_, max_tokens);
  }
}

bool NetworkSimulator::ProcessPacket(size_t packet_size_bytes) {
  std::unique_lock<std::mutex> lock(mutex_);

  // Loss check
  if (config_.loss_rate > 0.0) {
    std::uniform_real_distribution<double> dist(0.0, 1.0);
    if (dist(rng_) < config_.loss_rate) {
      return false;  // packet dropped
    }
  }

  // Bandwidth limiting (token bucket)
  if (config_.bandwidth_kbps > 0) {
    RefillTokens();
    double needed = static_cast<double>(packet_size_bytes);
    while (tokens_ < needed) {
      // Calculate wait time for needed tokens
      double deficit = needed - tokens_;
      double bytes_per_us = config_.bandwidth_kbps / 8000.0;
      int64_t wait_us = static_cast<int64_t>(deficit / bytes_per_us) + 1;
      lock.unlock();
      std::this_thread::sleep_for(std::chrono::microseconds(wait_us));
      lock.lock();
      RefillTokens();
    }
    tokens_ -= needed;
  }

  // Delay + jitter
  int delay_ms = config_.propagation_delay_ms;
  if (config_.jitter_ms > 0) {
    std::uniform_int_distribution<int> jitter_dist(-config_.jitter_ms, config_.jitter_ms);
    delay_ms += jitter_dist(rng_);
    delay_ms = std::max(0, delay_ms);
  }

  if (delay_ms > 0) {
    lock.unlock();
    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
  }

  return true;  // packet passes
}
