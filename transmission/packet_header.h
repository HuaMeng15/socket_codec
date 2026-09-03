#ifndef TRANSMISSION_PACKET_HEADER_H
#define TRANSMISSION_PACKET_HEADER_H

#include <cstdint>

// Frame packet header structure (sent before frame data payload)
struct FramePacketHeader {
  uint16_t frame_sequence;  // Frame sequence number
  uint16_t packet_index;    // Packet index within frame (0-based)
  uint16_t total_packets;   // Total packets for frame; 0 means frame still open
  uint16_t payload_size;    // Size of payload in this packet
};
static_assert(sizeof(FramePacketHeader) == 8);

// Reserved frame_sequence value marking a padding packet (not real video).
// Padding is emitted by the pacer to fill the pipe during bandwidth probes so
// the probe's measured received rate reflects true link capacity even when the
// encoder hasn't yet produced probe-rate frames. The receiver recognizes this
// sentinel, reports the packet's arrival + wire size in TWCC feedback (so it
// counts toward the probe's received-rate measurement), and does NOT attempt
// frame assembly or decode. The sender never records a send-time for padding,
// so it is excluded from the delay-based trendline (which pairs send/arrival
// deltas) — padding only tops up the send rate, the real frames carry the
// delay signal. Real frame sequence numbers increment from 0 and never reach
// this value within an experiment run (30fps would need ~36 min).
static constexpr uint16_t kPaddingFrameSequence = 0xFFFF;

// Feedback packet header structure (sent before feedback payload)
struct FeedbackPacketHeader {
  uint16_t frame_sequence;  // Frame sequence number that was received (network byte order)
  uint16_t packet_index;   // Packet index that was received (network byte order)
  uint8_t feedback_type;   // Feedback type
};
static_assert(sizeof(FeedbackPacketHeader) == 6);

#endif  // TRANSMISSION_PACKET_HEADER_H
