#ifndef TRANSMISSION_FEEDBACK_MANAGE_H
#define TRANSMISSION_FEEDBACK_MANAGE_H

#include <cstdint>
#include <string>

#include "transmission/message_handler.h"

// Feedback packet structure
// Note: This is a variable-length structure due to timestamp string
// The format is: frame_sequence (4 bytes, network byte order) +
//                packet_index (2 bytes, network byte order) +
//                timestamp_string (null-terminated, format: yyyy-mm-dd-hh-mm-ss-mmm)
struct FeedbackPacket {
  uint32_t frame_sequence;  // Frame sequence number that was received (network byte order)
  uint16_t packet_index;    // Packet index that was received (network byte order)
  char timestamp[32];       // Timestamp string: yyyy-mm-dd-hh-mm-ss-mmm (null-terminated)
};

// FeedbackManage class for handling feedback messages from receiver
class FeedbackManage : public MessageHandler {
 public:
  FeedbackManage();
  ~FeedbackManage();

  // Initialize the feedback manager
  // Returns 0 on success, negative value on error
  int Initialize();

  // HandlePacketMessage implementation from MessageHandler
  // Handles feedback packets received from receiver
  int HandlePacketMessage(const uint8_t* packet_data,
                          size_t packet_size) override;

  // Get statistics (optional, for monitoring)
  uint32_t GetFeedbackCount() const { return feedback_count_; }

 private:
  // Handle a feedback message (internal method)
  // feedback_data: raw feedback packet data
  // feedback_size: size of the feedback data in bytes
  // Returns 0 on success, negative value on error
  int HandleFeedback(const uint8_t* feedback_data, size_t feedback_size);

 private:
  bool initialized_;
  uint32_t feedback_count_;
};

#endif  // TRANSMISSION_FEEDBACK_MANAGE_H

