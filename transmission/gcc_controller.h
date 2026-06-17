#ifndef TRANSMISSION_GCC_CONTROLLER_H
#define TRANSMISSION_GCC_CONTROLLER_H

#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

#include "bandwidth_prober.h"
#include "congestion_controller.h"

/**
 * GccController: Google Congestion Control aligned with WebRTC implementation.
 *
 * Delay-based component (references: trendline_estimator.cc, delay_based_bwe.cc):
 *   - Per-packet inter-arrival delta (recv_delta - send_delta), accumulated
 *     and EWMA-smoothed (coeff 0.9)
 *   - Trendline estimator: least-squares slope over a duration-based window
 *     (<= kTrendlineWindowMs of arrival span), refit when the window is trimmed
 *   - Overuse detection: modified_trend = min(num_deltas, 60) * slope * gain(4)
 *     vs adaptive threshold; requires sustained over-use (time_over_using >
 *     10ms), overuse_counter > 1, and trend non-decreasing
 *   - Adaptive threshold: grows at k_up (0.0087) during overuse,
 *     decays at k_down (0.039) otherwise, clamped to [6, 600] ms
 *   - Rate control: AIMD
 *     - Overuse: multiplicative decrease (0.85 × estimated_throughput)
 *     - Normal/Underuse: additive increase (~8% of current per second)
 *
 * Loss-based component (reference: send_side_bandwidth_estimation.cc):
 *   - loss < 2%: can increase
 *   - 2% ≤ loss < 10%: hold
 *   - loss ≥ 10%: decrease by (1 - 0.5 * loss_fraction)
 *
 * Final bitrate = min(delay-based, loss-based), clamped to bounds.
 * Prober may temporarily override when actively probing.
 */
class GccController : public CongestionController {
 public:
  GccController();
  ~GccController() override = default;

  void OnTransportFeedback(const TransportFeedback& feedback) override;
  void OnLossReport(const LossReport& report) override;
  int GetTargetBitrateKbps() const override;
  void SetBitrateRange(int min_kbps, int max_kbps) override;

  void SetInitialBitrate(int kbps);

  /**
   * Report total packets sent in a period. Call periodically so the loss
   * fraction can be computed correctly: loss_fraction = lost / sent.
   * Without this, OnLossReport can only approximate.
   */
  void OnPacketsSent(int count);

  /**
   * Inject a fake clock for deterministic testing. When set, all internal
   * time reads use this value instead of steady_clock. Advance it manually.
   * Pass nullptr to revert to real clock.
   */
  void SetClockForTesting(int64_t* clock_ms);

  // --- Test-only getters for internal state ---
  int GetDelayBasedBitrateKbps() const;
  int GetLossBasedBitrateKbps() const;
  double GetAdaptiveThreshold() const;
  int GetOveruseCounter() const;
  // Override the acknowledged-throughput estimate for deterministic
  // delay-based unit tests (decouples them from synthetic acked estimation).
  void SetAckedBitrateForTesting(double kbps);

 private:
  int64_t NowMs() const;
  // --- Trendline estimator (WebRTC trendline_estimator.cc) ---
  struct DelayPoint {
    double arrival_time_ms;   // arrival time relative to first sample (ms)
    double smoothed_delay;    // EWMA-smoothed accumulated delay (ms)
  };

  // Process all packets in a feedback batch; returns the latest bandwidth-usage
  // hypothesis (matches WebRTC, which calls Detect per packet).
  enum class BandwidthUsage { kUnderuse, kNormal, kOveruse };
  BandwidthUsage UpdateTrendline(const TransportFeedback& feedback);
  double ComputeTrendlineSlope() const;

  // --- Overuse detector ---
  BandwidthUsage Detect(double trendline_slope, double ts_delta_ms,
                        int64_t now_ms);
  void UpdateAdaptiveThreshold(double modified_trend, int64_t now_ms);

  // --- Rate controller ---
  void UpdateDelayBasedRate(BandwidthUsage usage, int64_t now_ms);
  void UpdateLossBasedRate(double loss_fraction);
  // Periodically (time-based) recompute the loss-based estimate over the
  // accumulated window. Called from the feedback path so the estimate tracks
  // upward when loss is low and downward when loss is high.
  void MaybeUpdateLossRate(int64_t now_ms);
  int ComputeFinalBitrate() const;

  mutable std::mutex mutex_;

  // Trendline state
  std::deque<DelayPoint> trendline_window_;
  double accumulated_delay_;
  double smoothed_delay_;
  int64_t first_arrival_ms_;
  double prev_send_ms_d_;       // previous packet send time (ms)
  double prev_arrival_ms_d_;    // previous packet arrival time (ms)
  int num_deltas_;

  // WebRTC trendline constants (trendline_estimator.cc)
  static constexpr double kTrendlineSmoothingCoeff = 0.9;
  static constexpr double kThresholdGain = 4.0;
  static constexpr int kMinNumDeltas = 60;       // modified_trend scale cap
  static constexpr int kDeltaCounterMax = 1000;  // num_deltas saturation
  static constexpr double kTrendlineWindowMs = 100.0;  // duration-based window
  static constexpr double kMaxAdaptOffsetMs = 15.0;    // threshold spike guard

  // Adaptive threshold state
  double adaptive_threshold_;
  int64_t last_threshold_update_ms_;
  static constexpr double kInitialThreshold = 12.5;  // ms
  static constexpr double kMinThreshold = 6.0;
  static constexpr double kMaxThreshold = 600.0;
  // Threshold adaptation rates (per ms)
  static constexpr double kThresholdUp = 0.0087;    // k_up in WebRTC
  static constexpr double kThresholdDown = 0.039;   // k_down in WebRTC

  // Overuse detection (WebRTC OveruseDetector / TrendlineEstimator::Detect).
  // Overuse is signaled only when the modified trend stays above threshold for
  // a sustained time (overusing_time_threshold) AND the trend is not
  // decreasing (trend >= prev_trend) AND it has occurred more than once.
  int overuse_counter_;
  int64_t last_overuse_time_ms_;
  double time_over_using_ms_;     // accumulated time over threshold (-1 = reset)
  double prev_trend_;             // previous raw trendline slope
  static constexpr double kOverusingTimeThresholdMs = 10.0;  // WebRTC default

  // Delay-based rate control
  int delay_based_bitrate_kbps_;
  int64_t last_increase_time_ms_;
  // AIMD parameters (matching WebRTC)
  static constexpr double kMultiplicativeDecrease = 0.85;
  // Increase: ~8% of current bitrate per second
  static constexpr double kIncreaseRatePerSecond = 0.08;
  static constexpr int kMinIncreaseKbps = 50;  // Floor for additive increase

  // Acknowledged throughput estimate (WebRTC AimdRateControl uses this to cap
  // the increase at 1.5x and to snap the decrease to measured throughput).
  // Estimated from feedback: total acked payload bytes over the arrival span.
  void UpdateAckedBitrate(const TransportFeedback& feedback);
  double acked_bitrate_kbps_;
  bool acked_frozen_for_testing_ = false;  // pin acked in detector unit tests
  static constexpr double kAckedSmoothingCoeff = 0.95;  // EWMA on acked rate
  static constexpr int kPayloadBytesPerPacket = 1454;   // MTU payload estimate

  // Loss-based rate control (WebRTC send_side_bandwidth_estimation)
  int loss_based_bitrate_kbps_;
  static constexpr double kLossIncreaseThreshold = 0.02;   // < 2%: increase
  static constexpr double kLossHoldThreshold = 0.10;       // < 10%: hold
  // >= 10%: decrease by (1 - 0.5 * loss_fraction)

  // Loss tracking: separate sent and lost counters for accurate fraction.
  // The loss-based estimate is re-evaluated periodically (every
  // kLossUpdateIntervalMs) over the accumulated window, so it can INCREASE
  // during loss-free periods (matching WebRTC's periodic update), not only
  // decrease when a loss report happens to arrive.
  int packets_sent_since_last_loss_update_;
  int packets_lost_since_last_loss_update_;
  int64_t last_loss_update_ms_;
  static constexpr int kLossUpdateIntervalMs = 500;
  static constexpr int kLossUpdateMinPackets = 20;

  // Final rate
  int target_bitrate_kbps_;
  int min_bitrate_kbps_;
  int max_bitrate_kbps_;

  // Bandwidth probing
  BandwidthProber prober_;
  // Probe resolution bookkeeping (GCC side). When a probe is in progress we
  // track when it started and a "floor" (loss estimate just before the probe)
  // so we can abort if the probe rate induces congestion.
  bool probe_active_;
  int64_t probe_started_ms_;
  int probe_floor_kbps_;
  static constexpr double kProbeAbortDelayMs = 80.0;  // abort if queue grows past this
  static constexpr int kProbeEvalWindowMs = 300;      // resolve probe after this

  // Fake clock for testing (nullptr = use real clock)
  int64_t* fake_clock_ms_;
};

#endif  // TRANSMISSION_GCC_CONTROLLER_H
