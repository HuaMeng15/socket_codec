#ifndef TRANSMISSION_FEEDBACK_HANDLER_H
#define TRANSMISSION_FEEDBACK_HANDLER_H

#include <cstdint>
#include <string>

#include "transmission/message_handler.h"
#include "transmission/packet_header.h"

class FeedbackHandler : public MessageHandler {
 public:
  FeedbackHandler();
  ~FeedbackHandler();

  int Initialize();

  int HandlePacketMessage(const uint8_t* packet_data,
                          size_t packet_size) override;

  // Get statistics (optional, for monitoring)
  uint32_t GetFeedbackCount() const { return feedback_count_; }

 private:
  // Handle a feedback message (internal method)
  // feedback_data: raw feedback packet data
  // feedback_size: size of the feedback data in bytes
  int HandleFeedback(const uint8_t* feedback_data, size_t feedback_size);

 private:
  bool initialized_;
  uint32_t feedback_count_;
};

#endif  // TRANSMISSION_FEEDBACK_HANDLER_H

