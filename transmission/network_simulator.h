#ifndef TRANSMISSION_NETWORK_SIMULATOR_H
#define TRANSMISSION_NETWORK_SIMULATOR_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <random>

/**
 * NetworkSimulator: applies bandwidth cap, propagation delay, and random
 * packet loss to outgoing packets. Sits between DataSender and the actual
 * socket send() call.
 *
 * When disabled or not attached, packets pass through with zero overhead.
 *
 * Bandwidth model: token-bucket. Each send consumes tokens proportional to
 * packet size; if insufficient tokens, the caller blocks until enough
 * accumulate (simulating link capacity).
 *
 * Delay model: caller sleeps for propagation_delay before the packet is sent.
 * (This is a simplification — real delay would queue + release later, but for
 * an experimental project this is sufficient.)
 *
 * Loss model: uniform random drop with configurable probability.
 */
class NetworkSimulator {
 public:
  struct Config {
    int bandwidth_kbps = 0;        // 0 = unlimited
    int propagation_delay_ms = 0;  // one-way delay in ms
    double loss_rate = 0.0;        // [0.0, 1.0]
    int jitter_ms = 0;             // random +/- jitter on delay
  };

  NetworkSimulator();
  ~NetworkSimulator() = default;

  /** Configure simulator parameters. Can be changed at runtime. */
  void SetConfig(const Config& config);

  /** Get current config. */
  Config GetConfig() const;

  /**
   * Process a packet before sending. May block (bandwidth), may drop (loss).
   * Returns true if the packet should be sent, false if it was "lost."
   */
  bool ProcessPacket(size_t packet_size_bytes);

  /** Update bandwidth dynamically (e.g. for step-change tests). */
  void SetBandwidthKbps(int bandwidth_kbps);

 private:
  mutable std::mutex mutex_;
  Config config_;

  // Token bucket state for bandwidth limiting
  double tokens_;  // in bytes
  std::chrono::steady_clock::time_point last_refill_time_;
  bool bucket_initialized_;

  // Random state for loss + jitter
  std::mt19937 rng_;

  void RefillTokens();
};

#endif  // TRANSMISSION_NETWORK_SIMULATOR_H
