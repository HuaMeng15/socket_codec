#ifndef TRANSMISSION_BANDWIDTH_PROBER_H
#define TRANSMISSION_BANDWIDTH_PROBER_H

#include <chrono>
#include <cstdint>
#include <mutex>
#include <vector>

/**
 * BandwidthProber: WebRTC-style probe controller.
 *
 * Probe triggers (referencing WebRTC probe_controller.cc):
 *
 * 1. Initial exponential probing: at startup, probes at multiples of
 *    the initial estimate (e.g., 3x, 6x) to quickly discover capacity.
 *    If a probe succeeds (estimated > further_probe_threshold * target),
 *    another probe at 2x the new estimate is initiated.
 *
 * 2. Periodic ALR probing: when the application is sending below
 *    the estimated capacity (application-limited), periodically probe
 *    to re-discover bandwidth (every kAlrProbeIntervalMs).
 *
 * 3. Probe after bitrate drop: if the estimated bitrate drops by
 *    kBitrateDropThreshold (66%) and stabilizes, probe at a fraction
 *    (kProbeFractionAfterDrop = 0.85) of the pre-drop bitrate.
 *
 * Probe bitrate targets consider current network situation:
 *   - Limited by max_bitrate (don't probe above ceiling)
 *   - Scaled by current estimate (probe at 2x current, not absolute)
 *   - Further probes only if headroom exists (current < max * 0.95)
 *
 * State: Idle → Probing → WaitingForResult → (success/failure) → Idle
 */
class BandwidthProber {
 public:
  enum class State { kIdle, kProbing, kWaitingForResult };

  // Origin of the currently active probe. Only kPeriodic/kDropRecovery probes
  // are allowed to chain (fire a further probe from their result); the kSeed
  // startup probes commit their measured rate and hand off to AIMD, matching
  // WebRTC where the initial 3x/6x cluster does not self-extend indefinitely.
  enum class ProbeType { kSeed, kPeriodic, kDropRecovery };

  struct ProbeCluster {
    int target_bitrate_kbps;
    int cluster_id;
  };

  BandwidthProber();
  ~BandwidthProber() = default;

  /** Set the current estimated bitrate (from GCC). */
  void SetEstimatedBitrate(int bitrate_kbps);

  /** Set max bitrate ceiling. Probes won't exceed this. */
  void SetMaxBitrate(int max_kbps);

  /**
   * Called on each feedback round. Returns the effective bitrate to use.
   * If probing, returns the probe target; otherwise returns current estimate.
   */
  int GetEffectiveBitrateKbps();

  /**
   * Report probe result from feedback analysis.
   * estimated_kbps: the bitrate estimated from the probe cluster feedback.
   */
  void OnProbeResult(int estimated_kbps, bool success);

  /** Report overuse signal — cancels active probe. */
  void OnOveruseDetected();

  /**
   * Report underuse signal — the delay-based estimator sees the queue
   * draining. Drop-recovery probing is only permitted shortly after underuse,
   * so we never probe back into a still-congested link.
   */
  void OnUnderuseDetected();

  /** Report that application is below estimated rate (ALR condition). */
  void OnApplicationLimited();

  /** Get current state. */
  State GetState() const;

  /** Get pending probe clusters (for pacer to send at elevated rate). */
  std::vector<ProbeCluster> GetPendingProbes();

  /** Inject fake clock for testing (nullptr = real clock). */
  void SetClockForTesting(int64_t* clock_ms);

 private:
  int64_t NowMs() const;
  void MaybeInitiateProbe();
  void InitiateExponentialProbe();
  void InitiatePeriodicProbe();
  void InitiateAlrProbe();
  void InitiateDropRecoveryProbe();
  int ComputeProbeTarget() const;
  // Cap a probe target so it can't exceed a sane multiple of the CURRENT
  // estimate. After a capacity cliff (e.g. 10→1 Mbps) the drop-recovery probe
  // would otherwise target 0.85× the stale pre-drop rate (≈9 Mbps) and flood
  // the new 1 Mbps link. Bounding by current estimate keeps every probe
  // proportional to what the link is actually carrying now.
  int CapProbeTarget(int target_kbps) const;
  static constexpr int kMaxProbeMultipleOfEstimate = 3;

  mutable std::mutex mutex_;
  State state_;

  int estimated_bitrate_kbps_;
  int max_bitrate_kbps_;

  // Initial exponential probing. The 3x and 6x seeds are BOTH taken off the
  // estimate captured when the first seed fires (seed_base_kbps_), so they land
  // at 3x/6x of the startup rate as a cluster — not 6x of the already-committed
  // 3x result (which would compound to near-cap in two jumps).
  bool initial_probing_done_;
  int exponential_probe_count_;
  int seed_base_kbps_;  // estimate snapshot when the seed cluster began
  static constexpr int kFirstExponentialMultiplier = 3;   // First seed: 3x
  static constexpr int kSecondExponentialMultiplier = 6;  // Second seed: 6x
  static constexpr int kMaxExponentialProbes = 2;
  static constexpr double kFurtherProbeThreshold = 0.7;   // Success if est > 0.7 * target
  static constexpr int kFurtherProbeMultiplier = 2;       // Next probe: 2x successful est

  // Chain control: which probe is active, and how many further hops it has
  // taken. A periodic/recovery probe may chain at most kMaxChainHops times so a
  // single probe session cannot run all the way to 2x the link capacity (which
  // is what flooded the pipe and forced the overuse→collapse cycle).
  ProbeType current_probe_type_;
  int chain_hops_;
  static constexpr int kMaxChainHops = 2;

  // Periodic re-probing. After initial probing, re-probe every
  // kPeriodicProbeIntervalMs at kPeriodicProbeMultiplier x the current estimate
  // to search for freed-up headroom. Decoupled from the ALR/application-limited
  // flag: a greedy encoder (mock, or CBR) is never application-limited, so the
  // WebRTC ALR trigger would never fire and convergence would stall on AIMD.
  int64_t last_periodic_probe_ms_;
  static constexpr int kPeriodicProbeIntervalMs = 5000;
  static constexpr double kPeriodicProbeMultiplier = 2.0;
  // Don't bother re-probing once we're already near the ceiling.
  static constexpr double kPeriodicProbeMaxEstimateFraction = 0.95;

  // ALR periodic probing (kept for real application-limited encoders).
  bool application_limited_;
  int64_t last_alr_probe_ms_;
  static constexpr int kAlrProbeIntervalMs = 5000;
  static constexpr double kAlrProbeMultiplier = 1.5;

  // Bitrate drop recovery probing
  int pre_drop_bitrate_kbps_;
  int64_t bitrate_drop_time_ms_;
  bool bitrate_drop_detected_;
  static constexpr double kBitrateDropThreshold = 0.66;
  static constexpr int kBitrateDropTimeoutMs = 5000;
  static constexpr double kProbeFractionAfterDrop = 0.85;
  // Drop-recovery probing is only allowed within this window after an underuse
  // signal (queue draining). Mirrors WebRTC's in_alr / alr_ended_recently gate
  // in ProbeController::RequestProbe — probe only when the link is no longer
  // congested, never straight back into a full queue.
  int64_t last_underuse_time_ms_;
  static constexpr int kUnderuseRecencyMs = 3000;

  // Current probe state
  int probe_target_kbps_;
  int next_cluster_id_;
  std::vector<ProbeCluster> pending_probes_;
  int64_t probe_start_ms_;
  static constexpr int kProbeTimeoutMs = 3000;

  // Timing
  int64_t last_overuse_time_ms_;
  static constexpr int kMinTimeBetweenProbesMs = 1000;

  // Fake clock for testing
  int64_t* fake_clock_ms_;
};

#endif  // TRANSMISSION_BANDWIDTH_PROBER_H
