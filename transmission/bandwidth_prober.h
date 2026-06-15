#ifndef TRANSMISSION_BANDWIDTH_PROBER_H
#define TRANSMISSION_BANDWIDTH_PROBER_H

#include <chrono>
#include <cstdint>
#include <mutex>

/**
 * BandwidthProber: state machine that periodically probes for available
 * headroom above the current estimated bitrate.
 *
 * States:
 *   Idle → Probing → Evaluating → Committed | Aborted → Idle
 *
 * During Probing: send rate is temporarily increased to probe_rate (1.5x).
 * During Evaluating: check feedback for overuse signals.
 * If overuse: Aborted (revert to previous rate).
 * If stable: Committed (keep higher rate).
 */
class BandwidthProber {
 public:
  enum class State { kIdle, kProbing, kEvaluating, kCommitted, kAborted };

  BandwidthProber();
  ~BandwidthProber() = default;

  /** Set the current estimated bitrate. Called by GCC on each update. */
  void SetCurrentBitrate(int bitrate_kbps);

  /**
   * Called periodically (e.g., on each feedback). Advances the state machine.
   * Returns the effective send bitrate: either current (idle) or probe rate.
   */
  int GetEffectiveBitrateKbps();

  /** Report overuse signal from GCC. If probing, triggers abort. */
  void OnOveruseDetected();

  /** Report stable/underuse from GCC feedback. Used during evaluation. */
  void OnStableSignal();

  /** Get current probe state. */
  State GetState() const;

  /** Configure probe parameters. */
  void SetProbeMultiplier(double multiplier);
  void SetStableTimeBeforeProbeMs(int ms);
  void SetProbeDurationMs(int ms);
  void SetEvalDurationMs(int ms);

 private:
  void TryStartProbe();
  void Evaluate();

  mutable std::mutex mutex_;
  State state_;
  int current_bitrate_kbps_;
  int probe_bitrate_kbps_;
  int pre_probe_bitrate_kbps_;

  // Timing
  std::chrono::steady_clock::time_point last_overuse_time_;
  std::chrono::steady_clock::time_point probe_start_time_;
  std::chrono::steady_clock::time_point eval_start_time_;

  // Parameters
  double probe_multiplier_;
  int stable_time_before_probe_ms_;
  int probe_duration_ms_;
  int eval_duration_ms_;
  int stable_signals_in_eval_;
  int overuse_signals_in_eval_;
};

#endif  // TRANSMISSION_BANDWIDTH_PROBER_H
