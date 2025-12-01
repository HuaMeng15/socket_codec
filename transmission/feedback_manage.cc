#include "feedback_manage.h"

#include <arpa/inet.h>
#include <cstring>

#include "log_system/log_system.h"

FeedbackManage::FeedbackManage()
    : initialized_(false), feedback_count_(0) {}

FeedbackManage::~FeedbackManage() {}

int FeedbackManage::Initialize() {
  if (initialized_) {
    LOG(WARNING) << "[FeedbackManage] Already initialized";
    return 0;
  }

  initialized_ = true;
  feedback_count_ = 0;

  LOG(INFO) << "[FeedbackManage] Initialized";
  return 0;
}

int FeedbackManage::HandlePacketMessage(const uint8_t* packet_data,
                                         size_t packet_size) {
  // Forward to HandleFeedback
  return HandleFeedback(packet_data, packet_size);
}

int FeedbackManage::HandleFeedback(const uint8_t* feedback_data,
                                    size_t feedback_size) {
  if (!initialized_) {
    LOG(ERROR) << "[FeedbackManage] Not initialized";
    return -1;
  }

  // Minimum size: frame_sequence (4) + packet_index (2) + at least 1 char for timestamp
  const size_t min_size = sizeof(uint32_t) + sizeof(uint16_t) + 1;
  if (!feedback_data || feedback_size < min_size) {
    LOG(WARNING) << "[FeedbackManage] Invalid feedback data or size";
    return -1;
  }

  // Parse feedback packet
  const FeedbackPacket* feedback =
      reinterpret_cast<const FeedbackPacket*>(feedback_data);
  uint32_t frame_sequence = ntohl(feedback->frame_sequence);
  uint16_t packet_index = ntohs(feedback->packet_index);
  
  // Extract timestamp string (ensure null termination)
  std::string timestamp_str;
  if (feedback_size >= min_size) {
    size_t timestamp_offset = sizeof(feedback->frame_sequence) + 
                              sizeof(feedback->packet_index);
    size_t max_timestamp_len = feedback_size - timestamp_offset;
    const char* timestamp_ptr = reinterpret_cast<const char*>(
        feedback_data + timestamp_offset);
    
    // Find null terminator or use max length
    size_t timestamp_len = 0;
    while (timestamp_len < max_timestamp_len && 
           timestamp_ptr[timestamp_len] != '\0') {
      timestamp_len++;
    }
    timestamp_str = std::string(timestamp_ptr, timestamp_len);
  }

  feedback_count_++;

  LOG(INFO) << "[FeedbackManage] Received feedback: frame=" << frame_sequence
            << " packet=" << packet_index << " timestamp=" << timestamp_str
            << " (total feedbacks=" << feedback_count_ << ")";

  // TODO: Add custom feedback handling logic here
  // For example: adjust encoding parameters, track packet loss, etc.

  return 0;
}

