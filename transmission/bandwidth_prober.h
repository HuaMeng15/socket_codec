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
 * 3. Optional congestion-aware periodic probing: at a caller-configured
 *    interval, probe even outside ALR, but only when GCC's delay, loss, byte
 *    delivery, and congestion-window signals all report a healthy network.
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
 *   - Periodic ALR discovery is bounded to one conservative 1.25× step over
 *     the current measured estimate; startup probing keeps its original policy
 *   - Further probes only if headroom exists (current < max * 0.95)
 *
 * State: Idle → Probing → WaitingForResult → (success/failure) → Idle
 */
class BandwidthProber {
 public:
  enum class State { kIdle, kProbing, kWaitingForResult };

  // Origin of the currently active probe.
  //   kSeed    — the initial 3x/6x exponential cluster; commits and does not chain.
  //   kStartup — startup-stage accelerator probe (1.5x, one per ~1s settle) that
  //              keeps re-probing until overuse or a weak result, so a fast idle
  //              link (e.g. 10 Mbps) converges in a few seconds instead of the
  //              ~16s AIMD crawl. Does not chain; the cadence is driven by the
  //              settle timer in MaybeInitiateProbe.
  //   kAlr      — conservative, one-step ALR discovery probe.
  //   kPeriodic — conservative, one-step scheduled healthy-network probe.
  enum class ProbeType { kSeed, kStartup, kAlr, kPeriodic };

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

  /**
   * Report the current ALR state. This is deliberately a level-triggered
   * signal, not a latched event: a short VBR lull must not arm a probe that
   * fires much later after the source has resumed filling the estimate.
   */
  void SetApplicationLimited(bool limited);

  /** Enable or disable the ordinary WebRTC-style periodic ALR probes. */
  void SetAlrProbingEnabled(bool enabled);

  /**
   * Configure an optional non-ALR periodic probe interval. A value <= 0
   * disables it. The network-health gate still applies to every opportunity.
   */
  void SetUnconditionalPeriodicProbeIntervalMs(int interval_ms);

  /**
   * Allow periodic ALR discovery only while the controller's measured network
   * state is healthy (no congestion-window pushback or growing queue). This
   * gate deliberately does not affect the startup probe policy.
   */
  void SetPeriodicProbingAllowed(bool allowed);

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
  void InitiateStartupProbe();
  void InitiateAlrProbe();
  void InitiatePeriodicProbe();
  // Cap a probe target to a conservative increase over the current measured
  // capacity line. Startup probing has its own unchanged policy; this cap is
  // used only by periodic ALR discovery.
  int CapProbeTarget(int target_kbps) const;
  static constexpr double kMaxPeriodicProbeIncreaseLimit = 1.25;

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

  // Startup stage. After the seed cluster, keep firing 1.5x probes (one per
  // ~kStartupSettleMs, letting the new rate settle in between) so an idle
  // high-capacity link converges in a few seconds rather than the slow AIMD
  // crawl. The stage ends the first time a probe comes back weak (< 70% of
  // target => link can't sustain it) or byte/cwnd state confirms congestion —
  // after that, normal AIMD/ALR takes over.
  bool startup_stage_;
  int64_t last_startup_probe_ms_;
  static constexpr int kStartupSettleMs = 1000;
  static constexpr double kStartupProbeMultiplier = 1.5;
  // Startup's next probe still explores 1.5× the measured rate. The gentler
  // periodic-ALR limit below does not alter startup behavior.

  // Origin of the active probe. Periodic probes deliberately do not chain:
  // capacity discovery progresses one measured step per ALR interval instead
  // of compounding several speculative increases in a few milliseconds.
  ProbeType current_probe_type_;

  // ALR periodic probing. Fires only while application-limited (the encoder is
  // sending below the estimate), driven by the real AlrDetector. This is the
  // WebRTC-style source-limited probe mechanism: a greedy encoder that fills
  // the pipe is never in ALR, while a variable/VBR encoder goes app-limited on
  // simple content and gets a probe to keep the estimate warm. The optional
  // scheduled mechanism below is separate and explicitly configured.
  bool application_limited_;
  bool alr_probing_enabled_;
  bool periodic_probing_allowed_;
  int64_t application_limited_since_ms_;
  int64_t last_alr_probe_ms_;
  static constexpr int kAlrProbeIntervalMs = 5000;
  // Require ALR to persist before probing. This rejects transient VBR gaps and
  // makes both frame- and slice-producing encoders qualify by the same
  // sustained under-production signal.
  static constexpr int kAlrQualificationMs = 500;
  static constexpr double kAlrProbeMultiplier = 1.25;

  // Optional non-ALR periodic probing. The schedule is based on opportunities,
  // not successful probes: an unhealthy opportunity is skipped and the next
  // check occurs after another full interval.
  int unconditional_periodic_probe_interval_ms_;
  int64_t last_unconditional_periodic_opportunity_ms_;

  // Current probe state
  int probe_target_kbps_;
  int next_cluster_id_;
  std::vector<ProbeCluster> pending_probes_;
  int64_t probe_start_ms_;
  static constexpr int kProbeTimeoutMs = 3000;

  // Timing
  int64_t last_overuse_time_ms_;
  int64_t last_probe_time_ms_;
  static constexpr int kMinTimeBetweenProbesMs = 1000;

  // Fake clock for testing
  int64_t* fake_clock_ms_;
};

#endif  // TRANSMISSION_BANDWIDTH_PROBER_H
