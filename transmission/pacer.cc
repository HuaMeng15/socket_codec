#include "pacer.h"

#include <chrono>
#include <thread>

#include "config/config.h"
#include "log_system/log_system.h"

// Startup default; overridden by SetTargetBitrate(cc_initial) before the first
// packet. Aligned with the GCC/encoder startup rate.
static const int kDefaultBitrateKbps = kDefaultInitialBitrateKbps;

Pacer::Pacer()
    : bitrate_kbps_(kDefaultBitrateKbps) {}

void Pacer::SetTargetBitrate(int bitrate_kbps) {
  std::lock_guard<std::mutex> lock(mutex_);
  bitrate_kbps_ = bitrate_kbps > 0 ? bitrate_kbps : kDefaultBitrateKbps;
}

void Pacer::Pace(size_t packet_size_bytes) {
  if (packet_size_bytes == 0) return;

  std::unique_lock<std::mutex> lock(mutex_);
  int kbps = bitrate_kbps_;
  if (kbps <= 0) return;

  // Time this packet "consumes" at target bitrate: size_bits / bitrate_bps
  double bitrate_bps = static_cast<double>(kbps) * 1000.0;
  double packet_time_sec = (static_cast<double>(packet_size_bytes) * 8.0) / bitrate_bps;
  auto packet_duration = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
      std::chrono::duration<double>(packet_time_sec));

  auto now = std::chrono::steady_clock::now();
  if (!next_send_time_initialized_) {
    next_send_time_ = now;
    next_send_time_initialized_ = true;
    static bool logged = false;
    if (!logged) {
      logged = true;
      LOG(INFO) << "[Pacer] Pacing enabled at " << kbps << " kbps";
    }
  }
  auto allowed = next_send_time_;
  next_send_time_ += packet_duration;
  lock.unlock();

  if (allowed > now) {
    std::this_thread::sleep_until(allowed);
  }
}
