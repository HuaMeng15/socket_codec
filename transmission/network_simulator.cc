#include "network_simulator.h"

#include <algorithm>

#include "log_system/log_system.h"

NetworkSimulator::NetworkSimulator()
    : link_free_init_(false),
      running_(false),
      rng_(std::random_device{}()) {}

NetworkSimulator::~NetworkSimulator() { Stop(); }

void NetworkSimulator::SetConfig(const Config& config) {
  std::lock_guard<std::mutex> lock(mutex_);
  config_ = config;
}

NetworkSimulator::Config NetworkSimulator::GetConfig() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return config_;
}

void NetworkSimulator::SetBandwidthKbps(int bandwidth_kbps) {
  std::lock_guard<std::mutex> lock(mutex_);
  config_.bandwidth_kbps = bandwidth_kbps;
}

void NetworkSimulator::SetDeliverCallback(DeliverFn fn) {
  std::lock_guard<std::mutex> lock(mutex_);
  deliver_ = std::move(fn);
}

void NetworkSimulator::Start() {
  if (running_.exchange(true)) return;
  delivery_thread_ = std::thread([this]() { DeliveryLoop(); });
}

void NetworkSimulator::Stop() {
  if (!running_.exchange(false)) return;
  cv_.notify_all();
  if (delivery_thread_.joinable()) {
    delivery_thread_.join();
  }
}

bool NetworkSimulator::Enqueue(const uint8_t* data, size_t size) {
  std::unique_lock<std::mutex> lock(mutex_);

  // Random loss at ingress
  if (config_.loss_rate > 0.0) {
    std::uniform_real_distribution<double> dist(0.0, 1.0);
    if (dist(rng_) < config_.loss_rate) {
      return false;
    }
  }

  auto now = Clock::now();

  // Serialization (transmit) time for this packet at current bandwidth.
  // bytes / (kbps * 1000 / 8) seconds = bytes * 8000 / kbps microseconds.
  int64_t serialize_us = 0;
  if (config_.bandwidth_kbps > 0) {
    serialize_us = static_cast<int64_t>(size) * 8000 / config_.bandwidth_kbps;
  }

  if (!link_free_init_) {
    link_free_time_ = now;
    link_free_init_ = true;
  }
  // The link is busy until link_free_time_; new packet waits behind it.
  auto base = std::max(now, link_free_time_);
  auto departure = base + std::chrono::microseconds(serialize_us);
  link_free_time_ = departure;

  // Queuing delay = how long this packet waited before its transmission ended,
  // beyond its own serialization. Drop if the buffer overflowed.
  int64_t queue_delay_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(departure - now)
          .count();
  if (config_.max_queue_ms > 0 && queue_delay_ms > config_.max_queue_ms) {
    // Bottleneck buffer overflow → congestion drop. Roll back link time so the
    // dropped packet doesn't consume capacity.
    link_free_time_ = base;
    return false;
  }

  // Propagation delay + optional jitter.
  int prop_ms = config_.propagation_delay_ms;
  if (config_.jitter_ms > 0) {
    std::uniform_int_distribution<int> jd(-config_.jitter_ms, config_.jitter_ms);
    prop_ms = std::max(0, prop_ms + jd(rng_));
  }
  auto arrival = departure + std::chrono::milliseconds(prop_ms);

  QueuedPacket qp;
  qp.data.assign(data, data + size);
  qp.deliver_time = arrival;
  queue_.push(std::move(qp));
  cv_.notify_one();
  return true;
}

void NetworkSimulator::DeliveryLoop() {
  std::unique_lock<std::mutex> lock(mutex_);
  while (running_.load()) {
    if (queue_.empty()) {
      cv_.wait_for(lock, std::chrono::milliseconds(50));
      continue;
    }
    auto& front = queue_.front();
    auto deliver_time = front.deliver_time;
    auto now = Clock::now();
    if (now < deliver_time) {
      cv_.wait_until(lock, deliver_time);
      continue;
    }
    // Time to deliver this packet.
    QueuedPacket qp = std::move(queue_.front());
    queue_.pop();
    DeliverFn fn = deliver_;
    lock.unlock();
    if (fn) {
      fn(qp.data.data(), qp.data.size());
    }
    lock.lock();
  }
}
