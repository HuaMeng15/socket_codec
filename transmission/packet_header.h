#ifndef TRANSMISSION_PACKET_HEADER_H
#define TRANSMISSION_PACKET_HEADER_H

#include <cstdint>

// Packet header structure (sent before payload)
struct PacketHeader {
  uint32_t frame_sequence;  // Frame sequence number
  uint16_t packet_index;    // Packet index within frame (0-based)
  uint16_t total_packets;   // Total number of packets for this frame
  uint32_t payload_size;    // Size of payload in this packet
};

#endif  // TRANSMISSION_PACKET_HEADER_H