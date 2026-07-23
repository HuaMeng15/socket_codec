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
      chain_hops_(0),
      application_limited_(false),
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

    // Chaining rule (aligned with the WebRTC reference curve):
    //   - Seed probes (3x/6x startup cluster) do NOT chain. They commit their
    //     measured rate and hand control to AIMD, which is why the reference
    //     jumps once to ~4 Mbps at startup and then climbs slowly — rather than
    //     chaining straight to the ceiling and flooding the link.
    //   - Periodic (ALR) probes MAY chain, but only up to kMaxChainHops hops,
    //     AND each hop is capped at 1.5× the just-measured rate (CapProbeTarget).
    //     Bounding both the hop count and the per-hop multiple stops a single
    //     probe session from running to ~2× the link capacity — the runaway
    //     that flooded the pipe and caused the overuse→"drop"→re-probe cycle.
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

    // Only ALR (periodic) probes chain within OnProbeResult.
    bool chainable = (current_probe_type_ == ProbeType::kPeriodic);
    if (chainable && chain_hops_ < kMaxChainHops &&
        ratio >= kFurtherProbeThreshold &&
        estimated_kbps < max_bitrate_kbps_ * 0.95) {
      chain_hops_++;
      // Cap against the freshly measured rate (estimated_bitrate_kbps_ hasn't
      // been updated with this probe's result yet), so the next hop explores at
      // most 1.5× what the link just demonstrated it can carry.
      probe_target_kbps_ =
          std::min(static_cast<int>(estimated_kbps * kMaxProbeIncreaseLimit),
                   max_bitrate_kbps_);
      state_ = State::kProbing;
      probe_start_ms_ = NowMs();
      pending_probes_.push_back({probe_target_kbps_, next_cluster_id_++});
      LOG(INFO) << "[Prober] Further probe (" << chain_hops_ << "/"
                << kMaxChainHops << ") at " << probe_target_kbps_ << " kbps";
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
  if (application_limited_) {
    if (now - last_alr_probe_ms_ >= kAlrProbeIntervalMs) {
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
  chain_hops_ = 0;
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
  chain_hops_ = 0;
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
  chain_hops_ = 0;
  state_ = State::kProbing;
  probe_start_ms_ = NowMs();
  last_alr_probe_ms_ = probe_start_ms_;
  pending_probes_.push_back({probe_target_kbps_, next_cluster_id_++});
  application_limited_ = false;

  LOG(INFO) << "[Prober] ALR probe at " << probe_target_kbps_
            << " kbps (est=" << estimated_bitrate_kbps_ << ")";
}

int BandwidthProber::CapProbeTarget(int target_kbps) const {
  // WebRTC AimdRateControl caps every increase at 1.5×throughput. We apply the
  // same bound to probe targets: a probe explores at most 50% above the current
  // estimate, so it can't overshoot to ~2× capacity and flood the bottleneck.
  if (estimated_bitrate_kbps_ <= 0) {
    return target_kbps;
  }
  int cap = static_cast<int>(estimated_bitrate_kbps_ * kMaxProbeIncreaseLimit);
  return std::min(target_kbps, cap);
}
