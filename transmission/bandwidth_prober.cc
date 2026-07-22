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
      seed_base_kbps_(0),
      current_probe_type_(ProbeType::kSeed),
      chain_hops_(0),
      last_periodic_probe_ms_(0),
      application_limited_(false),
      last_alr_probe_ms_(0),
      pre_drop_bitrate_kbps_(0),
      bitrate_drop_time_ms_(0),
      bitrate_drop_detected_(false),
      probe_target_kbps_(0),
      next_cluster_id_(0),
      probe_start_ms_(0),
      last_overuse_time_ms_(0),
      last_underuse_time_ms_(0),
      fake_clock_ms_(nullptr) {
  int64_t now = NowMs();
  last_alr_probe_ms_ = now;
  last_periodic_probe_ms_ = now;
  bitrate_drop_time_ms_ = now;
  probe_start_ms_ = now;
  last_overuse_time_ms_ = now;
  // Far enough in the past that drop-recovery is gated off until a real
  // underuse signal arrives.
  last_underuse_time_ms_ = now - kUnderuseRecencyMs - 1;
}

void BandwidthProber::SetClockForTesting(int64_t* clock_ms) {
  fake_clock_ms_ = clock_ms;
  if (clock_ms) {
    int64_t now = *clock_ms;
    last_alr_probe_ms_ = now;
    last_periodic_probe_ms_ = now;
    bitrate_drop_time_ms_ = now;
    probe_start_ms_ = now;
    last_overuse_time_ms_ = now - 2000;  // allow probing immediately in tests
    last_underuse_time_ms_ = now - kUnderuseRecencyMs - 1;  // gated off by default
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

  // Accept results while actively probing OR awaiting the cluster result. The
  // pacer-driven kWaitingForResult transition is optional; controllers that
  // resolve probes directly from feedback call this straight from kProbing.
  if (state_ != State::kProbing && state_ != State::kWaitingForResult) {
    return;
  }

  state_ = State::kIdle;

  if (success && estimated_kbps > estimated_bitrate_kbps_) {
    LOG(INFO) << "[Prober] Probe succeeded: target=" << probe_target_kbps_
              << " estimated=" << estimated_kbps << " kbps";

    // Chaining rule (aligned with the WebRTC reference curve):
    //   - Seed probes (3x/6x startup cluster) do NOT chain. They commit their
    //     measured rate and hand control to AIMD, which is why the reference
    //     jumps once to ~4 Mbps at startup and then climbs slowly — rather than
    //     chaining straight to the ceiling and flooding the link.
    //   - Periodic / drop-recovery probes MAY chain, but only up to
    //     kMaxChainHops hops. Bounding the hop count stops a single probe
    //     session from doubling all the way to 2x the link capacity (the
    //     runaway that caused the overuse->collapse->slow-recovery cycle).
    double ratio = static_cast<double>(estimated_kbps) / probe_target_kbps_;
    bool chainable = (current_probe_type_ != ProbeType::kSeed);
    if (chainable && chain_hops_ < kMaxChainHops &&
        ratio >= kFurtherProbeThreshold &&
        estimated_kbps < max_bitrate_kbps_ * 0.95) {
      chain_hops_++;
      probe_target_kbps_ = std::min(
          estimated_kbps * kFurtherProbeMultiplier, max_bitrate_kbps_);
      state_ = State::kProbing;
      probe_start_ms_ = NowMs();
      pending_probes_.push_back({probe_target_kbps_, next_cluster_id_++});
      LOG(INFO) << "[Prober] Further probe (" << chain_hops_ << "/"
                << kMaxChainHops << ") at " << probe_target_kbps_ << " kbps";
    }

    bitrate_drop_detected_ = false;
    pre_drop_bitrate_kbps_ = estimated_kbps;
  } else {
    LOG(INFO) << "[Prober] Probe failed or no improvement: target="
              << probe_target_kbps_ << " estimated=" << estimated_kbps;
    // A failed seed still ends initial probing (hand off to AIMD); a failed
    // periodic/recovery probe just returns to AIMD until the next interval.
    if (current_probe_type_ == ProbeType::kSeed) {
      initial_probing_done_ = true;
    }
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

void BandwidthProber::OnUnderuseDetected() {
  std::lock_guard<std::mutex> lock(mutex_);
  last_underuse_time_ms_ = NowMs();
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

  // 2. Bitrate drop recovery — only if the queue is draining (recent underuse).
  //    A bitrate drop is itself a congestion symptom; probing back up while the
  //    link is still congested deepens the congestion. Require a recent
  //    underuse signal, mirroring WebRTC's in_alr / alr_ended_recently gate.
  if (bitrate_drop_detected_) {
    bool queue_draining =
        now - last_underuse_time_ms_ < kUnderuseRecencyMs;
    if (now - bitrate_drop_time_ms_ < kBitrateDropTimeoutMs) {
      if (queue_draining) {
        InitiateDropRecoveryProbe();
        return;
      }
      // Still congested — hold the drop flag and wait for the link to drain.
    } else {
      bitrate_drop_detected_ = false;
    }
  }

  // 3. Periodic re-probing. After initial probing, search for headroom every
  //    kPeriodicProbeIntervalMs — independent of the ALR flag, which never
  //    fires for a greedy (mock/CBR) encoder. This is what drives convergence
  //    up to the cap after the seed cluster + AIMD, mirroring WebRTC where an
  //    ALR-limited real encoder gets a periodic probe on the same cadence.
  if (estimated_bitrate_kbps_ <
      max_bitrate_kbps_ * kPeriodicProbeMaxEstimateFraction) {
    if (now - last_periodic_probe_ms_ >= kPeriodicProbeIntervalMs) {
      InitiatePeriodicProbe();
      return;
    }
  }

  // 4. ALR periodic probing
  if (application_limited_) {
    if (now - last_alr_probe_ms_ >= kAlrProbeIntervalMs) {
      InitiateAlrProbe();
      return;
    }
  }
}

void BandwidthProber::InitiatePeriodicProbe() {
  probe_target_kbps_ = std::min(
      static_cast<int>(estimated_bitrate_kbps_ * kPeriodicProbeMultiplier),
      max_bitrate_kbps_);

  if (probe_target_kbps_ <= estimated_bitrate_kbps_) {
    last_periodic_probe_ms_ = NowMs();
    return;
  }

  current_probe_type_ = ProbeType::kPeriodic;
  chain_hops_ = 0;
  state_ = State::kProbing;
  probe_start_ms_ = NowMs();
  last_periodic_probe_ms_ = probe_start_ms_;
  pending_probes_.push_back({probe_target_kbps_, next_cluster_id_++});

  LOG(INFO) << "[Prober] Periodic probe at " << probe_target_kbps_
            << " kbps (est=" << estimated_bitrate_kbps_ << ")";
}

void BandwidthProber::InitiateExponentialProbe() {
  // Snapshot the estimate when the seed cluster begins, and take BOTH seeds
  // off that base. Using the live estimate for the 6x seed would compound: the
  // 3x probe commits a higher estimate first, so 6x of THAT lands near the
  // ceiling in two jumps. WebRTC fires 3x and 6x of the same start rate.
  if (exponential_probe_count_ == 0) {
    seed_base_kbps_ = estimated_bitrate_kbps_;
  }

  int multiplier = (exponential_probe_count_ == 0)
      ? kFirstExponentialMultiplier
      : kSecondExponentialMultiplier;

  probe_target_kbps_ = std::min(
      seed_base_kbps_ * multiplier, max_bitrate_kbps_);

  if (estimated_bitrate_kbps_ >= max_bitrate_kbps_ * 0.95) {
    initial_probing_done_ = true;
    return;
  }

  exponential_probe_count_++;
  if (exponential_probe_count_ >= kMaxExponentialProbes) {
    initial_probing_done_ = true;
  }

  current_probe_type_ = ProbeType::kSeed;
  chain_hops_ = 0;
  state_ = State::kProbing;
  probe_start_ms_ = NowMs();
  pending_probes_.push_back({probe_target_kbps_, next_cluster_id_++});

  LOG(INFO) << "[Prober] Exponential probe #" << exponential_probe_count_
            << " at " << probe_target_kbps_ << " kbps (seed_base="
            << seed_base_kbps_ << ")";
}

void BandwidthProber::InitiateAlrProbe() {
  probe_target_kbps_ = std::min(
      static_cast<int>(estimated_bitrate_kbps_ * kAlrProbeMultiplier),
      max_bitrate_kbps_);

  if (probe_target_kbps_ <= estimated_bitrate_kbps_) {
    return;
  }

  current_probe_type_ = ProbeType::kPeriodic;
  chain_hops_ = 0;
  state_ = State::kProbing;
  probe_start_ms_ = NowMs();
  last_alr_probe_ms_ = probe_start_ms_;
  pending_probes_.push_back({probe_target_kbps_, next_cluster_id_++});
  application_limited_ = false;

  LOG(INFO) << "[Prober] ALR probe at " << probe_target_kbps_
            << " kbps (est=" << estimated_bitrate_kbps_ << ")";
}

void BandwidthProber::InitiateDropRecoveryProbe() {
  // Drop-recovery references the (stale) pre-drop rate, which after a true
  // capacity cliff (10→1 Mbps) is far above the new capacity. Cap it by the
  // current estimate so we don't flood the link probing back toward a rate it
  // can no longer carry.
  probe_target_kbps_ = CapProbeTarget(std::min(
      static_cast<int>(pre_drop_bitrate_kbps_ * kProbeFractionAfterDrop),
      max_bitrate_kbps_));

  if (probe_target_kbps_ <= estimated_bitrate_kbps_) {
    bitrate_drop_detected_ = false;
    return;
  }

  current_probe_type_ = ProbeType::kDropRecovery;
  chain_hops_ = 0;
  state_ = State::kProbing;
  probe_start_ms_ = NowMs();
  pending_probes_.push_back({probe_target_kbps_, next_cluster_id_++});
  bitrate_drop_detected_ = false;

  LOG(INFO) << "[Prober] Drop recovery probe at " << probe_target_kbps_
            << " kbps (pre-drop=" << pre_drop_bitrate_kbps_
            << ", current=" << estimated_bitrate_kbps_ << ")";
}

int BandwidthProber::CapProbeTarget(int target_kbps) const {
  // Never probe at more than kMaxProbeMultipleOfEstimate × the current
  // estimate. Keeps a probe proportional to what the link carries now, so a
  // stale pre-drop reference can't push the probe far above real capacity.
  if (estimated_bitrate_kbps_ <= 0) {
    return target_kbps;
  }
  int cap = estimated_bitrate_kbps_ * kMaxProbeMultipleOfEstimate;
  return std::min(target_kbps, cap);
}
