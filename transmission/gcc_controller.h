#ifndef TRANSMISSION_GCC_CONTROLLER_H
#define TRANSMISSION_GCC_CONTROLLER_H

#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

#include "alr_detector.h"
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
   * True while the bandwidth prober is actively probing (send rate elevated
   * above the estimate to test for headroom). The pacer uses this to fill idle
   * time with padding so the probe's measured received rate reflects true link
   * capacity. Thread-safe.
   */
  bool IsProbing() const;

  /**
   * Report total packets sent in a period. Call periodically so the loss
   * fraction can be computed correctly: loss_fraction = lost / sent.
   * Without this, OnLossReport can only approximate.
   */
  void OnPacketsSent(int count);

  /**
   * Report wire bytes actually sent since the last call, for ALR detection.
   * Call once per feedback batch (bytes come from Pacer::ConsumeBytesSent()).
   * Feeds the AlrDetector; when it reports application-limited, the prober is
   * armed to fire a periodic probe. A greedy encoder that fills the pipe never
   * trips this (WebRTC-faithful); a variable/VBR encoder does on simple scenes.
   */
  void OnBytesSent(size_t bytes_sent);

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
  // Read the current acknowledged-throughput estimate (the sliding-window
  // measured received rate). Test-only.
  double GetAckedBitrateKbpsForTesting() const;
  // Re-enable the real sliding-window acked estimator after a prior
  // SetAckedBitrateForTesting froze it. Resets the window. Test-only.
  void EnableAckedEstimatorForTesting();

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
  // elapsed_ms: real elapsed time since the previous sample (arrival-time
  // delta), used to accumulate "time over using". Arrival deltas (not send
  // deltas) so burst-paced sends don't collapse the overuse timer.
  BandwidthUsage Detect(double trendline_slope, double elapsed_ms,
                        int64_t now_ms);
  void UpdateAdaptiveThreshold(double modified_trend, int64_t now_ms);

  // --- Rate controller ---
  void UpdateDelayBasedRate(BandwidthUsage usage, int64_t now_ms);
  void UpdateLossBasedRate(double loss_fraction);
  // Periodically (time-based) recompute the loss-based estimate over the
  // accumulated window. Called from the feedback path so the estimate tracks
  // upward when loss is low and downward when loss is high.
  void MaybeUpdateLossRate(int64_t now_ms);
  // Under low loss, keep loss_based == delay_based every batch. WebRTC caps the
  // loss-based target at the delay-based estimate; with no loss the two coincide.
  // Doing this continuously (not only on the kLossUpdateIntervalMs cadence)
  // stops a stale loss_based from pinning ComputeFinalBitrate below a
  // just-committed delay_based (e.g. immediately after a successful probe), and
  // keeps loss_based a correct base for the next decrease when loss rises.
  void MaybeSyncLossToDelay();
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
  double last_modified_trend_ = 0.0;  // last modified_trend (for trace logging)
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
  // Measured from feedback as a sliding window of received bytes over time:
  // rate = sum(recv_size over window) * 8 / window_span. A time window (vs a
  // single feedback batch) is robust to bursty arrivals — a batch whose
  // packets happen to land close together can't inflate the estimate past the
  // true link rate, because the window span still reflects real elapsed time.
  void UpdateAckedBitrate(const TransportFeedback& feedback);
  double acked_bitrate_kbps_;
  bool acked_frozen_for_testing_ = false;  // pin acked in detector unit tests
  // Sliding window of (arrival_time_us, recv_bytes) samples, keyed on the
  // receiver-clock arrival offset carried in the feedback.
  struct AckedSample {
    int64_t arrival_us;
    int bytes;
  };
  std::deque<AckedSample> acked_window_;
  int64_t acked_window_bytes_ = 0;  // running sum of bytes in the window
  static constexpr int64_t kAckedWindowUs = 1000000;  // 1s sliding window
  static constexpr int kAckedMinSamples = 2;          // need ≥2 for a span

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
  // Most recent measured loss fraction. Under low loss we keep loss_based
  // pinned to delay_based every batch (not just on the periodic cadence), so a
  // freshly committed delay_based (e.g. right after a probe) is never dragged
  // down by a stale loss_based in ComputeFinalBitrate. Defaults to 0 (low-loss
  // regime) before the first loss measurement.
  double last_loss_fraction_;
  static constexpr int kLossUpdateIntervalMs = 500;
  static constexpr int kLossUpdateMinPackets = 20;

  // Final rate
  int target_bitrate_kbps_;
  int min_bitrate_kbps_;
  int max_bitrate_kbps_;

  // Bandwidth probing
  BandwidthProber prober_;
  AlrDetector alr_detector_;
  // Probe resolution bookkeeping (GCC side), following WebRTC's
  // ProbeBitrateEstimator: accumulate the probe traffic's received bytes and
  // arrival span across feedback batches starting when the probe begins, and
  // resolve once the accumulated span is long enough to be a trustworthy
  // measurement. A *minimum span* guard is essential — a single batch whose
  // packets arrive bunched (delivery jitter, scheduler coalescing, a burst
  // racing ahead of the bottleneck) spans only microseconds and would report a
  // rate many times the real capacity. Requiring a real span before committing
  // makes the measured probe rate robust to that bunching.
  bool probe_active_;
  int probe_floor_kbps_;
  // Accumulated probe-traffic measurement since the probe started.
  int64_t probe_first_arrival_us_;   // arrival of the first probe-window packet
  int64_t probe_last_arrival_us_;    // arrival of the most recent packet
  double probe_recv_bytes_;          // received bytes since the first (exclusive)
  bool probe_first_seen_;            // have we recorded the first packet yet
  // Per-batch received rate from the most recent feedback (kbps), kept for
  // diagnostics. The probe now resolves from the accumulated measurement below.
  double last_received_rate_kbps_;
  // WebRTC kTargetUtilizationFraction: when the link is saturated by the probe,
  // commit slightly below the measured receive rate to avoid immediate overuse.
  static constexpr double kProbeUtilizationFraction = 0.95;
  static constexpr double kProbeAbortDelayMs = 80.0;  // abort if queue grows past this
  // Minimum accumulated arrival span before a probe may commit. Below this, the
  // measurement is dominated by bunching noise, not real throughput.
  static constexpr int64_t kProbeMinSpanUs = 30000;  // 30 ms

  // Fake clock for testing (nullptr = use real clock)
  int64_t* fake_clock_ms_;
};

#endif  // TRANSMISSION_GCC_CONTROLLER_H
