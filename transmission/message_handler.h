#ifndef TRANSMISSION_MESSAGE_HANDLER_H
#define TRANSMISSION_MESSAGE_HANDLER_H

#include <cstddef>
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

  // Timestamp-aware receive path. DataReceiver supplies the kernel socket
  // arrival timestamp when the platform supports it. Existing handlers keep
  // working through the default implementation.
  virtual int HandlePacketMessageWithTimestamp(const uint8_t* packet_data,
                                               size_t packet_size,
                                               int64_t arrival_time_us) {
    (void)arrival_time_us;
    return HandlePacketMessage(packet_data, packet_size);
  }
};

#endif  // TRANSMISSION_MESSAGE_HANDLER_H
