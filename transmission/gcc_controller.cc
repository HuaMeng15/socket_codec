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
      prev_send_ms_d_(0.0),
      prev_arrival_ms_d_(0.0),
      num_deltas_(0),
      adaptive_threshold_(kInitialThreshold),
      last_threshold_update_ms_(0),
      overuse_counter_(0),
      last_overuse_time_ms_(0),
      time_over_using_ms_(-1.0),
      prev_trend_(0.0),
      delay_based_bitrate_kbps_(1000),
      last_increase_time_ms_(0),
      acked_bitrate_kbps_(0.0),
      loss_based_bitrate_kbps_(30000),
      packets_sent_since_last_loss_update_(0),
      packets_lost_since_last_loss_update_(0),
      last_loss_update_ms_(0),
      target_bitrate_kbps_(1000),
      min_bitrate_kbps_(100),
      max_bitrate_kbps_(30000),
      probe_active_(false),
      probe_floor_kbps_(0),
      last_received_rate_kbps_(0.0),
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

void GccController::SetAckedBitrateForTesting(double kbps) {
  std::lock_guard<std::mutex> lock(mutex_);
  acked_bitrate_kbps_ = kbps;
  acked_frozen_for_testing_ = true;
}

void GccController::OnTransportFeedback(const TransportFeedback& feedback) {
  std::lock_guard<std::mutex> lock(mutex_);
  int64_t now_ms = NowMs();

  BandwidthUsage usage = UpdateTrendline(feedback);
  UpdateAckedBitrate(feedback);
  UpdateDelayBasedRate(usage, now_ms);

  // Periodically re-evaluate the loss-based estimate. Without this, loss_based
  // only ever changed on a loss report and stayed frozen during loss-free
  // operation — capping the target below the true capacity.
  MaybeUpdateLossRate(now_ms);

  // Feed signals to prober
  if (usage == BandwidthUsage::kOveruse) {
    prober_.OnOveruseDetected();
  } else if (usage == BandwidthUsage::kUnderuse) {
    // Queue draining — permits drop-recovery probing (see BandwidthProber).
    prober_.OnUnderuseDetected();
  }
  prober_.SetEstimatedBitrate(delay_based_bitrate_kbps_);

  // --- Probe lifecycle management (WebRTC ProbeBitrateEstimator semantics) ---
  // The prober raises the send rate to test for headroom. WebRTC resolves a
  // probe the moment it has a received-rate sample for the probe traffic — it
  // does NOT wait a fixed time window. We mirror that: the first feedback batch
  // after a probe starts carries the probe traffic's received rate, so we
  // commit immediately from that measurement.
  //
  // The received rate is self-limiting: if the probe target exceeds capacity,
  // those packets queue at the bottleneck and arrive at ~capacity, so we
  // measure capacity directly and commit to it — never to the over-target send
  // rate. This is what avoids overshoot and converges quickly.
  bool probing = prober_.GetState() == BandwidthProber::State::kProbing ||
                 prober_.GetState() == BandwidthProber::State::kWaitingForResult;
  if (probing && !probe_active_) {
    // New probe just started — snapshot the floor and reset the delay
    // integrator so the probe's effect is measured fresh. We do NOT commit on
    // this batch: the probe traffic hasn't been acknowledged yet.
    probe_active_ = true;
    probe_floor_kbps_ = loss_based_bitrate_kbps_;
    accumulated_delay_ = 0.0;
    smoothed_delay_ = 0.0;
  } else if (probing && probe_active_) {
    // Abort if the elevated rate already induced congestion.
    bool congested = (usage == BandwidthUsage::kOveruse) ||
                     (accumulated_delay_ > kProbeAbortDelayMs) ||
                     (loss_based_bitrate_kbps_ < probe_floor_kbps_ / 2);
    if (congested) {
      prober_.OnProbeResult(delay_based_bitrate_kbps_, /*success=*/false);
      probe_active_ = false;
    } else if (last_received_rate_kbps_ > 0.0) {
      // We have a received-rate sample for the probe traffic — resolve now.
      int probe_target = prober_.GetEffectiveBitrateKbps();
      int received = static_cast<int>(last_received_rate_kbps_);
      // WebRTC commits min(send_rate, receive_rate); the probe target bounds the
      // send rate, so the result is min(target, received). When the link is
      // saturated (received well below the probe target), back off to
      // kProbeUtilizationFraction × received to avoid immediate overuse.
      int probed;
      if (received < probe_target) {
        probed = static_cast<int>(kProbeUtilizationFraction * received);
      } else {
        probed = probe_target;
      }
      bool real_gain = probed > delay_based_bitrate_kbps_;
      prober_.OnProbeResult(probed, /*success=*/real_gain);
      probe_active_ = false;
      if (real_gain) {
        delay_based_bitrate_kbps_ = std::min(probed, max_bitrate_kbps_);
        last_increase_time_ms_ = now_ms;  // reset AIMD pacing from new base
      }
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
            << " slope=" << prev_trend_
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
  // Forward the same fake clock to the embedded prober, otherwise it reads the
  // real steady_clock and its inter-probe timing gates fight the fake clock.
  prober_.SetClockForTesting(clock_ms);
  if (clock_ms) {
    last_threshold_update_ms_ = *clock_ms;
    last_increase_time_ms_ = *clock_ms;
    last_loss_update_ms_ = *clock_ms;
    last_overuse_time_ms_ = *clock_ms - 2000;  // allow immediate probing in tests
  }
}

// --- Acknowledged throughput estimate ---
//
// Estimate the rate actually getting through from a feedback batch: the
// packets in the batch arrived over a span, so received_rate = bytes / span.
// Packets are ~MTU; we approximate payload as kPayloadBytesPerPacket. The
// result is EWMA-smoothed. AimdRateControl uses this to cap the increase
// (<= 1.5x acked) and to snap the decrease to measured throughput.
void GccController::UpdateAckedBitrate(const TransportFeedback& feedback) {
  if (acked_frozen_for_testing_) {
    // Tests pin the smoothed acked estimate; mirror it as the raw per-batch
    // received-rate sample so probe-commit logic is exercisable in unit tests.
    last_received_rate_kbps_ = acked_bitrate_kbps_;
    return;
  }
  if (feedback.packets.size() < 2) {
    return;
  }
  int64_t first_us = feedback.packets.front().arrival_time_us;
  int64_t last_us = feedback.packets.back().arrival_time_us;
  double span_s = (last_us - first_us) / 1e6;
  if (span_s <= 0) {
    return;
  }
  // Bytes received in the batch (exclude the first packet: rate is over the
  // gaps between arrivals).
  double bytes = (feedback.packets.size() - 1) *
                 static_cast<double>(kPayloadBytesPerPacket);
  double sample_kbps = bytes * 8.0 / 1000.0 / span_s;

  // Keep the raw per-batch sample for probe resolution (WebRTC resolves probes
  // from the measured received rate, not the EWMA-smoothed estimate).
  last_received_rate_kbps_ = sample_kbps;

  if (acked_bitrate_kbps_ <= 0.0) {
    acked_bitrate_kbps_ = sample_kbps;
  } else {
    acked_bitrate_kbps_ = kAckedSmoothingCoeff * acked_bitrate_kbps_ +
                          (1.0 - kAckedSmoothingCoeff) * sample_kbps;
  }
}

// --- Trendline estimator (faithful to WebRTC trendline_estimator.cc) ---
//
// For each acknowledged packet we compute the one-way delay variation
//   delta = recv_delta - send_delta
// accumulate it, EWMA-smooth it, and keep a duration-based window
// (<= kTrendlineWindowMs of arrival span). When the window is trimmed we refit
// a least-squares line; its slope is the trend fed to Detect(). All times are
// in milliseconds, matching WebRTC.

GccController::BandwidthUsage GccController::UpdateTrendline(
    const TransportFeedback& feedback) {
  BandwidthUsage usage = BandwidthUsage::kNormal;
  for (const auto& pkt : feedback.packets) {
    if (pkt.send_time_us < 0) {
      continue;
    }

    double send_ms = pkt.send_time_us / 1000.0;
    double arrival_ms = pkt.arrival_time_us / 1000.0;

    if (first_arrival_ms_ == 0) {
      first_arrival_ms_ = static_cast<int64_t>(arrival_ms);
      // Need a previous packet to form a delta; record and continue.
      prev_arrival_ms_d_ = arrival_ms;
      prev_send_ms_d_ = send_ms;
      continue;
    }

    double recv_delta_ms = arrival_ms - prev_arrival_ms_d_;
    double send_delta_ms = send_ms - prev_send_ms_d_;
    double delta_ms = recv_delta_ms - send_delta_ms;
    prev_arrival_ms_d_ = arrival_ms;
    prev_send_ms_d_ = send_ms;

    num_deltas_++;
    if (num_deltas_ > kDeltaCounterMax) num_deltas_ = kDeltaCounterMax;

    // Exponential backoff filter.
    accumulated_delay_ += delta_ms;
    smoothed_delay_ = kTrendlineSmoothingCoeff * smoothed_delay_ +
                      (1.0 - kTrendlineSmoothingCoeff) * accumulated_delay_;

    double rel_arrival_ms = arrival_ms - first_arrival_ms_;
    trendline_window_.push_back({rel_arrival_ms, smoothed_delay_});

    // Maintain a duration-based window: pop until span <= kTrendlineWindowMs.
    double trend = prev_trend_;
    bool dofit = false;
    double duration = trendline_window_.back().arrival_time_ms -
                      trendline_window_.front().arrival_time_ms;
    while (duration > kTrendlineWindowMs && trendline_window_.size() > 2) {
      dofit = true;
      trendline_window_.pop_front();
      duration = trendline_window_.back().arrival_time_ms -
                 trendline_window_.front().arrival_time_ms;
    }
    if (dofit) {
      trend = ComputeTrendlineSlope();
    }

    usage = Detect(trend, send_delta_ms, static_cast<int64_t>(arrival_ms));
  }

  return usage;
}

double GccController::ComputeTrendlineSlope() const {
  if (trendline_window_.size() < 2) {
    return prev_trend_;
  }
  // Least-squares slope of (arrival_time_ms, smoothed_delay), centered.
  double sum_x = 0, sum_y = 0;
  for (const auto& p : trendline_window_) {
    sum_x += p.arrival_time_ms;
    sum_y += p.smoothed_delay;
  }
  double x_avg = sum_x / trendline_window_.size();
  double y_avg = sum_y / trendline_window_.size();
  double num = 0, den = 0;
  for (const auto& p : trendline_window_) {
    double dx = p.arrival_time_ms - x_avg;
    num += dx * (p.smoothed_delay - y_avg);
    den += dx * dx;
  }
  if (den == 0) return prev_trend_;
  return num / den;
}

// --- Overuse detector (faithful to TrendlineEstimator::Detect) ---

GccController::BandwidthUsage GccController::Detect(double trend,
                                                   double ts_delta_ms,
                                                   int64_t now_ms) {
  if (num_deltas_ < 2) {
    return BandwidthUsage::kNormal;
  }

  // modified_trend = min(num_deltas, kMinNumDeltas) * trend * gain
  const double modified_trend =
      std::min(num_deltas_, kMinNumDeltas) * trend * kThresholdGain;

  BandwidthUsage result = BandwidthUsage::kNormal;

  if (modified_trend > adaptive_threshold_) {
    if (time_over_using_ms_ < 0) {
      // Initialize: assume over-using half the time since the previous sample.
      time_over_using_ms_ = ts_delta_ms / 2.0;
    } else {
      time_over_using_ms_ += ts_delta_ms;
    }
    overuse_counter_++;
    if (time_over_using_ms_ > kOverusingTimeThresholdMs &&
        overuse_counter_ > 1) {
      if (trend >= prev_trend_) {
        time_over_using_ms_ = 0;
        overuse_counter_ = 0;
        result = BandwidthUsage::kOveruse;
      }
    }
  } else if (modified_trend < -adaptive_threshold_) {
    time_over_using_ms_ = -1;
    overuse_counter_ = 0;
    result = BandwidthUsage::kUnderuse;
  } else {
    time_over_using_ms_ = -1;
    overuse_counter_ = 0;
    result = BandwidthUsage::kNormal;
  }

  prev_trend_ = trend;
  UpdateAdaptiveThreshold(modified_trend, now_ms);
  return result;
}

void GccController::UpdateAdaptiveThreshold(double modified_trend,
                                            int64_t now_ms) {
  if (last_threshold_update_ms_ == 0) {
    last_threshold_update_ms_ = now_ms;
  }
  // Avoid adapting the threshold to big spikes (e.g. sudden capacity drop).
  if (std::abs(modified_trend) > adaptive_threshold_ + kMaxAdaptOffsetMs) {
    last_threshold_update_ms_ = now_ms;
    return;
  }
  double k = std::abs(modified_trend) < adaptive_threshold_ ? kThresholdDown
                                                            : kThresholdUp;
  const int64_t kMaxTimeDeltaMs = 100;
  int64_t time_delta_ms =
      std::min(now_ms - last_threshold_update_ms_, kMaxTimeDeltaMs);
  if (time_delta_ms < 0) time_delta_ms = 0;
  adaptive_threshold_ +=
      k * (std::abs(modified_trend) - adaptive_threshold_) * time_delta_ms;
  adaptive_threshold_ = std::clamp(adaptive_threshold_, kMinThreshold, kMaxThreshold);
  last_threshold_update_ms_ = now_ms;
}

// --- Rate controller (WebRTC: AIMD) ---

void GccController::UpdateDelayBasedRate(BandwidthUsage usage, int64_t now_ms) {
  switch (usage) {
    case BandwidthUsage::kOveruse: {
      // WebRTC AimdRateControl decrease: snap to beta * measured throughput
      // (not beta * current estimate). This drains the self-induced queue and
      // lands the estimate right at ~capacity instead of just shaving the
      // (possibly inflated) current value.
      int target = delay_based_bitrate_kbps_;
      if (acked_bitrate_kbps_ > 0.0) {
        int from_acked = static_cast<int>(acked_bitrate_kbps_ * kMultiplicativeDecrease);
        // Only decrease (never raise on overuse).
        target = std::min(delay_based_bitrate_kbps_, from_acked);
      } else {
        target = static_cast<int>(delay_based_bitrate_kbps_ * kMultiplicativeDecrease);
      }
      delay_based_bitrate_kbps_ = std::max(target, min_bitrate_kbps_);
      overuse_counter_ = 0;
      last_overuse_time_ms_ = now_ms;
      LOG(INFO) << "[GCC] Overuse → decrease to " << delay_based_bitrate_kbps_
                << " kbps (acked=" << static_cast<int>(acked_bitrate_kbps_) << ")";
      break;
    }
    case BandwidthUsage::kUnderuse:
    case BandwidthUsage::kNormal: {
      // Additive increase: ~8% of current rate per second.
      int64_t time_since_last_ms = now_ms - last_increase_time_ms_;
      if (time_since_last_ms <= 0) break;

      // Don't increase too soon after overuse (WebRTC: wait ~1s).
      if (now_ms - last_overuse_time_ms_ < 1000) break;

      double seconds = time_since_last_ms / 1000.0;
      int increase_kbps = static_cast<int>(
          delay_based_bitrate_kbps_ * kIncreaseRatePerSecond * seconds);
      increase_kbps = std::max(increase_kbps, static_cast<int>(kMinIncreaseKbps * seconds));

      int new_rate = delay_based_bitrate_kbps_ + increase_kbps;

      // WebRTC AimdRateControl: never increase beyond 1.5x the acknowledged
      // throughput (+ a small slack). This is what stops the estimate from
      // ramping far past a saturated link — the acked rate plateaus at
      // capacity, capping the increase near capacity instead of running away.
      if (acked_bitrate_kbps_ > 0.0) {
        int increase_limit = static_cast<int>(1.5 * acked_bitrate_kbps_) + 10;
        new_rate = std::min(new_rate, increase_limit);
        // Don't let the cap force a decrease here.
        new_rate = std::max(new_rate, delay_based_bitrate_kbps_);
      }

      delay_based_bitrate_kbps_ = std::min(new_rate, max_bitrate_kbps_);
      last_increase_time_ms_ = now_ms;
      break;
    }
  }
}

// --- Loss-based rate control (WebRTC: send_side_bandwidth_estimation.cc) ---
//
// WebRTC caps the loss-based target at the delay-based estimate
// (GetUpperLimit() == delay_based_limit_). With no loss the loss-based value
// rises 8%/interval but is capped at the delay-based estimate, so the two
// converge and coincide (~99.7% identical in our SparkRTC reference run).
// We model that end-state directly: when loss is low the loss-based estimate
// tracks the delay-based estimate; only sustained loss pulls it below.
void GccController::UpdateLossBasedRate(double loss_fraction) {
  if (loss_fraction < kLossIncreaseThreshold) {
    // < 2% loss: track the delay-based estimate (WebRTC's delay-based cap with
    // no loss → loss_based == delay_based).
    loss_based_bitrate_kbps_ = delay_based_bitrate_kbps_;
    loss_based_bitrate_kbps_ = std::clamp(loss_based_bitrate_kbps_,
                                          min_bitrate_kbps_, max_bitrate_kbps_);
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
