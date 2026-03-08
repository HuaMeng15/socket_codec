#include "feedback_handler.h"

#include <arpa/inet.h>
#include <chrono>
#include <cstring>

#include "log_system/log_system.h"
#include "packet_header.h"
#include "packet_send_time_store.h"

FeedbackHandler::FeedbackHandler()
    : initialized_(false), feedback_count_(0) {}

FeedbackHandler::~FeedbackHandler() {}

int FeedbackHandler::Initialize() {
  if (initialized_) {
    LOG(WARNING) << "[FeedbackHandler] Already initialized";
    return 0;
  }

  initialized_ = true;
  feedback_count_ = 0;

  LOG(INFO) << "[FeedbackHandler] Initialized";
  return 0;
}

int FeedbackHandler::HandlePacketMessage(const uint8_t* packet_data,
                                         size_t packet_size) {
  // Forward to HandleFeedback
  return HandleFeedback(packet_data, packet_size);
}

int FeedbackHandler::HandleFeedback(const uint8_t* feedback_data,
                                    size_t feedback_size) {
  if (!initialized_) {
    LOG(ERROR) << "[FeedbackHandler] Not initialized";
    return -1;
  }

  const size_t header_size = sizeof(FeedbackPacketHeader);
  if (!feedback_data || feedback_size < header_size) {
    LOG(WARNING) << "[FeedbackHandler] Invalid feedback data or size";
    return -1;
  }

  // Parse feedback packet header
  const FeedbackPacketHeader* header =
      reinterpret_cast<const FeedbackPacketHeader*>(feedback_data);
  uint16_t frame_sequence = ntohs(header->frame_sequence);
  uint8_t packet_index = header->packet_index;
  feedback_count_++;

  LOG(VERBOSE) << "[FeedbackHandler] Received feedback: frame=" << frame_sequence
            << " packet=" << (int)packet_index
            << " (total feedbacks=" << feedback_count_ << ")";

  // Reduce bitrate when current latency > (average of previous 10 packets) + threshold
  if (send_time_store_ && encoder_) {
    double recv_time = std::chrono::duration<double>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count();
    auto send_time = send_time_store_->GetSendTime(frame_sequence, packet_index);
    if (send_time) {
      double latency_ms = (recv_time - *send_time) * 1000.0;
      if (last_latencies_ms_.size() >= kLatencyWindowSize) {
        double sum = 0.0;
        for (double L : last_latencies_ms_) {
          sum += L;
        }
        double avg_ms = sum / static_cast<double>(kLatencyWindowSize);
        double threshold = avg_ms + kLatencyAboveAvgThresholdMs;
        if (latency_ms > threshold) {
          LOG(INFO) << "[FeedbackHandler] Packet latency " << latency_ms << " ms > avg "
                    << avg_ms << " + " << kLatencyAboveAvgThresholdMs << " ms, setting bitrate to "
                    << kLowBitrateKbps << " kbps";
          encoder_->SetTargetBitrate(kLowBitrateKbps);
        }
      }
      last_latencies_ms_.push_back(latency_ms);
      if (last_latencies_ms_.size() > kLatencyWindowSize) {
        last_latencies_ms_.pop_front();
      }
    }
  }

  return 0;
}

