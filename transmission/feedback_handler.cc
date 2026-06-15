#include "feedback_handler.h"

#include <arpa/inet.h>
#include <chrono>
#include <cstring>

#include "log_system/log_system.h"
#include "packet_header.h"
#include "packet_send_time_store.h"
#include "transport_feedback.h"

FeedbackHandler::FeedbackHandler()
    : initialized_(false), feedback_count_(0) {}

FeedbackHandler::~FeedbackHandler() {}

int FeedbackHandler::Initialize() {
  if (initialized_) {
    LOG(WARNING) << "[FeedbackHandler] Already initialized";
    return 0;
  }
  initialized_ = true;
  feedback_count_ = 0;
  LOG(INFO) << "[FeedbackHandler] Initialized";
  return 0;
}

int FeedbackHandler::HandlePacketMessage(const uint8_t* packet_data,
                                         size_t packet_size) {
  if (!initialized_ || !packet_data || packet_size < 1) {
    return -1;
  }

  // Distinguish message type by first byte.
  // New-style messages have FeedbackMessageType in byte 0.
  // Legacy ACKs have FeedbackPacketHeader which starts with frame_sequence (2 bytes).
  // We can distinguish because FeedbackMessageType values (1, 2) are small,
  // while frame_sequence high byte is typically non-zero for active streams.
  // But safer: check if size matches new-style header + records.
  if (packet_size >= sizeof(FeedbackMessageHeader)) {
    auto* header = reinterpret_cast<const FeedbackMessageHeader*>(packet_data);
    auto msg_type = static_cast<FeedbackMessageType>(header->message_type);

    if (msg_type == FeedbackMessageType::TransportFeedback) {
      return HandleTransportFeedback(packet_data, packet_size);
    } else if (msg_type == FeedbackMessageType::LossReport) {
      return HandleLossReport(packet_data, packet_size);
    }
  }

  // Fall through to legacy handling
  return HandleLegacyFeedback(packet_data, packet_size);
}

int FeedbackHandler::HandleLegacyFeedback(const uint8_t* data, size_t size) {
  const size_t header_size = sizeof(FeedbackPacketHeader);
  if (size < header_size) {
    return -1;
  }

  const auto* header = reinterpret_cast<const FeedbackPacketHeader*>(data);
  uint16_t frame_sequence = ntohs(header->frame_sequence);
  uint8_t packet_index = header->packet_index;
  feedback_count_++;

  LOG(VERBOSE) << "[FeedbackHandler] Legacy ACK: frame=" << frame_sequence
               << " packet=" << (int)packet_index;

  // Legacy behavior: compute one-way delay if stores are available, but don't
  // adjust bitrate here anymore — that's the congestion controller's job.
  // Just build a single-entry TransportFeedback and forward it.
  if (transport_feedback_cb_ && send_time_store_) {
    auto send_time = send_time_store_->GetSendTime(frame_sequence, packet_index);
    if (send_time) {
      TransportFeedback fb;
      auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count();
      fb.reference_time_us = now_us;
      fb.packets.push_back({frame_sequence, packet_index, now_us});
      transport_feedback_cb_(fb);
    }
  }

  return 0;
}

int FeedbackHandler::HandleTransportFeedback(const uint8_t* data, size_t size) {
  if (size < sizeof(FeedbackMessageHeader)) {
    return -1;
  }

  const auto* header = reinterpret_cast<const FeedbackMessageHeader*>(data);
  uint16_t record_count = ntohs(header->record_count);

  size_t expected_size = sizeof(FeedbackMessageHeader) +
                         record_count * sizeof(PacketArrivalRecord);
  if (size < expected_size) {
    LOG(WARNING) << "[FeedbackHandler] Transport feedback too small: " << size
                 << " expected " << expected_size;
    return -1;
  }

  const auto* records = reinterpret_cast<const PacketArrivalRecord*>(
      data + sizeof(FeedbackMessageHeader));

  // Build TransportFeedback struct
  TransportFeedback feedback;
  auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  feedback.reference_time_us = now_us;

  for (uint16_t i = 0; i < record_count; i++) {
    TransportFeedback::PacketInfo info;
    info.frame_sequence = ntohs(records[i].frame_sequence);
    info.packet_index = records[i].packet_index;
    // arrival_time_ms is relative offset within the batch
    int32_t offset_ms = static_cast<int32_t>(ntohl(
        static_cast<uint32_t>(records[i].arrival_time_ms)));
    info.arrival_time_us = now_us + static_cast<int64_t>(offset_ms) * 1000;
    feedback.packets.push_back(info);
  }

  feedback_count_ += record_count;

  LOG(VERBOSE) << "[FeedbackHandler] Transport feedback: " << record_count
               << " packets";

  if (transport_feedback_cb_) {
    transport_feedback_cb_(feedback);
  }

  return 0;
}

int FeedbackHandler::HandleLossReport(const uint8_t* data, size_t size) {
  if (size < sizeof(FeedbackMessageHeader)) {
    return -1;
  }

  const auto* header = reinterpret_cast<const FeedbackMessageHeader*>(data);
  uint16_t record_count = ntohs(header->record_count);

  size_t expected_size = sizeof(FeedbackMessageHeader) +
                         record_count * sizeof(PacketLossRecord);
  if (size < expected_size) {
    return -1;
  }

  const auto* records = reinterpret_cast<const PacketLossRecord*>(
      data + sizeof(FeedbackMessageHeader));

  LossReport report;
  for (uint16_t i = 0; i < record_count; i++) {
    report.packets.push_back({
        ntohs(records[i].frame_sequence),
        records[i].packet_index
    });
  }

  LOG(VERBOSE) << "[FeedbackHandler] Loss report: " << record_count << " packets";

  if (loss_report_cb_) {
    loss_report_cb_(report);
  }

  return 0;
}
