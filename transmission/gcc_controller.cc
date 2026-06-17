#include "gcc_controller.h"

#include <algorithm>
#include <cmath>
#include <numeric>

#include "log_system/log_system.h"

int64_t GccController::NowMs() const {
  if (fake_clock_ms_) {
    return *fake_clock_ms_;
  }
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}

GccController::GccController()
    : accumulated_delay_(0.0),
      smoothed_delay_(0.0),
      first_arrival_ms_(0),
      prev_send_time_ms_(0),
      prev_arrival_time_ms_(0),
      num_deltas_(0),
      adaptive_threshold_(kInitialThreshold),
      last_threshold_update_ms_(0),
      overuse_counter_(0),
      last_overuse_time_ms_(0),
      time_over_using_ms_(0.0),
      prev_modified_trend_(0.0),
      last_detect_ms_(0),
      delay_based_bitrate_kbps_(1000),
      last_increase_time_ms_(0),
      loss_based_bitrate_kbps_(30000),
      packets_sent_since_last_loss_update_(0),
      packets_lost_since_last_loss_update_(0),
      last_loss_update_ms_(0),
      target_bitrate_kbps_(1000),
      min_bitrate_kbps_(100),
      max_bitrate_kbps_(30000),
      probe_active_(false),
      probe_started_ms_(0),
      probe_floor_kbps_(0),
      fake_clock_ms_(nullptr) {
  last_threshold_update_ms_ = NowMs();
  last_increase_time_ms_ = NowMs();
  last_loss_update_ms_ = NowMs();
}

void GccController::SetInitialBitrate(int kbps) {
  std::lock_guard<std::mutex> lock(mutex_);
  delay_based_bitrate_kbps_ = kbps;
  loss_based_bitrate_kbps_ = kbps;
  target_bitrate_kbps_ = kbps;
  prober_.SetEstimatedBitrate(kbps);
}

void GccController::SetBitrateRange(int min_kbps, int max_kbps) {
  std::lock_guard<std::mutex> lock(mutex_);
  min_bitrate_kbps_ = min_kbps;
  max_bitrate_kbps_ = max_kbps;
  prober_.SetMaxBitrate(max_kbps);
}

int GccController::GetTargetBitrateKbps() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return target_bitrate_kbps_;
}

int GccController::GetDelayBasedBitrateKbps() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return delay_based_bitrate_kbps_;
}

int GccController::GetLossBasedBitrateKbps() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return loss_based_bitrate_kbps_;
}

double GccController::GetAdaptiveThreshold() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return adaptive_threshold_;
}

int GccController::GetOveruseCounter() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return overuse_counter_;
}

void GccController::OnTransportFeedback(const TransportFeedback& feedback) {
  std::lock_guard<std::mutex> lock(mutex_);
  int64_t now_ms = NowMs();

  UpdateTrendline(feedback);
  double slope = ComputeTrendlineSlope();
  BandwidthUsage usage = Detect(slope);
  UpdateDelayBasedRate(usage, now_ms);

  // Periodically re-evaluate the loss-based estimate. Without this, loss_based
  // only ever changed on a loss report and stayed frozen during loss-free
  // operation — capping the target below the true capacity.
  MaybeUpdateLossRate(now_ms);

  // Feed signals to prober
  if (usage == BandwidthUsage::kOveruse) {
    prober_.OnOveruseDetected();
  }
  prober_.SetEstimatedBitrate(delay_based_bitrate_kbps_);

  // --- Probe lifecycle management ---
  // The prober raises the send rate to test for headroom; we must resolve the
  // probe. Detect a freshly-started probe, then on each subsequent feedback
  // decide: ABORT if the elevated rate induced congestion (overuse, queue
  // growth past kProbeAbortDelayMs, or loss-based estimate collapsing below
  // the pre-probe floor); COMMIT if the eval window elapsed with delay low.
  bool probing = prober_.GetState() == BandwidthProber::State::kProbing ||
                 prober_.GetState() == BandwidthProber::State::kWaitingForResult;
  if (probing && !probe_active_) {
    // New probe just started — snapshot start time and floor.
    probe_active_ = true;
    probe_started_ms_ = now_ms;
    probe_floor_kbps_ = loss_based_bitrate_kbps_;
    // Reset the queue-delay integrator so the probe's effect is measured fresh.
    accumulated_delay_ = 0.0;
    smoothed_delay_ = 0.0;
  } else if (probing && probe_active_) {
    bool congested = (usage == BandwidthUsage::kOveruse) ||
                     (accumulated_delay_ > kProbeAbortDelayMs) ||
                     (loss_based_bitrate_kbps_ < probe_floor_kbps_ / 2);
    if (congested) {
      prober_.OnProbeResult(delay_based_bitrate_kbps_, /*success=*/false);
      probe_active_ = false;
    } else if (now_ms - probe_started_ms_ > kProbeEvalWindowMs) {
      prober_.OnProbeResult(prober_.GetEffectiveBitrateKbps(), /*success=*/true);
      probe_active_ = false;
    }
  } else if (!probing) {
    probe_active_ = false;
  }

  target_bitrate_kbps_ = ComputeFinalBitrate();

  // Queuing-delay signal for plotting: the per-batch change in accumulated
  // delay (ms). Absolute one-way delay is NOT meaningful here because send
  // timestamps (sender clock) and arrival timestamps (receiver clock) have
  // different origins — only deltas within each domain are valid, which is
  // exactly what the trendline uses. We expose accumulated_delay (the
  // integrated queuing delay) as the delay signal.
  double queuing_delay_ms = accumulated_delay_;

  // Structured state line for offline analysis / plotting.
  const char* usage_str = usage == BandwidthUsage::kOveruse ? "overuse"
                        : usage == BandwidthUsage::kUnderuse ? "underuse" : "normal";
  LOG(INFO) << "[GCC_STATE] target=" << target_bitrate_kbps_
            << " delay_based=" << delay_based_bitrate_kbps_
            << " loss_based=" << loss_based_bitrate_kbps_
            << " slope=" << slope
            << " threshold=" << adaptive_threshold_
            << " usage=" << usage_str
            << " queuing_delay_ms=" << queuing_delay_ms;
}

void GccController::OnLossReport(const LossReport& report) {
  std::lock_guard<std::mutex> lock(mutex_);

  // Just accumulate lost packets; the periodic MaybeUpdateLossRate (driven
  // from the feedback path) recomputes the loss fraction over the window.
  packets_lost_since_last_loss_update_ += static_cast<int>(report.packets.size());

  MaybeUpdateLossRate(NowMs());
  target_bitrate_kbps_ = ComputeFinalBitrate();
}

void GccController::OnPacketsSent(int count) {
  std::lock_guard<std::mutex> lock(mutex_);
  packets_sent_since_last_loss_update_ += count;
}

void GccController::MaybeUpdateLossRate(int64_t now_ms) {
  // Re-evaluate loss-based estimate at most every kLossUpdateIntervalMs, once
  // we have observed at least kLossUpdateMinPackets sends in the window.
  if (now_ms - last_loss_update_ms_ < kLossUpdateIntervalMs) {
    return;
  }
  if (packets_sent_since_last_loss_update_ < kLossUpdateMinPackets) {
    return;
  }

  double loss_fraction = static_cast<double>(packets_lost_since_last_loss_update_) /
      packets_sent_since_last_loss_update_;
  UpdateLossBasedRate(loss_fraction);

  last_loss_update_ms_ = now_ms;
  packets_sent_since_last_loss_update_ = 0;
  packets_lost_since_last_loss_update_ = 0;
}

void GccController::SetClockForTesting(int64_t* clock_ms) {
  fake_clock_ms_ = clock_ms;
  if (clock_ms) {
    last_threshold_update_ms_ = *clock_ms;
    last_increase_time_ms_ = *clock_ms;
    last_loss_update_ms_ = *clock_ms;
    last_overuse_time_ms_ = *clock_ms - 2000;  // allow immediate probing in tests
  }
}

// --- Trendline estimator (WebRTC: trendline_estimator.cc) ---
//
// Processes each acknowledged packet using its real send and arrival
// timestamps. The inter-packet delay variation
//   delay_delta = (arrival[i] - arrival[i-1]) - (send[i] - send[i-1])
// is accumulated and fed into a linear-regression window. A positive slope
// means queuing delay is growing (potential overuse).

void GccController::UpdateTrendline(const TransportFeedback& feedback) {
  for (const auto& pkt : feedback.packets) {
    if (pkt.send_time_us < 0) {
      continue;
    }

    // Work in microseconds for precision (avoid truncation of sub-ms deltas)
    int64_t send_us = pkt.send_time_us;
    int64_t arrival_us = pkt.arrival_time_us;

    if (first_arrival_ms_ == 0) {
      first_arrival_ms_ = arrival_us;  // Reusing field name, but now stores us
      prev_arrival_time_ms_ = arrival_us;
      prev_send_time_ms_ = send_us;
      continue;
    }

    // Per-packet inter-arrival and inter-send deltas (in ms, from us)
    double arrival_delta_ms = (arrival_us - prev_arrival_time_ms_) / 1000.0;
    double send_delta_ms = (send_us - prev_send_time_ms_) / 1000.0;
    double delay_delta_ms = arrival_delta_ms - send_delta_ms;

    prev_arrival_time_ms_ = arrival_us;
    prev_send_time_ms_ = send_us;

    // Accumulate delay in ms
    accumulated_delay_ += delay_delta_ms;

    // EWMA-smooth the accumulated delay (WebRTC smoothing_coef_ = 0.9). The
    // smoothed value is what feeds the regression window — it suppresses the
    // per-packet scheduling jitter that would otherwise produce spurious
    // positive slopes (false overuse) on an uncongested link. Under real
    // congestion the accumulated delay grows monotonically, so the smoothed
    // signal still tracks it and the slope goes clearly positive.
    smoothed_delay_ = kTrendlineSmoothingCoeff * smoothed_delay_ +
                      (1.0 - kTrendlineSmoothingCoeff) * accumulated_delay_;

    double time_since_first_ms = (arrival_us - first_arrival_ms_) / 1000.0;
    trendline_window_.push_back({smoothed_delay_, time_since_first_ms});
    if (static_cast<int>(trendline_window_.size()) > kTrendlineWindowSize) {
      trendline_window_.pop_front();
    }
    num_deltas_++;
  }
}

double GccController::ComputeTrendlineSlope() const {
  if (trendline_window_.size() < 4) {
    return 0.0;
  }

  // Linear least-squares regression: y = smoothed_delay, x = time
  int n = static_cast<int>(trendline_window_.size());
  double sum_x = 0, sum_y = 0, sum_xy = 0, sum_xx = 0;

  // Normalize x values to [0, 1] for numerical stability
  double x_base = trendline_window_.front().arrival_time_ms;
  double x_range = trendline_window_.back().arrival_time_ms - x_base;
  if (x_range <= 0) return 0.0;

  for (const auto& point : trendline_window_) {
    double x = (point.arrival_time_ms - x_base) / x_range;
    double y = point.smoothed_delay;
    sum_x += x;
    sum_y += y;
    sum_xy += x * y;
    sum_xx += x * x;
  }

  double denom = n * sum_xx - sum_x * sum_x;
  if (std::abs(denom) < 1e-9) return 0.0;

  double slope = (n * sum_xy - sum_x * sum_y) / denom;
  return slope;
}

// --- Overuse detector (WebRTC: trendline_estimator.cc Detect()) ---

GccController::BandwidthUsage GccController::Detect(double trendline_slope) {
  int64_t now_ms = NowMs();

  // Modified trend = slope * threshold_gain
  double modified_trend = trendline_slope * kThresholdGain;
  // Scale by number of deltas to reduce sensitivity at startup
  if (num_deltas_ < 10) {
    modified_trend *= static_cast<double>(num_deltas_) / 10.0;
  }

  int64_t dt_ms = (last_detect_ms_ == 0) ? 0 : (now_ms - last_detect_ms_);
  last_detect_ms_ = now_ms;

  BandwidthUsage result = BandwidthUsage::kNormal;

  if (modified_trend > adaptive_threshold_) {
    // Accumulate the time spent over-using. Only declare overuse once that
    // time exceeds kOverusingTimeThresholdMs AND the trend is not shrinking
    // (WebRTC OveruseDetector). This rejects isolated noise spikes.
    time_over_using_ms_ += static_cast<double>(dt_ms);
    overuse_counter_++;
    if (time_over_using_ms_ > kOverusingTimeThresholdMs &&
        overuse_counter_ >= kOveruseCountThreshold &&
        modified_trend >= prev_modified_trend_) {
      time_over_using_ms_ = 0.0;
      overuse_counter_ = 0;
      result = BandwidthUsage::kOveruse;
    }
  } else if (modified_trend < -adaptive_threshold_) {
    time_over_using_ms_ = 0.0;
    overuse_counter_ = 0;
    result = BandwidthUsage::kUnderuse;
  } else {
    time_over_using_ms_ = 0.0;
    overuse_counter_ = 0;
    result = BandwidthUsage::kNormal;
  }

  prev_modified_trend_ = modified_trend;
  UpdateAdaptiveThreshold(modified_trend, now_ms);
  return result;
}

void GccController::UpdateAdaptiveThreshold(double modified_trend, int64_t now_ms) {
  // WebRTC adaptive threshold logic:
  // If |modified_trend| > threshold: threshold increases at k_up rate
  // Otherwise: threshold decreases at k_down rate toward kMinThreshold
  int64_t time_delta_ms = now_ms - last_threshold_update_ms_;
  if (time_delta_ms <= 0) return;
  last_threshold_update_ms_ = now_ms;

  double abs_trend = std::abs(modified_trend);
  if (abs_trend > adaptive_threshold_) {
    // Increase threshold: adaptive_threshold += k_up * (abs_trend - threshold) * dt
    adaptive_threshold_ += kThresholdUp * (abs_trend - adaptive_threshold_) *
                           static_cast<double>(time_delta_ms);
  } else {
    // Decrease threshold toward kMinThreshold
    adaptive_threshold_ += kThresholdDown * (kMinThreshold - adaptive_threshold_) *
                           static_cast<double>(time_delta_ms);
  }
  adaptive_threshold_ = std::clamp(adaptive_threshold_, kMinThreshold, kMaxThreshold);
}

// --- Rate controller (WebRTC: AIMD) ---

void GccController::UpdateDelayBasedRate(BandwidthUsage usage, int64_t now_ms) {
  switch (usage) {
    case BandwidthUsage::kOveruse: {
      // Multiplicative decrease: new_rate = 0.85 * estimated_throughput
      // WebRTC uses the estimated throughput from acknowledged bitrate,
      // but we approximate with current delay-based rate
      delay_based_bitrate_kbps_ = static_cast<int>(
          delay_based_bitrate_kbps_ * kMultiplicativeDecrease);
      delay_based_bitrate_kbps_ = std::max(delay_based_bitrate_kbps_, min_bitrate_kbps_);
      overuse_counter_ = 0;
      last_overuse_time_ms_ = now_ms;
      LOG(INFO) << "[GCC] Overuse → decrease to " << delay_based_bitrate_kbps_ << " kbps";
      break;
    }
    case BandwidthUsage::kUnderuse:
    case BandwidthUsage::kNormal: {
      // Additive increase: ~8% of current rate per second
      // WebRTC: increase = max(1000, beta * rate) per second
      // where beta ≈ 0.08
      int64_t time_since_last_ms = now_ms - last_increase_time_ms_;
      if (time_since_last_ms <= 0) break;

      // Don't increase too soon after overuse (WebRTC: wait ~1s)
      if (now_ms - last_overuse_time_ms_ < 1000) break;

      double seconds = time_since_last_ms / 1000.0;
      int increase_kbps = static_cast<int>(
          delay_based_bitrate_kbps_ * kIncreaseRatePerSecond * seconds);
      increase_kbps = std::max(increase_kbps, static_cast<int>(kMinIncreaseKbps * seconds));

      delay_based_bitrate_kbps_ += increase_kbps;
      delay_based_bitrate_kbps_ = std::min(delay_based_bitrate_kbps_, max_bitrate_kbps_);
      last_increase_time_ms_ = now_ms;
      break;
    }
  }
}

// --- Loss-based rate control (WebRTC: send_side_bandwidth_estimation.cc) ---

void GccController::UpdateLossBasedRate(double loss_fraction) {
  if (loss_fraction < kLossIncreaseThreshold) {
    // < 2% loss: increase. Called every ~kLossUpdateIntervalMs (0.5s), so
    // grow ~8%/s → ~4% per update. This lets loss_based track upward toward
    // the true capacity instead of staying pinned at the initial estimate.
    int increase_kbps = std::max(
        static_cast<int>(loss_based_bitrate_kbps_ * kIncreaseRatePerSecond *
                         (kLossUpdateIntervalMs / 1000.0)),
        kMinIncreaseKbps);
    loss_based_bitrate_kbps_ += increase_kbps;
    loss_based_bitrate_kbps_ = std::min(loss_based_bitrate_kbps_, max_bitrate_kbps_);
  } else if (loss_fraction >= kLossHoldThreshold) {
    // >= 10% loss: decrease by (1 - 0.5 * loss_fraction)
    // At 10% loss: factor = 0.95, at 20%: factor = 0.90, at 50%: factor = 0.75
    double factor = 1.0 - 0.5 * loss_fraction;
    factor = std::max(factor, 0.5);  // Never decrease more than 50% at once
    loss_based_bitrate_kbps_ = static_cast<int>(loss_based_bitrate_kbps_ * factor);
    loss_based_bitrate_kbps_ = std::max(loss_based_bitrate_kbps_, min_bitrate_kbps_);
    LOG(INFO) << "[GCC] Loss=" << static_cast<int>(loss_fraction * 100)
              << "% → loss-based rate=" << loss_based_bitrate_kbps_ << " kbps";
  }
  // 2-10% loss: hold (no change)
}

int GccController::ComputeFinalBitrate() const {
  int base_kbps = std::min(delay_based_bitrate_kbps_, loss_based_bitrate_kbps_);
  // Always consult the prober so it can advance its state machine (initiate
  // probes when idle, run/finish active probes). When it is probing, its
  // effective rate may exceed the base estimate to actively test for headroom.
  auto& prober_ref = const_cast<BandwidthProber&>(prober_);
  int probe_kbps = prober_ref.GetEffectiveBitrateKbps();
  if (prober_ref.GetState() != BandwidthProber::State::kIdle &&
      probe_kbps > base_kbps) {
    base_kbps = probe_kbps;
  }
  return std::clamp(base_kbps, min_bitrate_kbps_, max_bitrate_kbps_);
}
