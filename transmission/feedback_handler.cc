#include "feedback_handler.h"

#include <arpa/inet.h>
#include <cstring>

#include "log_system/log_system.h"
#include "packet_header.h"

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

  // TODO: Add custom feedback handling logic here
  // For example: adjust encoding parameters, track packet loss, etc.

  return 0;
}

