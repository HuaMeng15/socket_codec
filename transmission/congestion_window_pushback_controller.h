#ifndef TRANSMISSION_CONGESTION_WINDOW_PUSHBACK_CONTROLLER_H
#define TRANSMISSION_CONGESTION_WINDOW_PUSHBACK_CONTROLLER_H

#include <cstdint>

/**
 * CongestionWindowPushbackController: faithful port of WebRTC's
 * modules/congestion_controller/goog_cc/congestion_window_pushback_controller.
 *
 * The delay-based/loss-based BWE only reacts to the *derivative* of delay, so
 * when link capacity collapses it walks the target down slowly and a large
 * standing queue forms (multi-second latency). This controller is the piece
 * that makes real WebRTC crash the encoder target to a floor within ~1 RTT and
 * hold it there until the in-flight backlog drains — purely from a
 * bytes-in-flight vs congestion-window ratio, with NO packet loss required.
 *
 * Mechanism:
 *   fill_ratio = outstanding_bytes / data_window
 *     > 1.5 : encoding_rate_ratio decays by 0.9 per 100ms
 *     > 1.0 : encoding_rate_ratio decays by 0.95 per 100ms
 *     < 0.1 : encoding_rate_ratio  = 1.0    (backlog drained -> release)
 *     else  : encoding_rate_ratio grows by 1.05 per 100ms (capped at 1.0)
 *   target = bwe * encoding_rate_ratio, floored at min_pushback_target.
 *
 * encoding_rate_ratio_ is persistent, but its evolution is based on elapsed
 * time rather than the number of feedback callbacks. This keeps equivalent
 * network conditions equivalent under packet-batched and per-packet feedback,
 * and avoids collapsing the encoder target merely because feedback is frequent.
 */
class CongestionWindowPushbackController {
 public:
  // min_pushback_target_bitrate_bps mirrors the field-trial MinBitrate
  // (WebRTC default 30000). add_pacing mirrors the (default-off)
  // WebRTC-AddPacingToCongestionWindowPushback trial.
  explicit CongestionWindowPushbackController(
      uint32_t min_pushback_target_bitrate_bps, bool add_pacing = false);

  void UpdateOutstandingData(int64_t outstanding_bytes);
  void UpdatePacingQueue(int64_t pacing_bytes);
  // Returns the pushback-adjusted target. If no data window has been set yet
  // (or it is zero) the input bitrate is returned unchanged.
  uint32_t UpdateTargetBitrate(uint32_t bitrate_bps, int64_t now_ms);
  void SetDataWindow(int64_t data_window_bytes);

  // Introspection for logging / tests.
  double encoding_rate_ratio() const { return encoding_rate_ratio_; }
  int64_t data_window_bytes() const { return current_data_window_bytes_; }

 private:
  const bool add_pacing_;
  const uint32_t min_pushback_target_bitrate_bps_;
  // <0 means "unset" (mirrors WebRTC's absl::optional data window).
  int64_t current_data_window_bytes_ = -1;
  int64_t outstanding_bytes_ = 0;
  int64_t pacing_bytes_ = 0;
  double encoding_rate_ratio_ = 1.0;
  int64_t last_update_ms_ = -1;
  static constexpr double kAdjustmentIntervalMs = 100.0;
  static constexpr int64_t kMaxElapsedMs = 500;
};

#endif  // TRANSMISSION_CONGESTION_WINDOW_PUSHBACK_CONTROLLER_H
