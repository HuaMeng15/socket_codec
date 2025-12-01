#ifndef TRANSMISSION_MESSAGE_HANDLER_H
#define TRANSMISSION_MESSAGE_HANDLER_H

#include <cstdint>

// MessageHandler base class for handling received packets
// Derived classes can implement different handling strategies
// (e.g., decoding, logging, forwarding, etc.)
class MessageHandler {
 public:
  MessageHandler() = default;
  virtual ~MessageHandler() = default;

  // Handle a received packet
  // packet_data: raw packet data including header
  // packet_size: size of the packet data in bytes
  // Returns 0 on success, negative value on error
  virtual int HandlePacketMessage(const uint8_t* packet_data,
                                  size_t packet_size) = 0;
};

#endif  // TRANSMISSION_MESSAGE_HANDLER_H

