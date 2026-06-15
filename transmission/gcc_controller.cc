#include "gcc_controller.h"

#include <algorithm>
#include <cmath>

#include "log_system/log_system.h"

GccController::GccController()
    : delay_gradient_(0.0),
      adaptive_threshold_(kInitialThreshold),
      last_arrival_time_us_(0),
      last_send_time_us_(0),
      overuse_counter_(0),
      delay_based_bitrate_kbps_(1000),
      loss_based_bitrate_kbps_(30000),
      target_bitrate_kbps_(1000),
      total_packets_received_(0),
      total_packets_lost_(0),
      min_bitrate_kbps_(100),
      max_bitrate_kbps_(30000) {
  last_overuse_time_ = std::chrono::steady_clock::now();
  last_increase_time_ = std::chrono::steady_clock::now();
}

void GccController::SetInitialBitrate(int kbps) {
  std::lock_guard<std::mutex> lock(mutex_);
  delay_based_bitrate_kbps_ = kbps;
  loss_based_bitrate_kbps_ = kbps;
  target_bitrate_kbps_ = kbps;
}

void GccController::SetBitrateRange(int min_kbps, int max_kbps) {
  std::lock_guard<std::mutex> lock(mutex_);
  min_bitrate_kbps_ = min_kbps;
  max_bitrate_kbps_ = max_kbps;
}

int GccController::GetTargetBitrateKbps() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return target_bitrate_kbps_;
}

void GccController::OnTransportFeedback(const TransportFeedback& feedback) {
  std::lock_guard<std::mutex> lock(mutex_);
  UpdateDelayEstimate(feedback);
  BandwidthUsage usage = DetectOveruse();
  UpdateDelayBasedRate(usage);
  target_bitrate_kbps_ = ComputeFinalBitrate();
}

void GccController::OnLossReport(const LossReport& report) {
  std::lock_guard<std::mutex> lock(mutex_);
  int lost = static_cast<int>(report.packets.size());
  // Approximate total: assume loss report covers a window of packets
  // We use the loss_window to track ratio over time
  total_packets_lost_ += lost;
  UpdateLossBasedRate(lost, lost + 10);  // rough estimate: lost out of (lost+10)
  target_bitrate_kbps_ = ComputeFinalBitrate();
}

void GccController::UpdateDelayEstimate(const TransportFeedback& feedback) {
  if (feedback.packets.size() < 2) {
    total_packets_received_ += static_cast<int>(feedback.packets.size());
    return;
  }

  total_packets_received_ += static_cast<int>(feedback.packets.size());

  // Compute inter-packet delay variation using first and last packet in batch
  // This approximates the "inter-group" delay model from the GCC paper
  const auto& first = feedback.packets.front();
  const auto& last = feedback.packets.back();

  int64_t arrival_delta_us = last.arrival_time_us - first.arrival_time_us;

  // Send time delta: use send_time_store or approximate from sequence numbers
  // For now, approximate send delta from expected pacing
  // In the real system, the sender attaches send timestamps; here we use
  // the arrival spread as a proxy for both
  if (last_arrival_time_us_ == 0) {
    // First feedback — initialize state
    last_arrival_time_us_ = last.arrival_time_us;
    return;
  }

  // Inter-group delay = (arrival_now - arrival_prev) - (send_now - send_prev)
  // Without send timestamps in this feedback, we estimate using the
  // expected inter-packet spacing at current bitrate
  int64_t inter_group_arrival = first.arrival_time_us - last_arrival_time_us_;

  // Expected send spacing: total bytes in batch / bitrate
  // Approximate: packets * 1400 bytes / (bitrate_kbps * 1000 / 8) * 1e6 us
  int packets_in_batch = static_cast<int>(feedback.packets.size());
  double bytes_in_batch = packets_in_batch * 1400.0;
  double expected_spacing_us = 0.0;
  if (delay_based_bitrate_kbps_ > 0) {
    expected_spacing_us = bytes_in_batch / (delay_based_bitrate_kbps_ * 1000.0 / 8.0) * 1e6;
  }

  // Delay gradient = inter_group_arrival - expected_spacing (in ms)
  double gradient_ms = (inter_group_arrival - expected_spacing_us) / 1000.0;

  // Exponential smoothing
  delay_gradient_ = kDelayGradientSmoothing * delay_gradient_ +
                    (1.0 - kDelayGradientSmoothing) * gradient_ms;

  last_arrival_time_us_ = last.arrival_time_us;
}

GccController::BandwidthUsage GccController::DetectOveruse() {
  if (delay_gradient_ > adaptive_threshold_) {
    overuse_counter_++;
    // Adapt threshold upward when gradient is high (reduces false positives)
    adaptive_threshold_ = std::min(
        kMaxThreshold,
        adaptive_threshold_ + (delay_gradient_ - adaptive_threshold_) * 0.01);
    if (overuse_counter_ >= kOveruseCountThreshold) {
      return BandwidthUsage::kOveruse;
    }
    return BandwidthUsage::kNormal;
  } else if (delay_gradient_ < -adaptive_threshold_) {
    overuse_counter_ = 0;
    // Adapt threshold downward during underuse
    adaptive_threshold_ = std::max(
        kMinThreshold,
        adaptive_threshold_ * 0.99);
    return BandwidthUsage::kUnderuse;
  } else {
    overuse_counter_ = 0;
    return BandwidthUsage::kNormal;
  }
}

void GccController::UpdateDelayBasedRate(BandwidthUsage usage) {
  auto now = std::chrono::steady_clock::now();

  switch (usage) {
    case BandwidthUsage::kOveruse: {
      // Multiplicative decrease
      delay_based_bitrate_kbps_ = static_cast<int>(
          delay_based_bitrate_kbps_ * kMultiplicativeDecrease);
      delay_based_bitrate_kbps_ = std::max(delay_based_bitrate_kbps_,
                                            min_bitrate_kbps_);
      overuse_counter_ = 0;
      last_overuse_time_ = now;
      LOG(INFO) << "[GCC] Overuse detected, decrease to "
                << delay_based_bitrate_kbps_ << " kbps";
      break;
    }
    case BandwidthUsage::kUnderuse: {
      // Additive increase (rate-limited)
      auto since_last = std::chrono::duration_cast<std::chrono::milliseconds>(
          now - last_increase_time_).count();
      if (since_last >= kIncreaseIntervalMs) {
        delay_based_bitrate_kbps_ += kAdditiveIncreaseKbps;
        delay_based_bitrate_kbps_ = std::min(delay_based_bitrate_kbps_,
                                              max_bitrate_kbps_);
        last_increase_time_ = now;
      }
      break;
    }
    case BandwidthUsage::kNormal:
      // Hold current rate; can still do slow additive increase
      {
        auto since_last = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_increase_time_).count();
        auto since_overuse = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_overuse_time_).count();
        // Only increase if no recent overuse (last 1s)
        if (since_last >= kIncreaseIntervalMs && since_overuse > 1000) {
          delay_based_bitrate_kbps_ += kAdditiveIncreaseKbps / 2;
          delay_based_bitrate_kbps_ = std::min(delay_based_bitrate_kbps_,
                                                max_bitrate_kbps_);
          last_increase_time_ = now;
        }
      }
      break;
  }
}

void GccController::UpdateLossBasedRate(int packets_lost, int packets_total) {
  loss_window_.push_back({packets_lost, packets_total});
  if (static_cast<int>(loss_window_.size()) > kLossWindowSize) {
    loss_window_.pop_front();
  }

  // Compute windowed loss ratio
  int total_lost = 0, total_count = 0;
  for (const auto& [lost, total] : loss_window_) {
    total_lost += lost;
    total_count += total;
  }

  double loss_ratio = total_count > 0
      ? static_cast<double>(total_lost) / total_count
      : 0.0;

  if (loss_ratio > kLossThreshold) {
    loss_based_bitrate_kbps_ = static_cast<int>(
        loss_based_bitrate_kbps_ * kLossDecreaseFactor);
    loss_based_bitrate_kbps_ = std::max(loss_based_bitrate_kbps_,
                                         min_bitrate_kbps_);
    LOG(INFO) << "[GCC] Loss ratio=" << loss_ratio
              << ", loss-based rate=" << loss_based_bitrate_kbps_ << " kbps";
  } else if (loss_ratio < kLossThreshold / 2.0) {
    // Slowly recover loss-based estimate
    loss_based_bitrate_kbps_ = std::min(
        loss_based_bitrate_kbps_ + kAdditiveIncreaseKbps,
        max_bitrate_kbps_);
  }
}

int GccController::ComputeFinalBitrate() const {
  int final_kbps = std::min(delay_based_bitrate_kbps_, loss_based_bitrate_kbps_);
  return std::clamp(final_kbps, min_bitrate_kbps_, max_bitrate_kbps_);
}
