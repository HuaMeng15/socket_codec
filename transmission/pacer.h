#ifndef TRANSMISSION_PACER_H
#define TRANSMISSION_PACER_H

#include <chrono>
#include <cstddef>
#include <mutex>

/**
 * Pacer: rate-limits packet sends so they are spread over time instead of
 * bursting. Call Pace(packet_size) before each send; it may block until the
 * packet is allowed under the current target bitrate.
 * Thread-safe: Pace() from sender thread, SetTargetBitrate() from feedback thread.
 */
class Pacer {
 public:
  Pacer();
  ~Pacer() = default;

  /** Set target bitrate in kbps; pacing uses this to space packets. */
  void SetTargetBitrate(int bitrate_kbps);

  /**
   * Block until the next packet of size packet_size_bytes may be sent,
   * then update internal state. Call before each SendPacket.
   * If bitrate is 0 or very high, returns immediately without blocking.
   */
  void Pace(size_t packet_size_bytes);

 private:
  std::mutex mutex_;
  int bitrate_kbps_;
  std::chrono::steady_clock::time_point next_send_time_;
  bool next_send_time_initialized_ = false;
};

#endif  // TRANSMISSION_PACER_H
