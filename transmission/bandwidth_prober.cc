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
      startup_stage_(true),
      last_startup_probe_ms_(0),
      current_probe_type_(ProbeType::kSeed),
      application_limited_(false),
      periodic_probing_allowed_(true),
      application_limited_since_ms_(-1),
      last_alr_probe_ms_(0),
      probe_target_kbps_(0),
      next_cluster_id_(0),
      probe_start_ms_(0),
      last_overuse_time_ms_(0),
      fake_clock_ms_(nullptr) {
  int64_t now = NowMs();
  last_alr_probe_ms_ = now;
  probe_start_ms_ = now;
  last_overuse_time_ms_ = now;
}

void BandwidthProber::SetClockForTesting(int64_t* clock_ms) {
  fake_clock_ms_ = clock_ms;
  if (clock_ms) {
    int64_t now = *clock_ms;
    last_alr_probe_ms_ = now;
    last_startup_probe_ms_ = now;
    probe_start_ms_ = now;
    last_overuse_time_ms_ = now - 2000;  // allow probing immediately in tests
  }
}

void BandwidthProber::SetEstimatedBitrate(int bitrate_kbps) {
  std::lock_guard<std::mutex> lock(mutex_);
  // No drop-detection here: a bitrate drop is recovered by AIMD, not a probe
  // (WebRTC-faithful). Reacting to a drop with a probe re-fed the estimate dip
  // caused by our own probe overshoot straight back into another probe.
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

    // Seed, startup, and periodic probes all commit at most one measured step.
    // In particular, a periodic ALR probe must not immediately chain: trial 5
    // showed that three individually plausible 1.5x hops can compound from
    // 3.9 Mbps to 13 Mbps in under 100 ms, just before a capacity cliff.
    double ratio = static_cast<double>(estimated_kbps) / probe_target_kbps_;

    // Startup accelerator: a strong result keeps the stage going (the settle
    // timer in MaybeInitiateProbe fires the next 1.5x probe); a weak result
    // (< 70% of target) means the link can't sustain the higher rate, so end
    // the stage and hand off to AIMD. Startup probes never chain here — their
    // cadence is the settle timer, not immediate re-fire.
    if (current_probe_type_ == ProbeType::kStartup) {
      if (ratio < kFurtherProbeThreshold) {
        startup_stage_ = false;
        LOG(INFO) << "[Prober] Startup probe weak (ratio=" << ratio
                  << ") — ending startup stage";
      }
      return;
    }

    if (current_probe_type_ == ProbeType::kPeriodic) {
      LOG(INFO) << "[Prober] Periodic probe completed at one-step target="
                << probe_target_kbps_ << " kbps measured=" << estimated_kbps
                << " kbps";
    }
  } else {
    LOG(INFO) << "[Prober] Probe failed or no improvement: target="
              << probe_target_kbps_ << " estimated=" << estimated_kbps;
    // A failed probe means the link couldn't sustain even this rate, so stop
    // ramping: a failed seed ends initial probing AND the startup accelerator
    // (no point probing higher after the link rejected the seed); a failed
    // startup probe ends the startup stage; a failed periodic probe just
    // returns to AIMD until the next interval.
    if (current_probe_type_ == ProbeType::kSeed) {
      initial_probing_done_ = true;
      startup_stage_ = false;
    } else if (current_probe_type_ == ProbeType::kStartup) {
      startup_stage_ = false;
    }
  }
}

void BandwidthProber::OnOveruseDetected() {
  std::lock_guard<std::mutex> lock(mutex_);
  last_overuse_time_ms_ = NowMs();
  initial_probing_done_ = true;
  // Latency rising ends the startup stage — we've found the ceiling; let normal
  // AIMD take over from here.
  startup_stage_ = false;
  application_limited_ = false;
  application_limited_since_ms_ = -1;

  if (state_ != State::kIdle) {
    LOG(INFO) << "[Prober] Overuse during probe, cancelling";
    state_ = State::kIdle;
    pending_probes_.clear();
  }
}

void BandwidthProber::OnUnderuseDetected() {
  // No-op. Recovery from a drop is handled by AIMD (GccController), not a probe.
  // Kept so GccController's existing call site needs no change.
}

void BandwidthProber::SetApplicationLimited(bool limited) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (application_limited_ == limited) return;
  application_limited_ = limited;
  application_limited_since_ms_ = limited ? NowMs() : -1;
}

void BandwidthProber::SetPeriodicProbingAllowed(bool allowed) {
  std::lock_guard<std::mutex> lock(mutex_);
  periodic_probing_allowed_ = allowed;
  if (!allowed && current_probe_type_ == ProbeType::kPeriodic &&
      state_ != State::kIdle) {
    LOG(INFO) << "[Prober] Network state became unhealthy during periodic "
                 "probe, cancelling";
    state_ = State::kIdle;
    pending_probes_.clear();
  }
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

  // 1. Initial exponential probing (the 3x/6x seed cluster).
  if (!initial_probing_done_) {
    InitiateExponentialProbe();
    return;
  }

  // 2. Startup-stage accelerator. After the seeds, keep probing 1.5x every
  //    settle interval so a fast idle link converges in a few seconds instead
  //    of AIMD's slow additive crawl. Ends on a weak probe or overuse (handled
  //    in OnProbeResult / OnOveruseDetected). At the ceiling there is nothing
  //    left to find, so exit the stage.
  if (startup_stage_) {
    if (estimated_bitrate_kbps_ >= max_bitrate_kbps_ * 0.95) {
      startup_stage_ = false;
    } else if (now - last_startup_probe_ms_ >= kStartupSettleMs) {
      InitiateStartupProbe();
      return;
    } else {
      return;  // still settling from the last startup probe
    }
  }

  // 3. ALR periodic probing. WebRTC fires a periodic probe every
  //    kAlrProbeIntervalMs ONLY while the sender is application-limited — the
  //    encoder is producing below the estimate, so AIMD can't grow the estimate
  //    (nothing is pushing on the pipe) and a probe is the only way to discover
  //    freed-up headroom. When the encoder is filling the pipe the sender is
  //    NOT in ALR, no periodic probe fires, and plain AIMD tracks capacity.
  //    The application_limited_ flag is driven by the real AlrDetector
  //    (bytes-sent vs. target), so a greedy encoder never trips it — exactly
  //    like WebRTC.
  if (periodic_probing_allowed_ && application_limited_ &&
      application_limited_since_ms_ >= 0) {
    if (now - application_limited_since_ms_ >= kAlrQualificationMs &&
        now - last_alr_probe_ms_ >= kAlrProbeIntervalMs) {
      InitiateAlrProbe();
      return;
    }
  }
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
  state_ = State::kProbing;
  probe_start_ms_ = NowMs();
  pending_probes_.push_back({probe_target_kbps_, next_cluster_id_++});
  // Start the startup-stage settle clock from the last seed, so the first
  // accelerator probe waits one settle interval after the seed commits.
  last_startup_probe_ms_ = probe_start_ms_;

  LOG(INFO) << "[Prober] Exponential probe #" << exponential_probe_count_
            << " at " << probe_target_kbps_ << " kbps (seed_base="
            << seed_base_kbps_ << ")";
}

void BandwidthProber::InitiateStartupProbe() {
  // Explore 1.5x the current estimate (AimdRateControl increase limit). Unlike
  // the seed cluster this repeats every settle interval until overuse or a weak
  // result ends the startup stage.
  probe_target_kbps_ = std::min(
      static_cast<int>(estimated_bitrate_kbps_ * kStartupProbeMultiplier),
      max_bitrate_kbps_);

  if (probe_target_kbps_ <= estimated_bitrate_kbps_) {
    startup_stage_ = false;  // nowhere to grow — hand off to AIMD
    return;
  }

  current_probe_type_ = ProbeType::kStartup;
  state_ = State::kProbing;
  probe_start_ms_ = NowMs();
  last_startup_probe_ms_ = probe_start_ms_;
  pending_probes_.push_back({probe_target_kbps_, next_cluster_id_++});

  LOG(INFO) << "[Prober] Startup probe at " << probe_target_kbps_
            << " kbps (est=" << estimated_bitrate_kbps_ << ")";
}

void BandwidthProber::InitiateAlrProbe() {
  probe_target_kbps_ = CapProbeTarget(std::min(
      static_cast<int>(estimated_bitrate_kbps_ * kAlrProbeMultiplier),
      max_bitrate_kbps_));

  if (probe_target_kbps_ <= estimated_bitrate_kbps_) {
    return;
  }

  current_probe_type_ = ProbeType::kPeriodic;
  state_ = State::kProbing;
  probe_start_ms_ = NowMs();
  last_alr_probe_ms_ = probe_start_ms_;
  pending_probes_.push_back({probe_target_kbps_, next_cluster_id_++});

  LOG(INFO) << "[Prober] ALR probe at " << probe_target_kbps_
            << " kbps (est=" << estimated_bitrate_kbps_ << ")";
}

int BandwidthProber::CapProbeTarget(int target_kbps) const {
  // Periodic discovery is intentionally gentler than startup: explore at most
  // 25% above the current measured line, then wait for another qualified ALR
  // interval before exploring again. The target remains fully measurement-
  // driven; there is no fixed bitrate or trace-specific ceiling.
  if (estimated_bitrate_kbps_ <= 0) {
    return target_kbps;
  }
  int cap = static_cast<int>(estimated_bitrate_kbps_ *
                             kMaxPeriodicProbeIncreaseLimit);
  return std::min(target_kbps, cap);
}
