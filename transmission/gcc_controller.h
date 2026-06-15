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
 *   - Inter-arrival delta computation between packet groups
 *   - Accumulated delay tracked per sample
 *   - Trendline estimator: linear least-squares over a sliding window
 *     (kTrendlineWindowSize = 20 samples)
 *   - Overuse detection: trendline * kThresholdGain (4.0) vs adaptive threshold
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

 private:
  int64_t NowMs() const;
  // --- Trendline estimator ---
  struct DelayPoint {
    double smoothed_delay;  // Accumulated delay (smoothed)
    double arrival_time_ms;
  };

  void UpdateTrendline(const TransportFeedback& feedback);
  double ComputeTrendlineSlope() const;

  // --- Overuse detector ---
  enum class BandwidthUsage { kUnderuse, kNormal, kOveruse };
  BandwidthUsage Detect(double trendline_slope);
  void UpdateAdaptiveThreshold(double modified_trend, int64_t now_ms);

  // --- Rate controller ---
  void UpdateDelayBasedRate(BandwidthUsage usage, int64_t now_ms);
  void UpdateLossBasedRate(double loss_fraction);
  int ComputeFinalBitrate() const;

  mutable std::mutex mutex_;

  // Trendline state
  std::deque<DelayPoint> trendline_window_;
  double accumulated_delay_;
  double smoothed_delay_;
  int64_t first_arrival_ms_;
  int64_t prev_send_time_ms_;
  int64_t prev_arrival_time_ms_;
  int num_deltas_;

  // WebRTC trendline constants
  static constexpr int kTrendlineWindowSize = 20;
  static constexpr double kTrendlineSmoothingCoeff = 0.9;
  static constexpr double kThresholdGain = 4.0;

  // Adaptive threshold state
  double adaptive_threshold_;
  int64_t last_threshold_update_ms_;
  static constexpr double kInitialThreshold = 12.5;  // ms
  static constexpr double kMinThreshold = 6.0;
  static constexpr double kMaxThreshold = 600.0;
  // Threshold adaptation rates (per ms)
  static constexpr double kThresholdUp = 0.0087;    // ~k_u in WebRTC
  static constexpr double kThresholdDown = 0.039;   // ~k_d in WebRTC

  // Overuse detection
  int overuse_counter_;
  static constexpr int kOveruseCountThreshold = 3;
  int64_t last_overuse_time_ms_;

  // Delay-based rate control
  int delay_based_bitrate_kbps_;
  int64_t last_increase_time_ms_;
  // AIMD parameters (matching WebRTC)
  static constexpr double kMultiplicativeDecrease = 0.85;
  // Increase: ~8% of current bitrate per second
  static constexpr double kIncreaseRatePerSecond = 0.08;
  static constexpr int kMinIncreaseKbps = 50;  // Floor for additive increase

  // Loss-based rate control (WebRTC send_side_bandwidth_estimation)
  int loss_based_bitrate_kbps_;
  static constexpr double kLossIncreaseThreshold = 0.02;   // < 2%: increase
  static constexpr double kLossHoldThreshold = 0.10;       // < 10%: hold
  // >= 10%: decrease by (1 - 0.5 * loss_fraction)

  // Loss tracking: separate sent and lost counters for accurate fraction
  int packets_sent_since_last_loss_update_;
  int packets_lost_since_last_loss_update_;
  static constexpr int kLossUpdateWindowSize = 20;

  // Final rate
  int target_bitrate_kbps_;
  int min_bitrate_kbps_;
  int max_bitrate_kbps_;

  // Bandwidth probing
  BandwidthProber prober_;

  // Fake clock for testing (nullptr = use real clock)
  int64_t* fake_clock_ms_;
};

#endif  // TRANSMISSION_GCC_CONTROLLER_H
