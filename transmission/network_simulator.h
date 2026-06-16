#ifndef TRANSMISSION_NETWORK_SIMULATOR_H
#define TRANSMISSION_NETWORK_SIMULATOR_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>
#include <random>
#include <thread>
#include <vector>

/**
 * NetworkSimulator: models a single bottleneck link as an ASYNCHRONOUS queue.
 *
 * Unlike a synchronous throttle (which would block the sender and couple
 * send timestamps to the bottleneck rate), this enqueues each packet without
 * blocking the caller and delivers it later on a background thread. This is
 * what makes congestion control observable: the sender injects packets at its
 * own paced (target) rate, packets queue at the bottleneck, and their arrival
 * is progressively delayed when target > capacity — producing the growing
 * one-way delay that the delay-based estimator detects.
 *
 * Per-packet delivery model (single FIFO bottleneck):
 *   serialization = packet_size / bandwidth          (link transmit time)
 *   departure     = max(now, link_free_time) + serialization
 *   arrival       = departure + propagation_delay (+ jitter)
 * Packets whose queuing delay would exceed max_queue_ms are dropped (the
 * bottleneck buffer overflowed) — this is congestion loss.
 *
 * Random loss is applied independently at enqueue time.
 *
 * When no simulator is attached, NetworkSender sends directly (zero overhead).
 */
class NetworkSimulator {
 public:
  struct Config {
    int bandwidth_kbps = 0;        // 0 = unlimited (no queuing)
    int propagation_delay_ms = 0;  // constant one-way base delay
    double loss_rate = 0.0;        // [0.0, 1.0] random loss
    int jitter_ms = 0;             // random +/- jitter on delivery
    int max_queue_ms = 1000;       // drop when queuing delay exceeds this
  };

  // Callback that performs the actual delivery (e.g. socket send()).
  using DeliverFn = std::function<void(const uint8_t* data, size_t size)>;

  NetworkSimulator();
  ~NetworkSimulator();

  void SetConfig(const Config& config);
  Config GetConfig() const;
  void SetBandwidthKbps(int bandwidth_kbps);

  /** Set the delivery callback (invoked from the background thread). */
  void SetDeliverCallback(DeliverFn fn);

  /** Start the delivery thread. Idempotent. */
  void Start();
  /** Stop the delivery thread and drain. Idempotent. */
  void Stop();

  /**
   * Enqueue a packet for (delayed) delivery. Non-blocking.
   * Returns true if accepted, false if dropped (random loss or queue overflow).
   */
  bool Enqueue(const uint8_t* data, size_t size);

 private:
  using Clock = std::chrono::steady_clock;

  struct QueuedPacket {
    std::vector<uint8_t> data;
    Clock::time_point deliver_time;
  };

  void DeliveryLoop();

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::queue<QueuedPacket> queue_;
  Config config_;

  Clock::time_point link_free_time_;  // when the link can next transmit
  bool link_free_init_;

  std::thread delivery_thread_;
  std::atomic<bool> running_;
  DeliverFn deliver_;

  std::mt19937 rng_;
};

#endif  // TRANSMISSION_NETWORK_SIMULATOR_H
