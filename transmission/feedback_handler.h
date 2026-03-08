#ifndef TRANSMISSION_FEEDBACK_HANDLER_H
#define TRANSMISSION_FEEDBACK_HANDLER_H

#include <cstdint>
#include <string>
#include <deque>
#include "codec/encoder.h"
#include "transmission/message_handler.h"
#include "transmission/packet_header.h"

class PacketSendTimeStore;

class FeedbackHandler : public MessageHandler {
 public:
  FeedbackHandler();
  ~FeedbackHandler();

  int Initialize();

  int HandlePacketMessage(const uint8_t* packet_data,
                          size_t packet_size) override;

  // Get statistics (optional, for monitoring)
  uint32_t GetFeedbackCount() const { return feedback_count_; }

  /** Set store to look up packet send times (for latency). */
  void SetSendTimeStore(PacketSendTimeStore* store) { send_time_store_ = store; }
  /** Set encoder to adjust bitrate when latency slope is high. */
  void SetEncoder(Encoder* encoder) { encoder_ = encoder; }

 private:
  // Handle a feedback message (internal method)
  // feedback_data: raw feedback packet data
  // feedback_size: size of the feedback data in bytes
  int HandleFeedback(const uint8_t* feedback_data, size_t feedback_size);

 private:
  bool initialized_;
  uint32_t feedback_count_;
  PacketSendTimeStore* send_time_store_ = nullptr;
  Encoder* encoder_ = nullptr;
  std::deque<double> last_latencies_ms_;
  static constexpr size_t kLatencyWindowSize = 10;
  /** Trigger bitrate reduction when current latency > (avg of previous 10 packets) + this (ms). */
  static constexpr double kLatencyAboveAvgThresholdMs = 10.0;
  static constexpr int kLowBitrateKbps = 1000;
};

#endif  // TRANSMISSION_FEEDBACK_HANDLER_H

