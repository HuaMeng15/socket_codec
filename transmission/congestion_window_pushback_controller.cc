#include "congestion_window_pushback_controller.h"

#include <algorithm>

CongestionWindowPushbackController::CongestionWindowPushbackController(
    uint32_t min_pushback_target_bitrate_bps, bool add_pacing)
    : add_pacing_(add_pacing),
      min_pushback_target_bitrate_bps_(min_pushback_target_bitrate_bps) {}

void CongestionWindowPushbackController::UpdateOutstandingData(
    int64_t outstanding_bytes) {
  outstanding_bytes_ = outstanding_bytes;
}

void CongestionWindowPushbackController::UpdatePacingQueue(
    int64_t pacing_bytes) {
  pacing_bytes_ = pacing_bytes;
}

void CongestionWindowPushbackController::SetDataWindow(
    int64_t data_window_bytes) {
  current_data_window_bytes_ = data_window_bytes;
}

uint32_t CongestionWindowPushbackController::UpdateTargetBitrate(
    uint32_t bitrate_bps) {
  // No window yet (or zero) -> pushback inactive, pass the estimate through.
  if (current_data_window_bytes_ <= 0) {
    return bitrate_bps;
  }
  int64_t total_bytes = outstanding_bytes_;
  if (add_pacing_) {
    total_bytes += pacing_bytes_;
  }
  double fill_ratio =
      total_bytes / static_cast<double>(current_data_window_bytes_);
  if (fill_ratio > 1.5) {
    encoding_rate_ratio_ *= 0.9;
  } else if (fill_ratio > 1.0) {
    encoding_rate_ratio_ *= 0.95;
  } else if (fill_ratio < 0.1) {
    encoding_rate_ratio_ = 1.0;
  } else {
    encoding_rate_ratio_ *= 1.05;
    encoding_rate_ratio_ = std::min(encoding_rate_ratio_, 1.0);
  }
  uint32_t adjusted_target_bitrate_bps =
      static_cast<uint32_t>(bitrate_bps * encoding_rate_ratio_);
  // Below the floor, clamp to min(estimate, floor): never push the target
  // above what the BWE believes, but don't starve below the floor either.
  if (adjusted_target_bitrate_bps < min_pushback_target_bitrate_bps_) {
    return std::min(bitrate_bps, min_pushback_target_bitrate_bps_);
  }
  return adjusted_target_bitrate_bps;
}
