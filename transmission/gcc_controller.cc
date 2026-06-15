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
      delay_based_bitrate_kbps_(1000),
      last_increase_time_ms_(0),
      loss_based_bitrate_kbps_(30000),
      packets_sent_since_last_loss_update_(0),
      packets_lost_since_last_loss_update_(0),
      target_bitrate_kbps_(1000),
      min_bitrate_kbps_(100),
      max_bitrate_kbps_(30000),
      fake_clock_ms_(nullptr) {
  last_threshold_update_ms_ = NowMs();
  last_increase_time_ms_ = NowMs();
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

void GccController::OnTransportFeedback(const TransportFeedback& feedback) {
  std::lock_guard<std::mutex> lock(mutex_);
  int64_t now_ms = NowMs();

  UpdateTrendline(feedback);
  double slope = ComputeTrendlineSlope();
  BandwidthUsage usage = Detect(slope);
  UpdateDelayBasedRate(usage, now_ms);

  // Feed signals to prober
  if (usage == BandwidthUsage::kOveruse) {
    prober_.OnOveruseDetected();
  }
  prober_.SetEstimatedBitrate(delay_based_bitrate_kbps_);

  target_bitrate_kbps_ = ComputeFinalBitrate();
}

void GccController::OnLossReport(const LossReport& report) {
  std::lock_guard<std::mutex> lock(mutex_);

  int lost = static_cast<int>(report.packets.size());
  packets_lost_since_last_loss_update_ += lost;

  // Update loss-based rate when we have enough data
  if (packets_sent_since_last_loss_update_ >= kLossUpdateWindowSize) {
    double loss_fraction = static_cast<double>(packets_lost_since_last_loss_update_) /
        packets_sent_since_last_loss_update_;
    UpdateLossBasedRate(loss_fraction);
    packets_sent_since_last_loss_update_ = 0;
    packets_lost_since_last_loss_update_ = 0;
  }

  target_bitrate_kbps_ = ComputeFinalBitrate();
}

void GccController::OnPacketsSent(int count) {
  std::lock_guard<std::mutex> lock(mutex_);
  packets_sent_since_last_loss_update_ += count;
}

void GccController::SetClockForTesting(int64_t* clock_ms) {
  fake_clock_ms_ = clock_ms;
  if (clock_ms) {
    last_threshold_update_ms_ = *clock_ms;
    last_increase_time_ms_ = *clock_ms;
    last_overuse_time_ms_ = *clock_ms - 2000;  // allow immediate probing in tests
  }
}

// --- Trendline estimator (WebRTC: trendline_estimator.cc) ---

void GccController::UpdateTrendline(const TransportFeedback& feedback) {
  if (feedback.packets.size() < 2) {
    return;
  }

  // Use the batch as a single delay sample
  int64_t batch_arrival_ms = feedback.packets.back().arrival_time_us / 1000;
  int64_t batch_send_time_ms = feedback.reference_time_us / 1000;

  if (first_arrival_ms_ == 0) {
    first_arrival_ms_ = batch_arrival_ms;
    prev_arrival_time_ms_ = batch_arrival_ms;
    prev_send_time_ms_ = batch_send_time_ms;
    return;
  }

  // Inter-group deltas
  double arrival_delta = static_cast<double>(batch_arrival_ms - prev_arrival_time_ms_);
  double send_delta = static_cast<double>(batch_send_time_ms - prev_send_time_ms_);

  // One-way delay variation for this group
  double delay_delta = arrival_delta - send_delta;

  prev_arrival_time_ms_ = batch_arrival_ms;
  prev_send_time_ms_ = batch_send_time_ms;

  // Accumulate delay and smooth it (WebRTC: smoothing_coef_ = 0.9)
  accumulated_delay_ += delay_delta;
  smoothed_delay_ = kTrendlineSmoothingCoeff * smoothed_delay_ +
                    (1.0 - kTrendlineSmoothingCoeff) * accumulated_delay_;

  // Add to trendline window
  double time_since_first = static_cast<double>(batch_arrival_ms - first_arrival_ms_);
  trendline_window_.push_back({smoothed_delay_, time_since_first});
  if (static_cast<int>(trendline_window_.size()) > kTrendlineWindowSize) {
    trendline_window_.pop_front();
  }
  num_deltas_++;
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

  if (modified_trend > adaptive_threshold_) {
    overuse_counter_++;
    UpdateAdaptiveThreshold(modified_trend, now_ms);
    if (overuse_counter_ >= kOveruseCountThreshold) {
      return BandwidthUsage::kOveruse;
    }
    return BandwidthUsage::kNormal;
  } else if (modified_trend < -adaptive_threshold_) {
    overuse_counter_ = 0;
    UpdateAdaptiveThreshold(modified_trend, now_ms);
    return BandwidthUsage::kUnderuse;
  } else {
    overuse_counter_ = 0;
    UpdateAdaptiveThreshold(modified_trend, now_ms);
    return BandwidthUsage::kNormal;
  }
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
    // < 2% loss: can increase (additive, same rate as delay-based)
    int increase_kbps = std::max(
        static_cast<int>(loss_based_bitrate_kbps_ * kIncreaseRatePerSecond * 0.2),
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
  // Only let prober override when actively probing
  auto& prober_ref = const_cast<BandwidthProber&>(prober_);
  if (prober_ref.GetState() != BandwidthProber::State::kIdle) {
    int probe_kbps = prober_ref.GetEffectiveBitrateKbps();
    if (probe_kbps > base_kbps) {
      base_kbps = probe_kbps;
    }
  }
  return std::clamp(base_kbps, min_bitrate_kbps_, max_bitrate_kbps_);
}
