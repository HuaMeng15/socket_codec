#ifndef TRANSMISSION_PACKET_SEND_TIME_STORE_H
#define TRANSMISSION_PACKET_SEND_TIME_STORE_H

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_map>

/**
 * Thread-safe store of (frame_sequence, packet_index) -> send timestamp.
 * Used by DataSender to record send times and FeedbackHandler to compute latency.
 */
class PacketSendTimeStore {
 public:
  void Record(uint16_t frame_sequence, uint8_t packet_index) {
    double t = NowSeconds();
    uint32_t key = Key(frame_sequence, packet_index);
    std::lock_guard<std::mutex> lock(mutex_);
    store_[key] = t;
    if (store_.size() > kMaxEntries) {
      EvictOld();
    }
  }

  /** Returns send time in seconds since epoch, or empty if not found. */
  std::optional<double> GetSendTime(uint16_t frame_sequence, uint8_t packet_index) const {
    uint32_t key = Key(frame_sequence, packet_index);
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = store_.find(key);
    if (it == store_.end()) return std::nullopt;
    return it->second;
  }

 private:
  static const size_t kMaxEntries = 50000;

  static uint32_t Key(uint16_t frame, uint8_t packet) {
    return (static_cast<uint32_t>(frame) << 8) | packet;
  }

  static double NowSeconds() {
    return std::chrono::duration<double>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }

  void EvictOld() {
    if (store_.empty()) return;
    uint32_t min_key = store_.begin()->first;
    for (const auto& p : store_) {
      if (p.first < min_key) min_key = p.first;
    }
    uint32_t cutoff = min_key + (1u << 16);
    for (auto it = store_.begin(); it != store_.end();) {
      if (it->first < cutoff)
        it = store_.erase(it);
      else
        ++it;
    }
  }

  mutable std::mutex mutex_;
  std::unordered_map<uint32_t, double> store_;
};

#endif  // TRANSMISSION_PACKET_SEND_TIME_STORE_H
