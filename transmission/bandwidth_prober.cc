#include "bandwidth_prober.h"

#include <algorithm>
#include <chrono>

#include "log_system/log_system.h"

int64_t BandwidthProber::NowMs() const {
  if (fake_clock_ms_) {
    return *fake_clock_ms_;
  }
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}

BandwidthProber::BandwidthProber()
    : state_(State::kIdle),
      estimated_bitrate_kbps_(1000),
      max_bitrate_kbps_(30000),
      initial_probing_done_(false),
      exponential_probe_count_(0),
      application_limited_(false),
      last_alr_probe_ms_(0),
      pre_drop_bitrate_kbps_(0),
      bitrate_drop_time_ms_(0),
      bitrate_drop_detected_(false),
      probe_target_kbps_(0),
      next_cluster_id_(0),
      probe_start_ms_(0),
      last_overuse_time_ms_(0),
      fake_clock_ms_(nullptr) {
  int64_t now = NowMs();
  last_alr_probe_ms_ = now;
  bitrate_drop_time_ms_ = now;
  probe_start_ms_ = now;
  last_overuse_time_ms_ = now;
}

void BandwidthProber::SetClockForTesting(int64_t* clock_ms) {
  fake_clock_ms_ = clock_ms;
  if (clock_ms) {
    int64_t now = *clock_ms;
    last_alr_probe_ms_ = now;
    bitrate_drop_time_ms_ = now;
    probe_start_ms_ = now;
    last_overuse_time_ms_ = now - 2000;  // allow probing immediately in tests
  }
}

void BandwidthProber::SetEstimatedBitrate(int bitrate_kbps) {
  std::lock_guard<std::mutex> lock(mutex_);

  // Detect significant bitrate drop
  if (!bitrate_drop_detected_ && pre_drop_bitrate_kbps_ > 0) {
    double ratio = static_cast<double>(bitrate_kbps) / pre_drop_bitrate_kbps_;
    if (ratio < kBitrateDropThreshold) {
      bitrate_drop_detected_ = true;
      bitrate_drop_time_ms_ = NowMs();
      LOG(INFO) << "[Prober] Bitrate drop detected: " << pre_drop_bitrate_kbps_
                << " -> " << bitrate_kbps << " kbps";
    }
  }

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
  int64_t now = NowMs();

  if (state_ == State::kIdle) {
    MaybeInitiateProbe();
    if (state_ == State::kProbing) {
      return probe_target_kbps_;
    }
    return estimated_bitrate_kbps_;
  }

  // Check probe timeout
  if (state_ == State::kWaitingForResult) {
    if (now - probe_start_ms_ > kProbeTimeoutMs) {
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

    double ratio = static_cast<double>(estimated_kbps) / probe_target_kbps_;
    if (ratio >= kFurtherProbeThreshold &&
        !initial_probing_done_ &&
        estimated_kbps < max_bitrate_kbps_ * 0.95) {
      probe_target_kbps_ = std::min(
          estimated_kbps * kFurtherProbeMultiplier, max_bitrate_kbps_);
      state_ = State::kProbing;
      probe_start_ms_ = NowMs();
      pending_probes_.push_back({probe_target_kbps_, next_cluster_id_++});
      LOG(INFO) << "[Prober] Further probe at " << probe_target_kbps_ << " kbps";
    }

    bitrate_drop_detected_ = false;
    pre_drop_bitrate_kbps_ = estimated_kbps;
  } else {
    LOG(INFO) << "[Prober] Probe failed or no improvement: target="
              << probe_target_kbps_ << " estimated=" << estimated_kbps;
    initial_probing_done_ = true;
  }
}

void BandwidthProber::OnOveruseDetected() {
  std::lock_guard<std::mutex> lock(mutex_);
  last_overuse_time_ms_ = NowMs();
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

  if (state_ == State::kProbing) {
    state_ = State::kWaitingForResult;
  }

  return probes;
}

void BandwidthProber::MaybeInitiateProbe() {
  int64_t now = NowMs();

  if (now - last_overuse_time_ms_ < kMinTimeBetweenProbesMs) {
    return;
  }

  // 1. Initial exponential probing
  if (!initial_probing_done_) {
    InitiateExponentialProbe();
    return;
  }

  // 2. Bitrate drop recovery
  if (bitrate_drop_detected_) {
    if (now - bitrate_drop_time_ms_ < kBitrateDropTimeoutMs) {
      InitiateDropRecoveryProbe();
      return;
    } else {
      bitrate_drop_detected_ = false;
    }
  }

  // 3. ALR periodic probing
  if (application_limited_) {
    if (now - last_alr_probe_ms_ >= kAlrProbeIntervalMs) {
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

  if (estimated_bitrate_kbps_ >= max_bitrate_kbps_ * 0.95) {
    initial_probing_done_ = true;
    return;
  }

  exponential_probe_count_++;
  if (exponential_probe_count_ >= kMaxExponentialProbes) {
    initial_probing_done_ = true;
  }

  state_ = State::kProbing;
  probe_start_ms_ = NowMs();
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
    return;
  }

  state_ = State::kProbing;
  probe_start_ms_ = NowMs();
  last_alr_probe_ms_ = probe_start_ms_;
  pending_probes_.push_back({probe_target_kbps_, next_cluster_id_++});
  application_limited_ = false;

  LOG(INFO) << "[Prober] ALR probe at " << probe_target_kbps_
            << " kbps (est=" << estimated_bitrate_kbps_ << ")";
}

void BandwidthProber::InitiateDropRecoveryProbe() {
  probe_target_kbps_ = std::min(
      static_cast<int>(pre_drop_bitrate_kbps_ * kProbeFractionAfterDrop),
      max_bitrate_kbps_);

  if (probe_target_kbps_ <= estimated_bitrate_kbps_) {
    bitrate_drop_detected_ = false;
    return;
  }

  state_ = State::kProbing;
  probe_start_ms_ = NowMs();
  pending_probes_.push_back({probe_target_kbps_, next_cluster_id_++});
  bitrate_drop_detected_ = false;

  LOG(INFO) << "[Prober] Drop recovery probe at " << probe_target_kbps_
            << " kbps (pre-drop=" << pre_drop_bitrate_kbps_
            << ", current=" << estimated_bitrate_kbps_ << ")";
}
