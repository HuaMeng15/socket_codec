#ifndef TRANSMISSION_GCC_CONTROLLER_H
#define TRANSMISSION_GCC_CONTROLLER_H

#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "alr_detector.h"
#include "bandwidth_prober.h"
#include "congestion_controller.h"
#include "congestion_window_pushback_controller.h"

/**
 * GccController: Google Congestion Control aligned with WebRTC implementation.
 *
 * Delay-based component (references: trendline_estimator.cc, delay_based_bwe.cc):
 *   - Packets are first aggregated into 5ms send-time groups, then group
 *     inter-arrival delta (recv_delta - send_delta) is accumulated and
 *     EWMA-smoothed (coeff 0.9). Grouping avoids packet-size/pacing noise.
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
   * Configure the congestion-window pushback controller (WebRTC field trial
   * "WebRTC-CongestionWindow"). queue_size_ms is the additional time added to
   * the min RTT when sizing the window; min_bitrate_kbps is the pushback floor.
   * Set queue_size_ms <= 0 to disable pushback entirely (legacy behavior).
   */
  void SetCongestionWindowConfig(int queue_size_ms, int min_bitrate_kbps);

  /** Enable or disable periodic ALR discovery probes (startup probes remain). */
  void SetPeriodicAlrProbingEnabled(bool enabled);

  /**
   * Configure congestion-aware probes outside ALR. Zero disables them.
   * Probe opportunities are skipped whenever controller health is uncertain.
   */
  void SetUnconditionalPeriodicProbeIntervalMs(int interval_ms);

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
   * Record a packet at the instant it is put on the wire. Congestion-window
   * accounting is packet based so an ACK or loss report can retire the exact
   * bytes instead of relying on cumulative sent-minus-received totals.
   */
  void OnPacketSent(uint16_t frame_sequence, uint16_t packet_index,
                    size_t wire_bytes);

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
  int64_t GetOutstandingBytesForTesting() const;
  // Re-enable the real sliding-window acked estimator after a prior
  // SetAckedBitrateForTesting froze it. Resets the window. Test-only.
  void EnableAckedEstimatorForTesting();

  /**
   * Get the current network usage state for encoder adaptation.
   * Returns a value representing the aggressiveness of overuse:
   *   < 2.0: normal or underuse (encoder can use relaxed VBV)
   *   >= 2.0: overuse (encoder should tighten VBV for fast adaptation)
   * Aligned with sparkrtc's aggressive_state mechanism.
   */
  double GetNetworkUsageState() const;

 private:
  int64_t NowMs() const;
  // --- Trendline estimator (WebRTC trendline_estimator.cc) ---
  struct DelayPoint {
    double arrival_time_ms;   // arrival time relative to first sample (ms)
    double smoothed_delay;    // EWMA-smoothed accumulated delay (ms)
  };

  struct PacketGroup {
    bool valid = false;
    double first_send_ms = 0.0;
    double last_send_ms = 0.0;
    double first_arrival_ms = 0.0;
    double last_arrival_ms = 0.0;
    int64_t bytes = 0;
    int packets = 0;
    uint16_t last_frame_sequence = 0;
    uint16_t last_packet_index = 0;
  };

  // Process all packets in a feedback batch and return the strongest hypothesis
  // observed. Because this controller updates its rate once per feedback batch,
  // an early overuse sample must not be overwritten by a later normal sample.
  enum class BandwidthUsage { kUnderuse, kNormal, kOveruse };
  BandwidthUsage UpdateTrendline(const TransportFeedback& feedback);
  BandwidthUsage ApplyByteDeliverySignal(BandwidthUsage delay_usage,
                                         int64_t now_ms);
  bool ShouldIgnoreSourceLimitedOveruse(int64_t now_ms) const;
  void ResetTrendlineAfterDiscontinuity();
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
  // Acked throughput the AIMD controller should react to: the instantaneous
  // per-batch received rate while in overuse (fast reaction to a capacity
  // drop), otherwise the smoothed sliding-window rate.
  double EffectiveAckedKbps() const;
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
  bool first_arrival_set_;
  PacketGroup current_group_;
  PacketGroup previous_group_;
  int num_deltas_;

  // WebRTC trendline constants (trendline_estimator.cc)
  static constexpr double kTrendlineSmoothingCoeff = 0.9;
  static constexpr double kThresholdGain = 4.0;
  static constexpr int kMinNumDeltas = 60;       // modified_trend scale cap
  static constexpr int kDeltaCounterMax = 1000;  // num_deltas saturation
  // A 50ms horizon starts fitting within roughly two feedback intervals while
  // still spanning enough 5ms send-time groups to reject isolated jitter. The
  // previous 100ms horizon imposed a hard ~90-100ms capacity-drop discovery
  // delay before the first meaningful slope could be calculated.
  static constexpr double kTrendlineWindowMs = 50.0;  // duration-based window
  static constexpr double kSendTimeGroupMs = 5.0;
  static constexpr double kTrendlineDiscontinuityMs = 1000.0;
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
  // Reduced threshold for early encoder notification (sparkrtc-aligned).
  // When time_over_using exceeds this, encoder should enter aggressive VBV mode
  // before full overuse detection triggers rate decrease.
  static constexpr double kEncoderOveruseThresholdMs = 5.0;

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
  // Sliding window of acknowledged packets. Keeping both send and arrival
  // timestamps lets the delivery detector compare the same packet cohort on
  // the two sides of the path; the clocks need not share an epoch because only
  // spans are compared.
  struct AckedSample {
    int64_t send_us;
    int64_t arrival_us;
    int bytes;
  };
  std::deque<AckedSample> acked_window_;
  int64_t acked_window_bytes_ = 0;  // running sum of bytes in the window
  // Normal delivery decisions use 200ms (roughly six 30fps frames), matching
  // the source-rate horizon and smoothing frame-shaped bursts. The severe
  // capacity-cliff path retains its separate 50ms estimator below.
  static constexpr int64_t kAckedWindowUs = 200000;  // 200ms sliding window
  static constexpr int kAckedMinSamples = 2;          // need ≥2 for a span
  double aligned_sent_bitrate_kbps_ = 0.0;
  double aligned_delivery_ratio_ = 1.0;

  // Secondary, byte-aware congestion signal. Delay trendlines measure queue
  // growth, while this compares send and arrival spans for the same
  // acknowledged packet cohort. The separate aggregate source window below
  // proves that the application is filling its target; packet loss is handled
  // by the loss controller rather than inferred from mismatched byte windows.
  struct SentSample {
    int64_t time_ms;
    int64_t bytes;
  };
  std::deque<SentSample> sent_rate_window_;
  int64_t sent_rate_window_bytes_ = 0;
  double sent_rate_kbps_ = 0.0;
  int64_t low_delivery_start_ms_ = -1;
  bool byte_delivery_overuse_ = false;
  bool severe_capacity_cliff_active_ = false;
  // One permanent AIMD reduction is allowed per congestion episode. Cwnd
  // pushback remains free to lower the effective sending target while the
  // existing backlog drains. Rearm only after a sustained healthy period.
  bool congestion_episode_active_ = false;
  int64_t congestion_recovery_start_ms_ = -1;
  int64_t last_suppressed_overuse_log_ms_ = -1;
  static constexpr int64_t kSentRateWindowMs = 200;
  static constexpr int64_t kByteEstimatorMinSpanMs = 150;
  static constexpr int64_t kByteSignalMinSpanMs = 200;
  static constexpr double kByteDeliveryRatioThreshold = 0.85;
  static constexpr double kByteDeliveryRecoveryRatio = 0.95;
  static constexpr int64_t kCongestionRecoverySpanMs = 300;
  // Recovery requires the outstanding backlog to represent at most 100 ms at
  // the measured delivery rate. This prevents a pushback-limited send rate
  // from inflating the delivery ratio and exposing a stale pre-cliff estimate.
  static constexpr double kCongestionRecoveryMaxDrainMs = 100.0;
  // A 10->1 Mbps-style cliff should not wait for the ordinary 200ms byte
  // confirmation or a second trendline group. Require three independent
  // signals before taking the fast path: delivery collapses below half the
  // offered rate, the delay trend is already over threshold, and accumulated
  // queue growth is substantial. A latch prevents a persistent cliff from
  // applying one multiplicative decrease per feedback packet.
  static constexpr double kSevereDeliveryRatioThreshold = 0.50;
  static constexpr double kSevereQueueGrowthMs = 20.0;
  static constexpr double kSenderUtilizationThreshold = 0.80;
  // A source-limited timing event is considered healthy only while its
  // outstanding bytes represent at most this much media time at the actual
  // send rate. This adapts to bitrate and remains independent of the separate
  // congestion-window queue allowance.
  static constexpr double kSourceLimitedOutstandingTimeMs = 50.0;

  // When overuse is detected we switch the AIMD decrease/increase-cap to use
  // the INSTANTANEOUS per-batch received rate instead of the windowed acked
  // rate — the instant rate reflects the collapsed capacity immediately (the
  // window still averages in pre-drop throughput). We hold this mode until the
  // queue drains enough to register underuse, then switch back to the window.
  bool use_instant_acked_ = false;
  // True only when the recent cross-batch arrival history contains enough span
  // to form a meaningful fast byte-rate estimate. A strict receiver feedback
  // timer can legitimately produce one-packet batches at low bandwidth, so
  // this estimate must be continuous across feedback message boundaries.
  bool instant_acked_sample_valid_ = false;
  double instant_sent_bitrate_kbps_ = 0.0;
  double instant_delivery_ratio_ = 1.0;
  static constexpr int64_t kInstantAckedWindowUs = 50000;
  static constexpr int64_t kMinInstantAckedSpanUs = 5000;

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
  bool periodic_alr_probing_enabled_;
  int unconditional_periodic_probe_interval_ms_ = 0;
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
  // Feedback can contain packets sent before the controller activated the
  // elevated probe rate. Exclude that codec-shaped traffic from the probe
  // throughput sample; only later send timestamps represent the probe.
  int64_t probe_measurement_send_cutoff_us_;
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
  // Delay/cwnd health threshold for optional periodic probing. An accumulated
  // delay above this is unsafe only while its live trend is still positive;
  // the integrated signal can retain a stale offset after the queue drains.
  // Startup does not terminate on this timing-only signal: its measured probe
  // result and byte/loss confirmation are codec-independent saturation evidence.
  static constexpr double kProbeAbortDelayMs = 20.0;
  static constexpr int64_t kPeriodicProbeCongestionCooldownMs = 5000;
  // Minimum accumulated arrival span before a probe may commit. Below this, the
  // measurement is dominated by bunching noise, not real throughput.
  static constexpr int64_t kProbeMinSpanUs = 30000;  // 30 ms

  // --- Congestion-window pushback (WebRTC goog_cc port) -------------------
  // Recomputes the congestion window from the loss-based rate and the min
  // feedback-max RTT, and applies the pushback ratchet to the final target.
  // Disabled when cwnd_queue_size_ms_ <= 0 (mirrors an unset field trial).
  void UpdateCongestionWindowSize();
  void RetireFeedbackPackets(const TransportFeedback& feedback);
  void RetireLostPackets(const LossReport& report);
  void ExpireStaleInflightPackets(int64_t now_ms);
  bool RetireInflightPacket(uint16_t frame_sequence, uint16_t packet_index);
  // nullptr when pushback is disabled (mirrors WebRTC's optional controller).
  std::unique_ptr<CongestionWindowPushbackController> pushback_;
  int cwnd_queue_size_ms_ = 350;      // field-trial QueueSize (additional time)
  int cwnd_min_bitrate_kbps_ = 30;    // field-trial MinBitrate
  int64_t current_data_window_bytes_ = -1;  // <0 = unset (EWMA seed)
  // Exact packet-level in-flight accounting. Media loss reports retire packets
  // explicitly. Probe padding has no frame-loss report, so inactive entries
  // also have an RTT-scaled liveness timeout. The timeout is deliberately much
  // larger than a normal feedback RTT and is evaluated using the latest RTT:
  // a real standing queue remains represented, while orphaned bytes cannot
  // pin pushback at its minimum forever after the queue has drained.
  struct InflightPacket {
    uint64_t id;
    uint32_t key;
    int64_t sent_time_ms;
    int64_t bytes;
  };
  std::deque<InflightPacket> inflight_history_;
  std::unordered_map<uint32_t, std::deque<uint64_t>> inflight_ids_by_key_;
  std::unordered_map<uint64_t, int64_t> active_inflight_bytes_;
  uint64_t next_inflight_id_ = 1;
  int64_t outstanding_bytes_ = 0;
  static constexpr int64_t kMinInflightTimeoutMs = 2000;
  static constexpr int64_t kInflightTimeoutRttMultiplier = 4;
  // Feedback-max-RTT window (ms). Each batch contributes its max sample; the
  // congestion window uses the MIN across the window (WebRTC semantics).
  std::deque<std::pair<int64_t, int64_t>> feedback_max_rtts_;  // (time_ms, rtt_ms)
  static constexpr int64_t kRttWindowMs = 1000;
  int64_t last_rtt_ms_ = 0;              // most recent batch max, for logging
  double pushback_ratio_ = 1.0;          // for logging
  static constexpr int64_t kMinCwndBytes = 2 * 1500;
  static constexpr int64_t kMaxRttWindowSamples = 100;

  // Fake clock for testing (nullptr = use real clock)
  int64_t* fake_clock_ms_;

  // Current bandwidth usage state (updated in Detect). Used by
  // GetNetworkUsageState for encoder VBV adaptation.
  BandwidthUsage last_bandwidth_usage_ = BandwidthUsage::kNormal;
};

#endif  // TRANSMISSION_GCC_CONTROLLER_H
