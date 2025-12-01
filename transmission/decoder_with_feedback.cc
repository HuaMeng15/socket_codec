#include "decoder_with_feedback.h"

#include "codec/decoder.h"
#include "log_system/log_system.h"

DecoderWithFeedback::DecoderWithFeedback(Decoder* decoder,
                                         MessageReceiver* message_receiver,
                                         int feedback_port)
    : decoder_(decoder),
      message_receiver_(message_receiver),
      feedback_port_(feedback_port),
      feedback_sender_initialized_(false) {}

DecoderWithFeedback::~DecoderWithFeedback() {
  if (feedback_sender_initialized_) {
    feedback_sender_.Close();
  }
}

int DecoderWithFeedback::HandlePacketMessage(const uint8_t* packet_data,
                                             size_t packet_size) {
  // Initialize feedback sender on first packet if not already initialized
  if (!feedback_sender_initialized_) {
    if (InitializeFeedbackSender()) {
      decoder_->SetFeedbackSender(&feedback_sender_);
      feedback_sender_initialized_ = true;
      LOG(INFO) << "[DecoderWithFeedback] Feedback sender initialized";
    } else {
      LOG(WARNING) << "[DecoderWithFeedback] Failed to initialize feedback sender, "
                      "continuing without feedback";
    }
  }

  // Forward packet to decoder
  if (decoder_) {
    return decoder_->HandlePacketMessage(packet_data, packet_size);
  }

  return -1;
}

bool DecoderWithFeedback::InitializeFeedbackSender() {
  if (!message_receiver_) {
    return false;
  }

  // Get sender information from message receiver
  std::string sender_ip;
  int sender_port;
  if (!message_receiver_->GetLastSenderInfo(sender_ip, sender_port)) {
    LOG(WARNING) << "[DecoderWithFeedback] No sender info available yet";
    return false;
  }

  // Initialize feedback sender to send back to the sender on feedback port
  if (feedback_sender_.Initialize(sender_ip, feedback_port_) != 0) {
    LOG(ERROR) << "[DecoderWithFeedback] Failed to initialize feedback sender to "
               << sender_ip << ":" << feedback_port_;
    return false;
  }

  LOG(INFO) << "[DecoderWithFeedback] Feedback sender initialized to send to "
            << sender_ip << ":" << feedback_port_;
  return true;
}

