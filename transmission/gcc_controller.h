#ifndef TRANSMISSION_GCC_CONTROLLER_H
#define TRANSMISSION_GCC_CONTROLLER_H

#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>

#include "bandwidth_prober.h"
#include "congestion_controller.h"

/**
 * GccController: Google Congestion Control implementation.
 *
 * Delay-based component:
 *   - Computes inter-arrival time deltas between consecutive packet groups
 *   - Estimates delay gradient via exponential moving average
 *   - Overuse detector with adaptive threshold
 *   - AIMD rate control: multiplicative decrease on overuse,
 *     additive increase on underuse
 *
 * Loss-based component:
 *   - Tracks packet loss ratio from LossReports
 *   - Multiplicative decrease when loss exceeds threshold
 *
 * Final bitrate = min(delay-based estimate, loss-based estimate),
 * clamped to [min_bitrate, max_bitrate].
 */
class GccController : public CongestionController {
 public:
  GccController();
  ~GccController() override = default;

  void OnTransportFeedback(const TransportFeedback& feedback) override;
  void OnLossReport(const LossReport& report) override;
  int GetTargetBitrateKbps() const override;
  void SetBitrateRange(int min_kbps, int max_kbps) override;

  /** Set initial bitrate estimate. */
  void SetInitialBitrate(int kbps);

 private:
  // Delay-based rate control
  enum class BandwidthUsage { kUnderuse, kNormal, kOveruse };

  struct PacketGroup {
    int64_t send_time_us;
    int64_t arrival_time_us;
    int size_bytes;
  };

  void UpdateDelayEstimate(const TransportFeedback& feedback);
  BandwidthUsage DetectOveruse();
  void UpdateDelayBasedRate(BandwidthUsage usage);

  // Loss-based rate control
  void UpdateLossBasedRate(int packets_lost, int packets_total);

  // Clamp and combine
  int ComputeFinalBitrate() const;

  mutable std::mutex mutex_;

  // Delay estimation state
  double delay_gradient_;            // Smoothed inter-group delay gradient (ms)
  double adaptive_threshold_;        // Overuse detection threshold
  int64_t last_arrival_time_us_;
  int64_t last_send_time_us_;
  int overuse_counter_;              // Consecutive overuse signals
  std::chrono::steady_clock::time_point last_overuse_time_;

  // Rate control state
  int delay_based_bitrate_kbps_;
  int loss_based_bitrate_kbps_;
  int target_bitrate_kbps_;

  // Loss tracking
  int total_packets_received_;
  int total_packets_lost_;
  std::deque<std::pair<int, int>> loss_window_;  // (lost, total) per report
  static constexpr int kLossWindowSize = 10;

  // AIMD parameters
  static constexpr double kMultiplicativeDecrease = 0.85;
  static constexpr int kAdditiveIncreaseKbps = 100;  // per feedback round
  static constexpr double kDelayGradientSmoothing = 0.9;
  static constexpr double kThresholdGain = 4.0;
  static constexpr double kInitialThreshold = 12.5;  // ms
  static constexpr double kMinThreshold = 6.0;
  static constexpr double kMaxThreshold = 600.0;
  static constexpr double kLossThreshold = 0.1;      // 10% loss triggers decrease
  static constexpr double kLossDecreaseFactor = 0.8;
  static constexpr int kOveruseCountThreshold = 3;   // consecutive signals before action

  // Bounds
  int min_bitrate_kbps_;
  int max_bitrate_kbps_;

  // Bandwidth probing
  BandwidthProber prober_;

  // Time tracking for additive increase pacing
  std::chrono::steady_clock::time_point last_increase_time_;
  static constexpr int kIncreaseIntervalMs = 200;  // increase every 200ms max
};

#endif  // TRANSMISSION_GCC_CONTROLLER_H
