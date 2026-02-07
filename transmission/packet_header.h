#ifndef TRANSMISSION_PACKET_HEADER_H
#define TRANSMISSION_PACKET_HEADER_H

#include <cstdint>

// Frame packet header structure (sent before frame data payload)
struct FramePacketHeader {
  uint16_t frame_sequence;  // Frame sequence number
  uint8_t packet_index;    // Packet index within frame (0-based)
  uint8_t total_packets;   // Total number of packets for this frame
  uint16_t payload_size;    // Size of payload in this packet
};

// Feedback packet header structure (sent before feedback payload)
struct FeedbackPacketHeader {
  uint16_t frame_sequence;  // Frame sequence number that was received (network byte order)
  uint8_t packet_index;    // Packet index that was received (network byte order)
  uint8_t feedback_type;   // Feedback type
};

#endif  // TRANSMISSION_PACKET_HEADER_H