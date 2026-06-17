#include "feedback_collector.h"

#include <arpa/inet.h>
#include <cstring>

#include "log_system/log_system.h"
#include "transport_feedback.h"

FeedbackCollector::FeedbackCollector()
    : feedback_interval_(20),
      epoch_set_(false) {
}

void FeedbackCollector::SetSendCallback(SendCallback cb) {
  send_cb_ = std::move(cb);
}

void FeedbackCollector::SetFeedbackInterval(int packet_count) {
  feedback_interval_ = packet_count > 0 ? packet_count : 20;
}

void FeedbackCollector::OnPacketReceived(uint16_t frame_sequence,
                                         uint8_t packet_index) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto now = std::chrono::steady_clock::now();
  if (!epoch_set_) {
    epoch_ = now;
    epoch_set_ = true;
  }

  pending_entries_.push_back({frame_sequence, packet_index, now});

  if (static_cast<int>(pending_entries_.size()) >= feedback_interval_) {
    SendTransportFeedback();
  }
}

std::vector<LossReport::LostPacket> FeedbackCollector::DetectLoss(
    uint16_t frame_sequence, uint8_t total_packets,
    const std::vector<bool>& received_mask) {
  std::vector<LossReport::LostPacket> lost;
  for (uint8_t i = 0; i < total_packets; i++) {
    if (i < received_mask.size() && !received_mask[i]) {
      lost.push_back({frame_sequence, i});
    }
  }
  return lost;
}

void FeedbackCollector::Flush() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!pending_entries_.empty()) {
    SendTransportFeedback();
  }
}

void FeedbackCollector::SendTransportFeedback() {
  if (!send_cb_ || pending_entries_.empty()) {
    pending_entries_.clear();
    return;
  }

  size_t record_count = pending_entries_.size();
  size_t msg_size = sizeof(FeedbackMessageHeader) +
                    record_count * sizeof(PacketArrivalRecord);

  std::vector<uint8_t> buffer(msg_size);
  auto* header = reinterpret_cast<FeedbackMessageHeader*>(buffer.data());
  header->message_type = static_cast<uint8_t>(FeedbackMessageType::TransportFeedback);
  header->reserved = 0;
  header->record_count = htons(static_cast<uint16_t>(record_count));
  header->sender_ssrc = 0;

  auto* records = reinterpret_cast<PacketArrivalRecord*>(
      buffer.data() + sizeof(FeedbackMessageHeader));

  // Encode arrival offsets relative to the persistent epoch (first packet
  // ever received), NOT per-batch. This keeps inter-packet arrival deltas
  // consistent across batch boundaries so the sender's trendline estimator
  // sees a continuous arrival-time series.
  for (size_t i = 0; i < record_count; i++) {
    const auto& entry = pending_entries_[i];
    records[i].frame_sequence = htons(entry.frame_sequence);
    records[i].packet_index = entry.packet_index;
    records[i].padding = 0;

    auto delta = std::chrono::duration_cast<std::chrono::microseconds>(
        entry.arrival_time - epoch_);
    records[i].arrival_time_us = htonl(static_cast<int32_t>(delta.count()));
  }

  send_cb_(buffer.data(), buffer.size());

  LOG(VERBOSE) << "[FeedbackCollector] Sent transport feedback with "
               << record_count << " records";

  pending_entries_.clear();
}

void FeedbackCollector::SendLossReport(
    const std::vector<LossReport::LostPacket>& lost_packets) {
  if (!send_cb_ || lost_packets.empty()) {
    return;
  }

  size_t record_count = lost_packets.size();
  size_t msg_size = sizeof(FeedbackMessageHeader) +
                    record_count * sizeof(PacketLossRecord);

  std::vector<uint8_t> buffer(msg_size);
  auto* header = reinterpret_cast<FeedbackMessageHeader*>(buffer.data());
  header->message_type = static_cast<uint8_t>(FeedbackMessageType::LossReport);
  header->reserved = 0;
  header->record_count = htons(static_cast<uint16_t>(record_count));
  header->sender_ssrc = 0;

  auto* records = reinterpret_cast<PacketLossRecord*>(
      buffer.data() + sizeof(FeedbackMessageHeader));

  for (size_t i = 0; i < record_count; i++) {
    records[i].frame_sequence = htons(lost_packets[i].frame_sequence);
    records[i].packet_index = lost_packets[i].packet_index;
    records[i].padding = 0;
  }

  send_cb_(buffer.data(), buffer.size());

  LOG(VERBOSE) << "[FeedbackCollector] Sent loss report with "
               << record_count << " lost packets";
}
