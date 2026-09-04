#include "congestion_window_pushback_controller.h"

#include <algorithm>
#include <cmath>

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
    uint32_t bitrate_bps, int64_t now_ms) {
  // No window yet (or zero) -> pushback inactive, pass the estimate through.
  if (current_data_window_bytes_ <= 0) {
    last_update_ms_ = now_ms;
    return bitrate_bps;
  }
  int64_t total_bytes = outstanding_bytes_;
  if (add_pacing_) {
    total_bytes += pacing_bytes_;
  }
  double fill_ratio =
      total_bytes / static_cast<double>(current_data_window_bytes_);

  // A drained queue can release pushback immediately. For all gradual
  // adjustments, scale by elapsed time so receiving ten feedback messages in
  // one millisecond has the same effect as receiving one. Cap a single update
  // interval so a long feedback outage cannot cause an unbounded jump.
  if (fill_ratio < 0.1) {
    encoding_rate_ratio_ = 1.0;
    last_update_ms_ = now_ms;
  } else {
    if (last_update_ms_ < 0 || now_ms < last_update_ms_) {
      last_update_ms_ = now_ms;
    } else if (now_ms > last_update_ms_) {
      int64_t elapsed_ms =
          std::min(now_ms - last_update_ms_, kMaxElapsedMs);
      double intervals = elapsed_ms / kAdjustmentIntervalMs;
      double factor = fill_ratio > 1.5 ? 0.9
                    : fill_ratio > 1.0 ? 0.95
                                       : 1.05;
      encoding_rate_ratio_ *= std::pow(factor, intervals);
      encoding_rate_ratio_ = std::min(encoding_rate_ratio_, 1.0);
      last_update_ms_ = now_ms;
    }
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
