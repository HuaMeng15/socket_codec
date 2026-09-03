#include "feedback_collector.h"

#include <arpa/inet.h>
#include <cstring>

#include "log_system/log_system.h"
#include "transport_feedback.h"

FeedbackCollector::FeedbackCollector()
    : feedback_interval_(20),
      feedback_max_interval_ms_(10),
      epoch_us_(0),
      epoch_set_(false),
      last_feedback_time_(),
      pending_since_(),
      stopping_(false) {
  timer_thread_ = std::thread(&FeedbackCollector::TimerLoop, this);
}

FeedbackCollector::~FeedbackCollector() {
  Stop();
}

void FeedbackCollector::SetSendCallback(SendCallback cb) {
  std::lock_guard<std::mutex> lock(mutex_);
  send_cb_ = std::move(cb);
}

void FeedbackCollector::SetFeedbackInterval(int packet_count) {
  std::lock_guard<std::mutex> lock(mutex_);
  feedback_interval_ = packet_count > 0 ? packet_count : 20;
}

void FeedbackCollector::SetFeedbackMaxIntervalMs(int ms) {
  std::lock_guard<std::mutex> lock(mutex_);
  feedback_max_interval_ms_ = ms;
  timer_cv_.notify_all();
}

void FeedbackCollector::OnPacketReceived(uint16_t frame_sequence,
                                         uint16_t packet_index,
                                         uint16_t recv_size) {
  int64_t arrival_time_us =
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count();
  OnPacketReceived(frame_sequence, packet_index, recv_size, arrival_time_us);
}

void FeedbackCollector::OnPacketReceived(uint16_t frame_sequence,
                                         uint16_t packet_index,
                                         uint16_t recv_size,
                                         int64_t arrival_time_us) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto processing_now = std::chrono::steady_clock::now();
  if (!epoch_set_) {
    epoch_us_ = arrival_time_us;
    epoch_set_ = true;
  }

  // Add the current packet first
  if (pending_entries_.empty()) {
    pending_since_ = processing_now;
  }
  pending_entries_.push_back(
      {frame_sequence, packet_index, recv_size, arrival_time_us});

  // Send on whichever fires first: enough packets accumulated, OR enough time
  // has elapsed since the last feedback was sent. Checking time AFTER adding
  // means the arriving packet is included, so the first packet after an
  // inter-frame gap triggers feedback immediately (rather than waiting for the
  // next packet). This bounds the feedback interval as tightly as the packet
  // arrival rate allows.
  bool count_reached =
      static_cast<int>(pending_entries_.size()) >= feedback_interval_;
  bool time_reached = false;
  if (feedback_max_interval_ms_ > 0) {
    if (last_feedback_time_.time_since_epoch().count() > 0) {
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                         processing_now - last_feedback_time_)
                         .count();
      time_reached = elapsed >= feedback_max_interval_ms_;
    } else {
      // First feedback ever - measure from first packet
      auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
                     processing_now - pending_since_)
                     .count();
      time_reached = age >= feedback_max_interval_ms_;
    }
  }

  if (count_reached || time_reached) {
    SendTransportFeedback();
  } else {
    // Wake the deadline thread when a new batch starts or its configuration
    // changes. Unlike the old arrival-triggered check, the thread flushes at
    // the deadline even if no later packet arrives.
    timer_cv_.notify_all();
  }
}

void FeedbackCollector::TimerLoop() {
  std::unique_lock<std::mutex> lock(mutex_);
  while (!stopping_) {
    if (pending_entries_.empty() || feedback_max_interval_ms_ <= 0) {
      timer_cv_.wait(lock);
      continue;
    }

    const auto interval =
        std::chrono::milliseconds(feedback_max_interval_ms_);
    const auto deadline =
        last_feedback_time_.time_since_epoch().count() > 0
            ? last_feedback_time_ + interval
            : pending_since_ + interval;

    if (std::chrono::steady_clock::now() >= deadline) {
      SendTransportFeedback();
      continue;
    }

    timer_cv_.wait_until(lock, deadline);
  }
}

std::vector<LossReport::LostPacket> FeedbackCollector::DetectLoss(
    uint16_t frame_sequence, uint16_t total_packets,
    const std::vector<bool>& received_mask) {
  std::vector<LossReport::LostPacket> lost;
  for (size_t i = 0; i < total_packets; i++) {
    if (i < received_mask.size() && !received_mask[i]) {
      lost.push_back({frame_sequence, static_cast<uint16_t>(i)});
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

void FeedbackCollector::Stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) {
      return;
    }
    stopping_ = true;
    timer_cv_.notify_all();
  }
  if (timer_thread_.joinable()) {
    timer_thread_.join();
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
    records[i].packet_index = htons(entry.packet_index);

    int64_t delta_us = entry.arrival_time_us - epoch_us_;
    records[i].arrival_time_us = htonl(static_cast<int32_t>(delta_us));
    records[i].recv_size = htons(entry.recv_size);
    records[i].reserved2 = 0;
  }

  send_cb_(buffer.data(), buffer.size());

  LOG(VERBOSE) << "[FeedbackCollector] Sent transport feedback with "
               << record_count << " records";

  // Record the time of this feedback send for the next interval check
  last_feedback_time_ = std::chrono::steady_clock::now();

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
    records[i].packet_index = htons(lost_packets[i].packet_index);
  }

  send_cb_(buffer.data(), buffer.size());

  LOG(VERBOSE) << "[FeedbackCollector] Sent loss report with "
               << record_count << " lost packets";
}
