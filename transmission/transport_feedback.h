#ifndef TRANSMISSION_TRANSPORT_FEEDBACK_H
#define TRANSMISSION_TRANSPORT_FEEDBACK_H

#include <cstdint>
#include <vector>

/**
 * TWCC-style transport feedback: batch of per-packet receive timestamps.
 * The receiver collects arrival times for packets and periodically sends
 * a TransportFeedback message back to the sender.
 *
 * Wire format:
 *   [FeedbackMessageHeader]
 *   [PacketArrivalRecord] × record_count
 *
 * All multi-byte fields are network byte order (big-endian).
 */

// Feedback message types (extends FeedbackType enum)
enum class FeedbackMessageType : uint8_t {
  LegacyAck = 0,         // Old per-packet ACK (backward compat)
  TransportFeedback = 1,  // TWCC-style batch feedback
  LossReport = 2,         // Explicit loss report
};

// Header for all new-style feedback messages
struct FeedbackMessageHeader {
  uint8_t message_type;   // FeedbackMessageType
  uint8_t reserved;
  uint16_t record_count;  // Number of records following this header
  uint32_t sender_ssrc;   // Identifies the sender (for multi-stream, currently unused)
};

// Per-packet arrival record in TransportFeedback
struct PacketArrivalRecord {
  uint16_t frame_sequence;   // Frame this packet belongs to
  uint8_t packet_index;      // Packet index within frame
  uint8_t padding;
  int32_t arrival_time_ms;   // Arrival time relative to first packet in this batch (ms)
};

// Per-packet loss record in LossReport
struct PacketLossRecord {
  uint16_t frame_sequence;
  uint8_t packet_index;
  uint8_t padding;
};

/**
 * TransportFeedback: high-level struct used within the sender to process
 * received feedback. Not a wire format — assembled from parsed messages.
 */
struct TransportFeedback {
  struct PacketInfo {
    uint16_t frame_sequence;
    uint8_t packet_index;
    int64_t send_time_us;     // Sender clock (looked up from PacketSendTimeStore)
    int64_t arrival_time_us;  // Receiver clock (from feedback message)
  };

  std::vector<PacketInfo> packets;
  int64_t reference_time_us = 0;  // Receiver's clock base for this batch
};

/**
 * LossReport: set of packets the receiver detected as lost.
 */
struct LossReport {
  struct LostPacket {
    uint16_t frame_sequence;
    uint8_t packet_index;
  };

  std::vector<LostPacket> packets;
};

#endif  // TRANSMISSION_TRANSPORT_FEEDBACK_H
