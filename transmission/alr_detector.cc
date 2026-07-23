#include "alr_detector.h"

#include <algorithm>

#include "log_system/log_system.h"

void AlrDetector::SetEstimatedBitrate(int bitrate_kbps) {
  if (bitrate_kbps <= 0) return;
  // Budget accrues at kBandwidthUsageRatio x the estimate: the sender is
  // considered application-limited once it sends below that fraction.
  budget_target_kbps_ = static_cast<int>(bitrate_kbps * kBandwidthUsageRatio);
  // kbps * ms == bits, so /8 == bytes over the window.
  max_bytes_ =
      static_cast<int64_t>(budget_target_kbps_) * kBudgetWindowMs / 8;
  budget_level_bytes_ = std::clamp<int64_t>(budget_level_bytes_, 0, max_bytes_);
}

void AlrDetector::OnBytesSent(int64_t send_time_ms, size_t bytes_sent) {
  if (max_bytes_ <= 0) return;  // no estimate yet

  if (last_send_time_ms_ < 0) {
    last_send_time_ms_ = send_time_ms;
  }
  int64_t delta_ms = send_time_ms - last_send_time_ms_;
  last_send_time_ms_ = send_time_ms;
  if (delta_ms < 0) delta_ms = 0;

  // Drain by what we actually sent, then accrue what we were allowed to send
  // over the elapsed time. Net-positive budget => under-utilizing => ALR.
  budget_level_bytes_ -= static_cast<int64_t>(bytes_sent);
  budget_level_bytes_ +=
      static_cast<int64_t>(budget_target_kbps_) * delta_ms / 8;
  budget_level_bytes_ = std::clamp<int64_t>(budget_level_bytes_, 0, max_bytes_);

  double ratio = static_cast<double>(budget_level_bytes_) / max_bytes_;
  if (!in_alr_ && ratio > kStartBudgetLevelRatio) {
    in_alr_ = true;
    LOG(INFO) << "[ALR] Entered application-limited region (ratio="
              << ratio << ")";
  } else if (in_alr_ && ratio < kStopBudgetLevelRatio) {
    in_alr_ = false;
    LOG(INFO) << "[ALR] Left application-limited region (ratio=" << ratio
              << ")";
  }
}
