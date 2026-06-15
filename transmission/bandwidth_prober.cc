#include "bandwidth_prober.h"

#include <algorithm>

#include "log_system/log_system.h"

BandwidthProber::BandwidthProber()
    : state_(State::kIdle),
      estimated_bitrate_kbps_(1000),
      max_bitrate_kbps_(30000),
      initial_probing_done_(false),
      exponential_probe_count_(0),
      application_limited_(false),
      pre_drop_bitrate_kbps_(0),
      bitrate_drop_detected_(false),
      probe_target_kbps_(0),
      next_cluster_id_(0) {
  auto now = std::chrono::steady_clock::now();
  last_alr_probe_time_ = now;
  bitrate_drop_time_ = now;
  probe_start_time_ = now;
  last_overuse_time_ = now;
}

void BandwidthProber::SetEstimatedBitrate(int bitrate_kbps) {
  std::lock_guard<std::mutex> lock(mutex_);

  // Detect significant bitrate drop
  if (!bitrate_drop_detected_ && pre_drop_bitrate_kbps_ > 0) {
    double ratio = static_cast<double>(bitrate_kbps) / pre_drop_bitrate_kbps_;
    if (ratio < kBitrateDropThreshold) {
      bitrate_drop_detected_ = true;
      bitrate_drop_time_ = std::chrono::steady_clock::now();
      LOG(INFO) << "[Prober] Bitrate drop detected: " << pre_drop_bitrate_kbps_
                << " → " << bitrate_kbps << " kbps";
    }
  }

  // Track the highest stable bitrate as pre-drop reference
  if (bitrate_kbps > pre_drop_bitrate_kbps_ && !bitrate_drop_detected_) {
    pre_drop_bitrate_kbps_ = bitrate_kbps;
  }

  estimated_bitrate_kbps_ = bitrate_kbps;
}

void BandwidthProber::SetMaxBitrate(int max_kbps) {
  std::lock_guard<std::mutex> lock(mutex_);
  max_bitrate_kbps_ = max_kbps;
}

int BandwidthProber::GetEffectiveBitrateKbps() {
  std::lock_guard<std::mutex> lock(mutex_);

  if (state_ == State::kIdle) {
    MaybeInitiateProbe();
  }

  // Check probe timeout
  if (state_ == State::kWaitingForResult) {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - probe_start_time_).count();
    if (elapsed > kProbeTimeoutMs) {
      LOG(INFO) << "[Prober] Probe timed out";
      state_ = State::kIdle;
      return estimated_bitrate_kbps_;
    }
  }

  if (state_ == State::kProbing || state_ == State::kWaitingForResult) {
    return probe_target_kbps_;
  }

  return estimated_bitrate_kbps_;
}

void BandwidthProber::OnProbeResult(int estimated_kbps, bool success) {
  std::lock_guard<std::mutex> lock(mutex_);

  if (state_ != State::kWaitingForResult) {
    return;
  }

  state_ = State::kIdle;

  if (success && estimated_kbps > estimated_bitrate_kbps_) {
    LOG(INFO) << "[Prober] Probe succeeded: target=" << probe_target_kbps_
              << " estimated=" << estimated_kbps << " kbps";

    // Check if further probing is warranted (WebRTC: further_probe_threshold)
    double ratio = static_cast<double>(estimated_kbps) / probe_target_kbps_;
    if (ratio >= kFurtherProbeThreshold &&
        !initial_probing_done_ &&
        estimated_kbps < max_bitrate_kbps_ * 0.95) {
      // Initiate further probe at 2x the successful estimate
      probe_target_kbps_ = std::min(
          estimated_kbps * kFurtherProbeMultiplier, max_bitrate_kbps_);
      state_ = State::kProbing;
      probe_start_time_ = std::chrono::steady_clock::now();
      pending_probes_.push_back({probe_target_kbps_, next_cluster_id_++});
      LOG(INFO) << "[Prober] Further probe at " << probe_target_kbps_ << " kbps";
    }

    // Reset drop state on successful probe
    bitrate_drop_detected_ = false;
    pre_drop_bitrate_kbps_ = estimated_kbps;
  } else {
    LOG(INFO) << "[Prober] Probe failed or no improvement: target="
              << probe_target_kbps_ << " estimated=" << estimated_kbps;
    // Done with initial probing if a probe fails
    initial_probing_done_ = true;
  }
}

void BandwidthProber::OnOveruseDetected() {
  std::lock_guard<std::mutex> lock(mutex_);
  last_overuse_time_ = std::chrono::steady_clock::now();

  // Overuse means network can't handle current rate — cancel any probe
  // and mark initial probing done
  initial_probing_done_ = true;

  if (state_ != State::kIdle) {
    LOG(INFO) << "[Prober] Overuse during probe, cancelling";
    state_ = State::kIdle;
    pending_probes_.clear();
  }
}

void BandwidthProber::OnApplicationLimited() {
  std::lock_guard<std::mutex> lock(mutex_);
  application_limited_ = true;
}

BandwidthProber::State BandwidthProber::GetState() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return state_;
}

std::vector<BandwidthProber::ProbeCluster> BandwidthProber::GetPendingProbes() {
  std::lock_guard<std::mutex> lock(mutex_);
  auto probes = std::move(pending_probes_);
  pending_probes_.clear();

  // Transition from probing to waiting for result
  if (state_ == State::kProbing && probes.empty()) {
    state_ = State::kWaitingForResult;
  } else if (state_ == State::kProbing) {
    state_ = State::kWaitingForResult;
  }

  return probes;
}

void BandwidthProber::MaybeInitiateProbe() {
  auto now = std::chrono::steady_clock::now();

  // Respect minimum time between probes
  auto since_overuse = std::chrono::duration_cast<std::chrono::milliseconds>(
      now - last_overuse_time_).count();
  if (since_overuse < kMinTimeBetweenProbesMs) {
    return;
  }

  // 1. Initial exponential probing (startup)
  if (!initial_probing_done_) {
    InitiateExponentialProbe();
    return;
  }

  // 2. Bitrate drop recovery probe
  if (bitrate_drop_detected_) {
    auto since_drop = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - bitrate_drop_time_).count();
    if (since_drop < kBitrateDropTimeoutMs) {
      InitiateDropRecoveryProbe();
      return;
    } else {
      // Timeout — give up on recovery
      bitrate_drop_detected_ = false;
    }
  }

  // 3. ALR periodic probing
  if (application_limited_) {
    auto since_last_alr = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - last_alr_probe_time_).count();
    if (since_last_alr >= kAlrProbeIntervalMs) {
      InitiateAlrProbe();
      return;
    }
  }
}

void BandwidthProber::InitiateExponentialProbe() {
  int multiplier = (exponential_probe_count_ == 0)
      ? kFirstExponentialMultiplier
      : kSecondExponentialMultiplier;

  probe_target_kbps_ = std::min(
      estimated_bitrate_kbps_ * multiplier, max_bitrate_kbps_);

  // Don't probe if we're already near max
  if (estimated_bitrate_kbps_ >= max_bitrate_kbps_ * 0.95) {
    initial_probing_done_ = true;
    return;
  }

  exponential_probe_count_++;
  if (exponential_probe_count_ >= kMaxExponentialProbes) {
    initial_probing_done_ = true;
  }

  state_ = State::kProbing;
  probe_start_time_ = std::chrono::steady_clock::now();
  pending_probes_.push_back({probe_target_kbps_, next_cluster_id_++});

  LOG(INFO) << "[Prober] Exponential probe #" << exponential_probe_count_
            << " at " << probe_target_kbps_ << " kbps (est="
            << estimated_bitrate_kbps_ << ")";
}

void BandwidthProber::InitiateAlrProbe() {
  probe_target_kbps_ = std::min(
      static_cast<int>(estimated_bitrate_kbps_ * kAlrProbeMultiplier),
      max_bitrate_kbps_);

  if (probe_target_kbps_ <= estimated_bitrate_kbps_) {
    return;  // No headroom to probe
  }

  state_ = State::kProbing;
  probe_start_time_ = std::chrono::steady_clock::now();
  last_alr_probe_time_ = probe_start_time_;
  pending_probes_.push_back({probe_target_kbps_, next_cluster_id_++});
  application_limited_ = false;

  LOG(INFO) << "[Prober] ALR probe at " << probe_target_kbps_
            << " kbps (est=" << estimated_bitrate_kbps_ << ")";
}

void BandwidthProber::InitiateDropRecoveryProbe() {
  // Probe at kProbeFractionAfterDrop of the pre-drop bitrate
  probe_target_kbps_ = std::min(
      static_cast<int>(pre_drop_bitrate_kbps_ * kProbeFractionAfterDrop),
      max_bitrate_kbps_);

  if (probe_target_kbps_ <= estimated_bitrate_kbps_) {
    bitrate_drop_detected_ = false;
    return;  // Already recovered
  }

  state_ = State::kProbing;
  probe_start_time_ = std::chrono::steady_clock::now();
  pending_probes_.push_back({probe_target_kbps_, next_cluster_id_++});
  bitrate_drop_detected_ = false;

  LOG(INFO) << "[Prober] Drop recovery probe at " << probe_target_kbps_
            << " kbps (pre-drop=" << pre_drop_bitrate_kbps_
            << ", current=" << estimated_bitrate_kbps_ << ")";
}
