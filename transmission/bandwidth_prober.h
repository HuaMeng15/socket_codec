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

  /** Report that application is below estimated rate (ALR condition). */
  void OnApplicationLimited();

  /** Get current state. */
  State GetState() const;

  /** Get pending probe clusters (for pacer to send at elevated rate). */
  std::vector<ProbeCluster> GetPendingProbes();

 private:
  void MaybeInitiateProbe();
  void InitiateExponentialProbe();
  void InitiateAlrProbe();
  void InitiateDropRecoveryProbe();
  int ComputeProbeTarget() const;

  mutable std::mutex mutex_;
  State state_;

  int estimated_bitrate_kbps_;
  int max_bitrate_kbps_;

  // Initial exponential probing
  bool initial_probing_done_;
  int exponential_probe_count_;
  static constexpr int kFirstExponentialMultiplier = 3;   // First probe: 3x
  static constexpr int kSecondExponentialMultiplier = 6;  // Second probe: 6x
  static constexpr int kMaxExponentialProbes = 2;
  static constexpr double kFurtherProbeThreshold = 0.7;   // Success if est > 0.7 * target
  static constexpr int kFurtherProbeMultiplier = 2;       // Next probe: 2x successful est

  // Periodic ALR probing
  bool application_limited_;
  std::chrono::steady_clock::time_point last_alr_probe_time_;
  static constexpr int kAlrProbeIntervalMs = 5000;
  static constexpr double kAlrProbeMultiplier = 1.5;  // Probe at 1.5x current in ALR

  // Bitrate drop recovery probing
  int pre_drop_bitrate_kbps_;
  std::chrono::steady_clock::time_point bitrate_drop_time_;
  bool bitrate_drop_detected_;
  static constexpr double kBitrateDropThreshold = 0.66;
  static constexpr int kBitrateDropTimeoutMs = 5000;
  static constexpr double kProbeFractionAfterDrop = 0.85;

  // Current probe state
  int probe_target_kbps_;
  int next_cluster_id_;
  std::vector<ProbeCluster> pending_probes_;
  std::chrono::steady_clock::time_point probe_start_time_;
  static constexpr int kProbeTimeoutMs = 3000;

  // Timing
  std::chrono::steady_clock::time_point last_overuse_time_;
  static constexpr int kMinTimeBetweenProbesMs = 1000;
};

#endif  // TRANSMISSION_BANDWIDTH_PROBER_H
