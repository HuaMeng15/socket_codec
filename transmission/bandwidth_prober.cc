#include "bandwidth_prober.h"

#include "log_system/log_system.h"

BandwidthProber::BandwidthProber()
    : state_(State::kIdle),
      current_bitrate_kbps_(1000),
      probe_bitrate_kbps_(0),
      pre_probe_bitrate_kbps_(0),
      probe_multiplier_(1.5),
      stable_time_before_probe_ms_(3000),
      probe_duration_ms_(500),
      eval_duration_ms_(500),
      stable_signals_in_eval_(0),
      overuse_signals_in_eval_(0) {
  auto now = std::chrono::steady_clock::now();
  last_overuse_time_ = now;
  probe_start_time_ = now;
  eval_start_time_ = now;
}

void BandwidthProber::SetCurrentBitrate(int bitrate_kbps) {
  std::lock_guard<std::mutex> lock(mutex_);
  current_bitrate_kbps_ = bitrate_kbps;
}

int BandwidthProber::GetEffectiveBitrateKbps() {
  std::lock_guard<std::mutex> lock(mutex_);

  auto now = std::chrono::steady_clock::now();

  switch (state_) {
    case State::kIdle:
      TryStartProbe();
      if (state_ == State::kProbing) {
        return probe_bitrate_kbps_;
      }
      return current_bitrate_kbps_;

    case State::kProbing: {
      auto since_start = std::chrono::duration_cast<std::chrono::milliseconds>(
          now - probe_start_time_).count();
      if (since_start >= probe_duration_ms_) {
        // Transition to evaluating
        state_ = State::kEvaluating;
        eval_start_time_ = now;
        stable_signals_in_eval_ = 0;
        overuse_signals_in_eval_ = 0;
        LOG(INFO) << "[Prober] Probing complete, evaluating at "
                  << probe_bitrate_kbps_ << " kbps";
      }
      return probe_bitrate_kbps_;
    }

    case State::kEvaluating: {
      Evaluate();
      return probe_bitrate_kbps_;
    }

    case State::kCommitted:
      // Committed: adopt probe rate as new current
      current_bitrate_kbps_ = probe_bitrate_kbps_;
      state_ = State::kIdle;
      LOG(INFO) << "[Prober] Committed to " << current_bitrate_kbps_ << " kbps";
      return current_bitrate_kbps_;

    case State::kAborted:
      // Aborted: revert to pre-probe rate
      current_bitrate_kbps_ = pre_probe_bitrate_kbps_;
      state_ = State::kIdle;
      LOG(INFO) << "[Prober] Aborted, reverting to " << current_bitrate_kbps_ << " kbps";
      return current_bitrate_kbps_;
  }

  return current_bitrate_kbps_;
}

void BandwidthProber::OnOveruseDetected() {
  std::lock_guard<std::mutex> lock(mutex_);
  last_overuse_time_ = std::chrono::steady_clock::now();

  if (state_ == State::kProbing || state_ == State::kEvaluating) {
    overuse_signals_in_eval_++;
    if (overuse_signals_in_eval_ >= 2) {
      state_ = State::kAborted;
      LOG(INFO) << "[Prober] Overuse during probe, aborting";
    }
  }
}

void BandwidthProber::OnStableSignal() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (state_ == State::kEvaluating) {
    stable_signals_in_eval_++;
  }
}

BandwidthProber::State BandwidthProber::GetState() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return state_;
}

void BandwidthProber::SetProbeMultiplier(double multiplier) {
  probe_multiplier_ = multiplier > 1.0 ? multiplier : 1.5;
}

void BandwidthProber::SetStableTimeBeforeProbeMs(int ms) {
  stable_time_before_probe_ms_ = ms > 0 ? ms : 3000;
}

void BandwidthProber::SetProbeDurationMs(int ms) {
  probe_duration_ms_ = ms > 0 ? ms : 500;
}

void BandwidthProber::SetEvalDurationMs(int ms) {
  eval_duration_ms_ = ms > 0 ? ms : 500;
}

void BandwidthProber::TryStartProbe() {
  auto now = std::chrono::steady_clock::now();
  auto since_overuse = std::chrono::duration_cast<std::chrono::milliseconds>(
      now - last_overuse_time_).count();

  if (since_overuse >= stable_time_before_probe_ms_) {
    // Start probing
    pre_probe_bitrate_kbps_ = current_bitrate_kbps_;
    probe_bitrate_kbps_ = static_cast<int>(current_bitrate_kbps_ * probe_multiplier_);
    probe_start_time_ = now;
    state_ = State::kProbing;
    stable_signals_in_eval_ = 0;
    overuse_signals_in_eval_ = 0;
    LOG(INFO) << "[Prober] Starting probe: " << current_bitrate_kbps_
              << " → " << probe_bitrate_kbps_ << " kbps";
  }
}

void BandwidthProber::Evaluate() {
  auto now = std::chrono::steady_clock::now();
  auto since_eval = std::chrono::duration_cast<std::chrono::milliseconds>(
      now - eval_start_time_).count();

  if (since_eval >= eval_duration_ms_) {
    // Evaluation period over — decide
    if (overuse_signals_in_eval_ >= 2) {
      state_ = State::kAborted;
    } else {
      state_ = State::kCommitted;
    }
  }
}
