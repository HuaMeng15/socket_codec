#ifndef TRANSMISSION_ALR_DETECTOR_H
#define TRANSMISSION_ALR_DETECTOR_H

#include <cstddef>
#include <cstdint>

/**
 * AlrDetector: Application-Limited Region detector (port of WebRTC's
 * modules/pacing/alr_detector.cc).
 *
 * Tracks the bytes actually sent against a budget that accrues at
 * kBandwidthUsageRatio x the current estimate. When the sender consistently
 * sends LESS than that (a content-limited / VBR encoder on a simple scene),
 * unspent budget builds up and we enter ALR; when it fills the pipe again the
 * budget drains and we leave ALR.
 *
 * ALR is the signal WebRTC uses to gate periodic bandwidth probing: while
 * app-limited, AIMD cannot grow the estimate (nothing pushes on the link), so
 * a probe is the only way to discover freed-up headroom.
 */
class AlrDetector {
 public:
  AlrDetector() = default;

  /** Set the current bandwidth estimate (kbps). Scales the accrual rate. */
  void SetEstimatedBitrate(int bitrate_kbps);

  /** Report bytes actually put on the wire at send_time_ms (incl. padding). */
  void OnBytesSent(int64_t send_time_ms, size_t bytes_sent);

  /** True while the sender is application-limited. */
  bool InAlr() const { return in_alr_; }

 private:
  // WebRTC defaults (percent constants expressed as ratios).
  static constexpr double kBandwidthUsageRatio = 0.65;
  static constexpr double kStartBudgetLevelRatio = 0.80;
  static constexpr double kStopBudgetLevelRatio = 0.50;
  static constexpr int kBudgetWindowMs = 500;

  int budget_target_kbps_ = 0;   // estimate x kBandwidthUsageRatio
  int64_t max_bytes_ = 0;        // kBudgetWindowMs of budget_target_kbps_
  int64_t budget_level_bytes_ = 0;
  int64_t last_send_time_ms_ = -1;
  bool in_alr_ = false;
};

#endif  // TRANSMISSION_ALR_DETECTOR_H
