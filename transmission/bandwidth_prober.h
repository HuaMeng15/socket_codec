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
 * There is deliberately NO probe-on-bitrate-drop path. WebRTC's only
 * drop-recovery probe (ProbeController::RequestProbe) is gated on being in ALR
 * (in_alr || alr_ended_recently) — a drop while the pipe was full is treated as
 * real congestion and recovered by AIMD alone, never a probe. Our ALR probe (#2)
 * already covers the app-limited case, so a separate drop path would only
 * re-probe into a link that AIMD is already backing off from — the exact
 * self-induced overshoot→"drop"→re-probe loop we removed.
 *
 * Probe bitrate targets consider current network situation:
 *   - Limited by max_bitrate (don't probe above ceiling)
 *   - Bounded by kMaxProbeIncreaseLimit × current estimate (WebRTC AimdRateControl
 *     uses 1.5×throughput as its increase limit; we cap probe targets the same
 *     way so a probe can't shoot far past what the link is actually carrying)
 *   - Further probes only if headroom exists (current < max * 0.95)
 *
 * State: Idle → Probing → WaitingForResult → (success/failure) → Idle
 */
class BandwidthProber {
 public:
  enum class State { kIdle, kProbing, kWaitingForResult };

  // Origin of the currently active probe. Only kPeriodic (ALR) probes are
  // allowed to chain (fire a further probe from their result); the kSeed
  // startup probes commit their measured rate and hand off to AIMD, matching
  // WebRTC where the initial 3x/6x cluster does not self-extend indefinitely.
  enum class ProbeType { kSeed, kPeriodic };

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
   * Report underuse signal — the delay-based estimator sees the queue draining.
   * Retained for interface compatibility with GccController; recovery is now
   * pure AIMD (WebRTC-faithful), so this no longer arms any probe.
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
  void InitiateAlrProbe();
  // Cap a probe target to WebRTC's AimdRateControl increase limit:
  // 1.5 × current estimate. A probe (ALR seed or a further/chained hop) can
  // explore up to 50% above what the link is currently carrying, but no more —
  // so it can't shoot to ~2× capacity, flood the bottleneck, and trigger the
  // AIMD backoff that used to be misread as a fresh "bitrate drop".
  int CapProbeTarget(int target_kbps) const;
  static constexpr double kMaxProbeIncreaseLimit = 1.5;

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
  // Next probe explores 1.5× the measured rate (see kMaxProbeIncreaseLimit /
  // CapProbeTarget) — WebRTC's AimdRateControl increase limit, not a raw 2×.

  // Chain control: which probe is active, and how many further hops it has
  // taken. A periodic/recovery probe may chain at most kMaxChainHops times so a
  // single probe session cannot run all the way to 2x the link capacity (which
  // is what flooded the pipe and forced the overuse→collapse cycle).
  ProbeType current_probe_type_;
  int chain_hops_;
  static constexpr int kMaxChainHops = 2;

  // ALR periodic probing. Fires only while application-limited (the encoder is
  // sending below the estimate), driven by the real AlrDetector. This is the
  // sole periodic probe mechanism — WebRTC-faithful: a greedy encoder that
  // fills the pipe is never in ALR, so no periodic probe fires and AIMD alone
  // tracks capacity; a variable/VBR encoder goes app-limited on simple content
  // and gets a probe to keep the estimate warm for the next scene change.
  bool application_limited_;
  int64_t last_alr_probe_ms_;
  static constexpr int kAlrProbeIntervalMs = 5000;
  static constexpr double kAlrProbeMultiplier = 1.5;

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
