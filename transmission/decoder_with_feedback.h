#ifndef TRANSMISSION_DECODER_WITH_FEEDBACK_H
#define TRANSMISSION_DECODER_WITH_FEEDBACK_H

#include "message_handler.h"
#include "message_receiver.h"
#include "message_sender.h"

class Decoder;

// Wrapper handler that forwards packets to decoder and manages feedback sender
class DecoderWithFeedback : public MessageHandler {
 public:
  DecoderWithFeedback(Decoder* decoder, MessageReceiver* message_receiver,
                      int feedback_port);
  ~DecoderWithFeedback();

  // HandlePacketMessage implementation
  int HandlePacketMessage(const uint8_t* packet_data,
                          size_t packet_size) override;

 private:
  // Initialize feedback sender with sender's address
  bool InitializeFeedbackSender();

  Decoder* decoder_;
  MessageReceiver* message_receiver_;
  MessageSender feedback_sender_;
  int feedback_port_;
  bool feedback_sender_initialized_;
};

#endif  // TRANSMISSION_DECODER_WITH_FEEDBACK_H

