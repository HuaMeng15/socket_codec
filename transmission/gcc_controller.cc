#include "gcc_controller.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <memory>
#include <numeric>

#include "log_system/log_system.h"
#include "packet_header.h"

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
      first_arrival_set_(false),
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
      last_loss_fraction_(0.0),
      target_bitrate_kbps_(1000),
      min_bitrate_kbps_(100),
      max_bitrate_kbps_(30000),
      periodic_alr_probing_enabled_(true),
      probe_active_(false),
      probe_floor_kbps_(0),
      probe_measurement_send_cutoff_us_(0),
      probe_first_arrival_us_(0),
      probe_last_arrival_us_(0),
      probe_recv_bytes_(0.0),
      probe_first_seen_(false),
      last_received_rate_kbps_(0.0),
      fake_clock_ms_(nullptr) {
  last_threshold_update_ms_ = NowMs();
  last_increase_time_ms_ = NowMs();
  last_loss_update_ms_ = NowMs();
  // Enable congestion-window pushback with WebRTC's default field-trial values
  // (QueueSize:350, MinBitrate:30000). This is on by default in SparkRTC.
  pushback_ = std::make_unique<CongestionWindowPushbackController>(
      static_cast<uint32_t>(cwnd_min_bitrate_kbps_) * 1000u);
}

void GccController::SetInitialBitrate(int kbps) {
  std::lock_guard<std::mutex> lock(mutex_);
  delay_based_bitrate_kbps_ = kbps;
  loss_based_bitrate_kbps_ = kbps;
  target_bitrate_kbps_ = kbps;
  prober_.SetEstimatedBitrate(kbps);
}

void GccController::SetCongestionWindowConfig(int queue_size_ms,
                                              int min_bitrate_kbps) {
  std::lock_guard<std::mutex> lock(mutex_);
  cwnd_queue_size_ms_ = queue_size_ms;
  cwnd_min_bitrate_kbps_ = min_bitrate_kbps;
  if (queue_size_ms > 0) {
    pushback_ = std::make_unique<CongestionWindowPushbackController>(
        static_cast<uint32_t>(min_bitrate_kbps) * 1000u);
  } else {
    pushback_.reset();  // disabled
  }
  current_data_window_bytes_ = -1;
}

void GccController::SetPeriodicAlrProbingEnabled(bool enabled) {
  std::lock_guard<std::mutex> lock(mutex_);
  periodic_alr_probing_enabled_ = enabled;
  prober_.SetAlrProbingEnabled(enabled);
  LOG(INFO) << "[GCC] Periodic ALR probing "
            << (enabled ? "enabled" : "disabled");
}

void GccController::SetUnconditionalPeriodicProbeIntervalMs(int interval_ms) {
  std::lock_guard<std::mutex> lock(mutex_);
  unconditional_periodic_probe_interval_ms_ = std::max(0, interval_ms);
  prober_.SetUnconditionalPeriodicProbeIntervalMs(
      unconditional_periodic_probe_interval_ms_);
  LOG(INFO) << "[GCC] Congestion-aware periodic probing interval="
            << unconditional_periodic_probe_interval_ms_ << " ms";
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

bool GccController::IsProbing() const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto s = prober_.GetState();
  return s == BandwidthProber::State::kProbing ||
         s == BandwidthProber::State::kWaitingForResult;
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

double GccController::GetAckedBitrateKbpsForTesting() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return acked_bitrate_kbps_;
}

int64_t GccController::GetOutstandingBytesForTesting() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return outstanding_bytes_;
}

void GccController::EnableAckedEstimatorForTesting() {
  std::lock_guard<std::mutex> lock(mutex_);
  acked_frozen_for_testing_ = false;
  acked_bitrate_kbps_ = 0.0;
  acked_window_.clear();
  acked_window_bytes_ = 0;
  instant_acked_sample_valid_ = false;
  instant_sent_bitrate_kbps_ = 0.0;
  instant_delivery_ratio_ = 1.0;
}

double GccController::GetNetworkUsageState() const {
  std::lock_guard<std::mutex> lock(mutex_);
  // Return a value representing overuse aggressiveness for encoder adaptation.
  // Similar to sparkrtc's aggressive_state which returns the accumulated
  // time_over_using normalized by the detection threshold.
  //
  // < 2.0: normal or underuse (encoder uses relaxed VBV = bitrate/2)
  // >= 2.0: overuse detected (encoder tightens VBV = bitrate/fps for fast adapt)
  //
  // We use a lower threshold (kEncoderOveruseThresholdMs = 5ms) than the full
  // overuse detector (kOverusingTimeThresholdMs = 10ms) so the encoder reacts
  // earlier, aligned with sparkrtc's early notification strategy.

  if (last_bandwidth_usage_ == BandwidthUsage::kUnderuse) {
    return 0.0;  // underuse: fully relaxed
  }

  // When overuse is detected, time_over_using_ms_ is reset to 0 as part of the
  // state machine, but we still want the encoder to react aggressively. Return
  // a value >= 2.0 to trigger tight VBV mode immediately.
  if (last_bandwidth_usage_ == BandwidthUsage::kOveruse) {
    return 2.0;  // overuse: trigger aggressive mode
  }

  // During normal or accumulation phase, return the ratio. If time_over_using_ms_
  // < 0, the detector is not accumulating yet, so return 0.
  if (time_over_using_ms_ < 0) {
    return 0.0;
  }

  // Return the ratio of accumulated over-threshold time to the encoder threshold.
  // When this exceeds 2.0, the encoder should enter aggressive VBV mode.
  double aggressive_ratio = time_over_using_ms_ / kEncoderOveruseThresholdMs;
  return aggressive_ratio;
}

void GccController::OnTransportFeedback(const TransportFeedback& feedback) {
  std::lock_guard<std::mutex> lock(mutex_);
  int64_t now_ms = NowMs();

  RetireFeedbackPackets(feedback);
  ExpireStaleInflightPackets(now_ms);

  // --- Congestion-window inputs: RTT and acked (in-flight) bytes ----------
  // RTT sample per acked packet = feedback-receipt time (now, sender clock) -
  // packet send time (sender clock). Both are steady_clock us. We take the
  // batch MAX (WebRTC's feedback_max_rtt) and keep a sliding window; the
  // window MIN is used to size the congestion window (robust to transient
  // spikes). Packet-level ACK retirement above supplies the outstanding
  // estimate; recv_size remains the source for acknowledged-throughput.
  int64_t batch_max_rtt_ms = -1;
  int64_t now_us = now_ms * 1000;
  for (const auto& pkt : feedback.packets) {
    if (pkt.send_time_us >= 0) {
      int64_t rtt_ms = (now_us - pkt.send_time_us) / 1000;
      if (rtt_ms > batch_max_rtt_ms) batch_max_rtt_ms = rtt_ms;
    }
  }
  if (batch_max_rtt_ms >= 0) {
    last_rtt_ms_ = batch_max_rtt_ms;
    feedback_max_rtts_.emplace_back(now_ms, batch_max_rtt_ms);
    while (feedback_max_rtts_.size() > 1 &&
           (now_ms - feedback_max_rtts_.front().first > kRttWindowMs ||
            feedback_max_rtts_.size() >
                static_cast<size_t>(kMaxRttWindowSamples))) {
      feedback_max_rtts_.pop_front();
    }
  }

  BandwidthUsage usage = UpdateTrendline(feedback);
  UpdateAckedBitrate(feedback);
  usage = ApplyByteDeliverySignal(usage, now_ms);
  if (usage == BandwidthUsage::kOveruse &&
      ShouldIgnoreSourceLimitedOveruse(now_ms)) {
    usage = BandwidthUsage::kNormal;
    last_bandwidth_usage_ = usage;
  }
  UpdateDelayBasedRate(usage, now_ms);

  // Periodically re-evaluate the loss-based estimate. Without this, loss_based
  // only ever changed on a loss report and stayed frozen during loss-free
  // operation — capping the target below the true capacity.
  MaybeUpdateLossRate(now_ms);

  // A codec's frame/slice burst shape can produce a small trend-only overuse
  // indication even when byte delivery and the congestion window are healthy.
  // Continue applying the ordinary AIMD correction, but do not let that
  // tentative indication terminate startup exploration for one codec earlier
  // than the other. Startup exploration must not be terminated by accumulated
  // delay alone:
  // that signal is intentionally sensitive to packet timing and therefore to
  // frame-vs-slice burst shape. The measured probe result already detects
  // saturation (received rate below target), while a byte-delivery cliff or a
  // loss collapse independently confirms real congestion. Congestion-window
  // pushback is also byte/RTT based and therefore codec-independent; once it
  // is active, startup discovery must end rather than surviving into the
  // post-drop drain. Periodic probing remains guarded by delay/cwnd state below
  // because it is optional recovery, not initial capacity discovery.
  bool confirmed_probe_congestion =
      byte_delivery_overuse_ || congestion_episode_active_ ||
      pushback_ratio_ < 0.999;

  // Periodic probing is more conservative than startup: ALR is necessary but
  // not sufficient. Do not start (or continue) a periodic probe while cwnd
  // pushback is active, outstanding data exceeds its measured window, or the
  // delay estimator currently reports overuse/queue growth. These are all
  // adaptive controller state; there is no fixed bitrate ceiling.
  bool cwnd_healthy = pushback_ratio_ >= 0.999 &&
      (current_data_window_bytes_ <= 0 ||
       outstanding_bytes_ <= current_data_window_bytes_);
  // accumulated_delay_ is an integrated variation signal, not an absolute
  // queue measurement. It can retain a positive offset after a transient even
  // when RTT is low, no bytes are outstanding, and the trend is falling. Treat
  // that history as unsafe only while it is both elevated and still growing;
  // otherwise the live loss/byte/cwnd checks below decide congestion health.
  bool delay_healthy = usage != BandwidthUsage::kOveruse &&
      (accumulated_delay_ < kProbeAbortDelayMs || prev_trend_ <= 0.0);
  bool periodic_probe_healthy = cwnd_healthy && delay_healthy &&
      !byte_delivery_overuse_ && !congestion_episode_active_ &&
      last_loss_fraction_ < kLossIncreaseThreshold &&
      now_ms - last_overuse_time_ms_ >= kPeriodicProbeCongestionCooldownMs;
  prober_.SetPeriodicProbingAllowed(periodic_probe_healthy);

  // Feed confirmed signals to the probe controller. Underuse remains useful
  // for state bookkeeping, although it does not itself arm a probe.
  if (confirmed_probe_congestion) {
    prober_.OnOveruseDetected();
  } else if (usage == BandwidthUsage::kUnderuse) {
    // Queue draining — permits drop-recovery probing (see BandwidthProber).
    prober_.OnUnderuseDetected();
  }
  prober_.SetEstimatedBitrate(delay_based_bitrate_kbps_);
  // Keep the ALR budget accruing against the current delay-based estimate (not
  // the transient probe target), so "sending below capacity" is judged against
  // what the link can actually carry.
  alr_detector_.SetEstimatedBitrate(delay_based_bitrate_kbps_);

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
    // New probe just started — snapshot the floor, reset the delay integrator
    // and the probe-measurement accumulators. We do NOT commit yet: the probe
    // traffic hasn't been acknowledged, and we need a real arrival span first.
    probe_active_ = true;
    probe_floor_kbps_ = loss_based_bitrate_kbps_;
    accumulated_delay_ = 0.0;
    smoothed_delay_ = 0.0;
    probe_first_seen_ = false;
    probe_recv_bytes_ = 0.0;
    probe_first_arrival_us_ = 0;
    probe_last_arrival_us_ = 0;
    // This feedback was already in flight when the controller observed the
    // newly active probe. Its rate therefore reflects normal codec output,
    // not the elevated probe. Start measuring strictly after its newest send
    // timestamp so frame- and slice-shaped traffic get the same capacity test.
    probe_measurement_send_cutoff_us_ = 0;
    for (const auto& pkt : feedback.packets) {
      probe_measurement_send_cutoff_us_ = std::max(
          probe_measurement_send_cutoff_us_, pkt.send_time_us);
    }
  }
  if (probing && probe_active_) {
    // Accumulate this batch's probe traffic into the measurement. Exclude the
    // very first packet (rate is measured over the span between arrivals).
    for (const auto& pkt : feedback.packets) {
      // Padding is explicit probe traffic. It intentionally has no send-time
      // entry (the delay trendline must ignore it), so identify it by its
      // reserved frame sequence and include it directly. Ordinary media is
      // included only when sent after the activation boundary.
      bool probe_padding = pkt.frame_sequence == kPaddingFrameSequence;
      if (pkt.recv_size == 0 ||
          (!probe_padding &&
           pkt.send_time_us <= probe_measurement_send_cutoff_us_)) {
        continue;
      }
      if (!probe_first_seen_) {
        probe_first_seen_ = true;
        probe_first_arrival_us_ = pkt.arrival_time_us;
        probe_last_arrival_us_ = pkt.arrival_time_us;
        continue;  // first packet: starts the span, contributes no bytes
      }
      probe_recv_bytes_ += pkt.recv_size;
      probe_last_arrival_us_ = pkt.arrival_time_us;
    }

    // Abort if the elevated rate already induced congestion.
    bool congested = confirmed_probe_congestion ||
                     (loss_based_bitrate_kbps_ < probe_floor_kbps_ / 2);
    int64_t span_us = probe_last_arrival_us_ - probe_first_arrival_us_;
    if (congested) {
      prober_.OnProbeResult(delay_based_bitrate_kbps_, /*success=*/false);
      probe_active_ = false;
    } else if (probe_first_seen_ && span_us >= kProbeMinSpanUs) {
      // Enough accumulated span for a trustworthy measurement. The probe's
      // received rate over the span is the throughput the link sustained at the
      // elevated send rate. A min span guards against bunched arrivals reporting
      // a rate far above real capacity.
      double measured_kbps = probe_recv_bytes_ * 8.0 / 1000.0 / (span_us / 1e6);
      last_received_rate_kbps_ = measured_kbps;
      int probe_target = prober_.GetEffectiveBitrateKbps();
      int received = static_cast<int>(measured_kbps);
      // WebRTC commits min(send_rate, receive_rate); the probe target bounds the
      // send rate. When the link is saturated (received below the probe target),
      // back off to kProbeUtilizationFraction × received to avoid overuse.
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
    // else: not enough span yet — keep accumulating across the next batches.
  } else if (!probing) {
    probe_active_ = false;
  }

  // Keep loss_based aligned with the (possibly just-committed) delay_based under
  // low loss, so ComputeFinalBitrate isn't dragged down by a stale value.
  MaybeSyncLossToDelay();
  int base_kbps = ComputeFinalBitrate();

  // --- Congestion-window pushback (WebRTC goog_cc) ------------------------
  // Recompute the window from the loss-based rate and min feedback RTT, feed
  // in the current in-flight bytes, and let the pushback ratchet adjust the
  // target. This is the piece that crashes the target to the floor within
  // ~1 RTT under a capacity drop and holds it until the backlog drains — with
  // no packet loss required. Applied to the target directly (DropFrame:false).
  if (pushback_) {
    UpdateCongestionWindowSize();
    if (current_data_window_bytes_ > 0) {
      pushback_->SetDataWindow(current_data_window_bytes_);
    }
    pushback_->UpdateOutstandingData(outstanding_bytes_);
    uint32_t pushed_bps = pushback_->UpdateTargetBitrate(
        static_cast<uint32_t>(base_kbps) * 1000u, now_ms);
    pushback_ratio_ = pushback_->encoding_rate_ratio();
    // Pushback deliberately drives the target BELOW the operational min during
    // a drain (down to its own floor, cwnd_min_bitrate_kbps_), so we do NOT
    // re-apply min_bitrate_kbps_ here — that would block the drain. The
    // controller already guarantees result >= min(estimate, pushback floor),
    // mirroring WebRTC's max(GetMinBitrate()=5kbps, pushback_rate).
    base_kbps = std::max<int>(cwnd_min_bitrate_kbps_,
                              static_cast<int>(pushed_bps / 1000));
  }
  target_bitrate_kbps_ = base_kbps;

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
  bool is_probing = prober_.GetState() == BandwidthProber::State::kProbing ||
                    prober_.GetState() == BandwidthProber::State::kWaitingForResult;
  LOG(INFO) << "[GCC_STATE] target=" << target_bitrate_kbps_
            << " delay_based=" << delay_based_bitrate_kbps_
            << " loss_based=" << loss_based_bitrate_kbps_
            << " acked_kbps=" << acked_bitrate_kbps_
            << " slope=" << prev_trend_
            << " threshold=" << adaptive_threshold_
            << " usage=" << usage_str
            << " overuse_counter=" << overuse_counter_
            << " loss_fraction=" << last_loss_fraction_
            << " queuing_delay_ms=" << queuing_delay_ms
            << " smoothed_delay_ms=" << smoothed_delay_
            << " rtt_ms=" << last_rtt_ms_
            << " outstanding_bytes=" << outstanding_bytes_
            << " data_window=" << current_data_window_bytes_
            << " pushback_ratio=" << pushback_ratio_
            << " received_rate_kbps=" << last_received_rate_kbps_
            << " sent_rate_kbps=" << sent_rate_kbps_
            << " aligned_sent_kbps=" << aligned_sent_bitrate_kbps_
            << " delivery_ratio=" << aligned_delivery_ratio_
            << " byte_overuse=" << (byte_delivery_overuse_ ? 1 : 0)
            << " congestion_episode="
            << (congestion_episode_active_ ? 1 : 0)
            << " instant_acked=" << (use_instant_acked_ ? 1 : 0)
            << " delay_probe_healthy=" << (delay_healthy ? 1 : 0)
            << " periodic_probe_healthy=" << (periodic_probe_healthy ? 1 : 0)
            << " probing=" << (is_probing ? 1 : 0);
}

void GccController::OnLossReport(const LossReport& report) {
  std::lock_guard<std::mutex> lock(mutex_);

  RetireLostPackets(report);
  ExpireStaleInflightPackets(NowMs());

  // Just accumulate lost packets; the periodic MaybeUpdateLossRate (driven
  // from the feedback path) recomputes the loss fraction over the window.
  packets_lost_since_last_loss_update_ += static_cast<int>(report.packets.size());

  MaybeUpdateLossRate(NowMs());
  MaybeSyncLossToDelay();
  target_bitrate_kbps_ = ComputeFinalBitrate();
}

void GccController::OnPacketsSent(int count) {
  std::lock_guard<std::mutex> lock(mutex_);
  packets_sent_since_last_loss_update_ += count;
}

void GccController::OnBytesSent(size_t bytes_sent) {
  std::lock_guard<std::mutex> lock(mutex_);
  int64_t now_ms = NowMs();
  alr_detector_.OnBytesSent(now_ms, bytes_sent);

  sent_rate_window_.push_back(
      {now_ms, static_cast<int64_t>(bytes_sent)});
  sent_rate_window_bytes_ += static_cast<int64_t>(bytes_sent);
  while (sent_rate_window_.size() > 1 &&
         now_ms - sent_rate_window_.front().time_ms > kSentRateWindowMs) {
    sent_rate_window_bytes_ -= sent_rate_window_.front().bytes;
    sent_rate_window_.pop_front();
  }
  if (sent_rate_window_.size() >= 2) {
    int64_t span_ms = now_ms - sent_rate_window_.front().time_ms;
    if (span_ms > 0) {
      int64_t bytes_over_span =
          sent_rate_window_bytes_ - sent_rate_window_.front().bytes;
      sent_rate_kbps_ = static_cast<double>(bytes_over_span) * 8.0 / span_ms;
    }
  }
  // Report the current ALR level on every batch. This must not be a latched
  // event: otherwise a short codec-dependent VBR lull can arm a probe that
  // fires much later after the source has resumed normal production.
  prober_.SetApplicationLimited(alr_detector_.InAlr());
}

void GccController::OnPacketSent(uint16_t frame_sequence,
                                 uint16_t packet_index,
                                 size_t wire_bytes) {
  if (wire_bytes == 0) return;
  std::lock_guard<std::mutex> lock(mutex_);
  const int64_t now_ms = NowMs();
  ExpireStaleInflightPackets(now_ms);

  const uint32_t key = (static_cast<uint32_t>(frame_sequence) << 16) |
                       packet_index;
  const uint64_t id = next_inflight_id_++;
  const int64_t bytes = static_cast<int64_t>(wire_bytes);
  inflight_history_.push_back({id, key, now_ms, bytes});
  inflight_ids_by_key_[key].push_back(id);
  active_inflight_bytes_[id] = bytes;
  outstanding_bytes_ += bytes;
}

bool GccController::RetireInflightPacket(uint16_t frame_sequence,
                                         uint16_t packet_index) {
  const uint32_t key = (static_cast<uint32_t>(frame_sequence) << 16) |
                       packet_index;
  auto ids_it = inflight_ids_by_key_.find(key);
  if (ids_it == inflight_ids_by_key_.end()) return false;

  auto& ids = ids_it->second;
  while (!ids.empty() && !active_inflight_bytes_.contains(ids.front())) {
    ids.pop_front();
  }
  if (ids.empty()) {
    inflight_ids_by_key_.erase(ids_it);
    return false;
  }

  const uint64_t id = ids.front();
  ids.pop_front();
  if (ids.empty()) inflight_ids_by_key_.erase(ids_it);
  auto active_it = active_inflight_bytes_.find(id);
  if (active_it == active_inflight_bytes_.end()) return false;
  outstanding_bytes_ -= active_it->second;
  active_inflight_bytes_.erase(active_it);
  return true;
}

void GccController::RetireFeedbackPackets(
    const TransportFeedback& feedback) {
  for (const auto& packet : feedback.packets) {
    RetireInflightPacket(packet.frame_sequence, packet.packet_index);
  }
}

void GccController::RetireLostPackets(const LossReport& report) {
  for (const auto& packet : report.packets) {
    RetireInflightPacket(packet.frame_sequence, packet.packet_index);
  }
}

void GccController::ExpireStaleInflightPackets(int64_t now_ms) {
  const int64_t timeout_ms = std::max(
      kMinInflightTimeoutMs, kInflightTimeoutRttMultiplier * last_rtt_ms_);
  while (!inflight_history_.empty() &&
         now_ms - inflight_history_.front().sent_time_ms >= timeout_ms) {
    const InflightPacket packet = inflight_history_.front();
    inflight_history_.pop_front();
    auto active_it = active_inflight_bytes_.find(packet.id);
    if (active_it == active_inflight_bytes_.end()) continue;

    outstanding_bytes_ -= active_it->second;
    active_inflight_bytes_.erase(active_it);
    auto ids_it = inflight_ids_by_key_.find(packet.key);
    if (ids_it != inflight_ids_by_key_.end()) {
      auto& ids = ids_it->second;
      while (!ids.empty() && !active_inflight_bytes_.contains(ids.front())) {
        ids.pop_front();
      }
      if (ids.empty()) inflight_ids_by_key_.erase(ids_it);
    }
  }
  if (outstanding_bytes_ < 0) outstanding_bytes_ = 0;
}

GccController::BandwidthUsage GccController::ApplyByteDeliverySignal(
    BandwidthUsage delay_usage, int64_t now_ms) {
  byte_delivery_overuse_ = false;
  int64_t sent_span_ms = sent_rate_window_.size() >= 2
      ? now_ms - sent_rate_window_.front().time_ms
      : 0;
  int64_t acked_span_us = acked_window_.size() >= 2
      ? acked_window_.back().arrival_us - acked_window_.front().arrival_us
      : 0;
  bool estimator_ready = !acked_frozen_for_testing_ &&
                         sent_span_ms >= kByteEstimatorMinSpanMs &&
                         acked_span_us >= kByteEstimatorMinSpanMs * 1000 &&
                         aligned_sent_bitrate_kbps_ > 0.0 &&
                         acked_bitrate_kbps_ > 0.0;
  bool sender_saturated =
      sent_rate_kbps_ >= kSenderUtilizationThreshold *
                             delay_based_bitrate_kbps_;
  // Compare rates over the same acknowledged packet cohort. This avoids
  // judging a frame burst in one source window against a different receiver
  // window. Cap at the controller target so a short probe/pacer burst above
  // the estimate cannot create a shortage by itself.
  double delivery_reference_kbps =
      std::min(aligned_sent_bitrate_kbps_,
               static_cast<double>(delay_based_bitrate_kbps_));
  double delivery_ratio = estimator_ready && delivery_reference_kbps > 0.0
      ? acked_bitrate_kbps_ / delivery_reference_kbps
      : 1.0;
  aligned_delivery_ratio_ = delivery_ratio;

  // A large new capacity cliff is a distinct congestion event even if an
  // earlier, mild overuse episode is still draining. Check it before the
  // one-reduction-per-episode guard; otherwise that old episode can suppress
  // the only reduction that reflects the new link capacity.
  bool severe_capacity_cliff =
      sent_span_ms >= 50 && sent_rate_kbps_ > 0.0 &&
      instant_acked_sample_valid_ && instant_sent_bitrate_kbps_ > 0.0 &&
      instant_delivery_ratio_ < kSevereDeliveryRatioThreshold &&
      last_modified_trend_ > adaptive_threshold_ &&
      accumulated_delay_ >= kSevereQueueGrowthMs;
  if (severe_capacity_cliff && !severe_capacity_cliff_active_) {
    bool superseded_episode = congestion_episode_active_;
    // Let UpdateDelayBasedRate apply one immediate reduction from the newly
    // measured capacity. It will latch a fresh episode for the new backlog.
    congestion_episode_active_ = false;
    congestion_recovery_start_ms_ = -1;
    last_suppressed_overuse_log_ms_ = -1;
    severe_capacity_cliff_active_ = true;
    low_delivery_start_ms_ = -1;
    byte_delivery_overuse_ = true;
    last_bandwidth_usage_ = BandwidthUsage::kOveruse;
    LOG(INFO) << "[GCC] Severe capacity cliff"
              << (superseded_episode ? " superseded prior episode:" : ":")
              << " delivered=" << static_cast<int>(acked_bitrate_kbps_)
              << " kbps sent=" << static_cast<int>(sent_rate_kbps_)
              << " kbps reference="
              << static_cast<int>(instant_sent_bitrate_kbps_)
              << " kbps ratio=" << instant_delivery_ratio_
              << " accumulated_delay_ms=" << accumulated_delay_
              << " modified_trend=" << last_modified_trend_
              << " threshold=" << adaptive_threshold_;
    return BandwidthUsage::kOveruse;
  }

  // A congestion episode owns at most one permanent AIMD decrease. While its
  // backlog remains, congestion-window pushback can still lower the effective
  // send target. Rearm only after delivery, delay, loss, and outstanding data
  // have all remained healthy for a continuous hysteresis interval.
  if (congestion_episode_active_) {
    bool cwnd_recovered = pushback_ratio_ >= 0.999 &&
        (current_data_window_bytes_ <= 0 ||
         outstanding_bytes_ * 2 <= current_data_window_bytes_);
    bool delivery_recovered =
        (estimator_ready &&
         delivery_ratio >= kByteDeliveryRecoveryRatio) ||
        (acked_frozen_for_testing_ && acked_bitrate_kbps_ >=
             kByteDeliveryRecoveryRatio * delay_based_bitrate_kbps_);
    double outstanding_drain_ms = acked_bitrate_kbps_ > 0.0
        ? outstanding_bytes_ * 8.0 / acked_bitrate_kbps_
        : std::numeric_limits<double>::infinity();
    bool backlog_recovered =
        outstanding_drain_ms <= kCongestionRecoveryMaxDrainMs;
    bool recovery_healthy = delivery_recovered &&
        backlog_recovered && delay_usage != BandwidthUsage::kOveruse &&
        cwnd_recovered &&
        last_loss_fraction_ < kLossIncreaseThreshold;
    if (recovery_healthy) {
      if (congestion_recovery_start_ms_ < 0) {
        congestion_recovery_start_ms_ = now_ms;
      } else if (now_ms - congestion_recovery_start_ms_ >=
                 kCongestionRecoverySpanMs) {
        congestion_episode_active_ = false;
        congestion_recovery_start_ms_ = -1;
        severe_capacity_cliff_active_ = false;
        low_delivery_start_ms_ = -1;
        LOG(INFO) << "[GCC] Congestion episode recovered; byte-delivery "
                     "detector rearmed";
      }
    } else {
      congestion_recovery_start_ms_ = -1;
    }
    if (congestion_episode_active_) {
      low_delivery_start_ms_ = -1;
      return delay_usage;
    }
    // Do not begin a fresh episode on the same feedback that completed
    // recovery; the next feedback starts with a clean measurement history.
    return delay_usage;
  }

  if (delay_usage == BandwidthUsage::kOveruse) {
    low_delivery_start_ms_ = -1;
    return delay_usage;
  }

  // Fast capacity-cliff path. The normal detector deliberately waits 200ms to
  // reject short pacing/ACK-rate mismatches. At a large bandwidth collapse,
  // however, the first completed trendline group already contains decisive
  // evidence. Waiting for a second 5ms send-time group can cost ~84ms at a
  // 1Mbps bottleneck because that group's packets must serialize first.
  bool severe_capacity_cliff =
      sent_span_ms >= 50 && sent_rate_kbps_ > 0.0 &&
      instant_acked_sample_valid_ && instant_sent_bitrate_kbps_ > 0.0 &&
      instant_delivery_ratio_ < kSevereDeliveryRatioThreshold &&
      last_modified_trend_ > adaptive_threshold_ &&
      accumulated_delay_ >= kSevereQueueGrowthMs;
  if (severe_capacity_cliff && !severe_capacity_cliff_active_) {
    severe_capacity_cliff_active_ = true;
    low_delivery_start_ms_ = -1;
    byte_delivery_overuse_ = true;
    last_bandwidth_usage_ = BandwidthUsage::kOveruse;
    LOG(INFO) << "[GCC] Severe capacity cliff: delivered="
              << static_cast<int>(acked_bitrate_kbps_) << " kbps sent="
              << static_cast<int>(sent_rate_kbps_) << " kbps reference="
              << static_cast<int>(instant_sent_bitrate_kbps_)
              << " kbps ratio=" << instant_delivery_ratio_
              << " accumulated_delay_ms="
              << accumulated_delay_ << " modified_trend="
              << last_modified_trend_ << " threshold=" << adaptive_threshold_;
    return BandwidthUsage::kOveruse;
  }

  // ALR suppresses the ordinary sustained byte-shortfall path because low
  // delivery is expected when the application is not filling its estimate.
  // It must not suppress the severe path above. A VBR encoder can be below 80%
  // of its target yet still send several times the collapsed link capacity;
  // delivery_ratio, positive trend, and queue growth together prove that this
  // is real congestion rather than application limitation.
  bool low_delivery = estimator_ready && !alr_detector_.InAlr() &&
                      sender_saturated &&
                      delivery_ratio < kByteDeliveryRatioThreshold;

  if (!low_delivery) {
    low_delivery_start_ms_ = -1;
    severe_capacity_cliff_active_ = false;
    return delay_usage;
  }
  if (low_delivery_start_ms_ < 0) {
    low_delivery_start_ms_ = now_ms;
    return delay_usage;
  }
  if (now_ms - low_delivery_start_ms_ < kByteSignalMinSpanMs) {
    return delay_usage;
  }

  // UpdateDelayBasedRate latches the resulting reduction as one congestion
  // episode. Do not rearm while feedback still describes this backlog.
  low_delivery_start_ms_ = -1;
  byte_delivery_overuse_ = true;
  last_bandwidth_usage_ = BandwidthUsage::kOveruse;
  LOG(INFO) << "[GCC] Sustained byte-delivery shortfall: delivered="
            << static_cast<int>(acked_bitrate_kbps_) << " kbps sent="
            << static_cast<int>(sent_rate_kbps_) << " kbps reference="
            << static_cast<int>(delivery_reference_kbps) << " kbps ratio="
            << delivery_ratio;
  return BandwidthUsage::kOveruse;
}

bool GccController::ShouldIgnoreSourceLimitedOveruse(int64_t now_ms) const {
  if (byte_delivery_overuse_) return false;

  int64_t sent_span_ms = sent_rate_window_.size() >= 2
      ? now_ms - sent_rate_window_.front().time_ms
      : 0;
  bool sent_rate_ready =
      sent_span_ms >= kByteEstimatorMinSpanMs && sent_rate_kbps_ > 0.0;
  bool source_below_target =
      sent_rate_ready &&
      sent_rate_kbps_ < kSenderUtilizationThreshold *
                            delay_based_bitrate_kbps_;

  // Being below the controller target does not by itself prove the source is
  // limiting delivery. If the receiver is acknowledging materially fewer
  // bytes than the source actually sends, keep the overuse signal actionable.
  // This also avoids depending on a large outstanding-byte residue to
  // distinguish genuine congestion from content-limited encoding.
  const double delivery_ratio = aligned_sent_bitrate_kbps_ > 0.0
      ? aligned_delivery_ratio_
      : (sent_rate_kbps_ > 0.0
             ? acked_bitrate_kbps_ / sent_rate_kbps_
             : 0.0);
  if (sent_rate_ready && acked_bitrate_kbps_ > 0.0 &&
      delivery_ratio < kByteDeliveryRatioThreshold) {
    return false;
  }

  double outstanding_time_ms = sent_rate_kbps_ > 0.0
      ? outstanding_bytes_ * 8.0 / sent_rate_kbps_
      : 0.0;
  bool cwnd_healthy =
      pushback_ratio_ >= 0.999 &&
      outstanding_time_ms <= kSourceLimitedOutstandingTimeMs;

  if (!source_below_target || !cwnd_healthy) return false;

  LOG(INFO) << "[GCC] Trend-only overuse ignored while source-limited: "
            << "target=" << delay_based_bitrate_kbps_ << " kbps sent="
            << static_cast<int>(sent_rate_kbps_) << " kbps acked="
            << static_cast<int>(acked_bitrate_kbps_) << " kbps ratio="
            << delivery_ratio << " outstanding=" << outstanding_bytes_
            << " outstanding_ms=" << outstanding_time_ms
            << " window=" << current_data_window_bytes_;
  return true;
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
  last_loss_fraction_ = loss_fraction;
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
// Estimate the rate actually getting through, using a sliding time window of
// received bytes: rate = sum(recv_size in window) * 8 / window_span. A time
// window (rather than a single feedback batch) is robust to bursty arrivals —
// if a batch's packets happen to land close together (delivery-thread bunching,
// network jitter, a probe burst racing ahead of the bottleneck), the short
// intra-batch span can't inflate the estimate, because the window span still
// reflects real elapsed time across many batches. Uses the real per-packet
// recv_size carried in the feedback, not a fixed-MTU assumption.
void GccController::UpdateAckedBitrate(const TransportFeedback& feedback) {
  if (acked_frozen_for_testing_) {
    // Tests pin the smoothed acked estimate; mirror it as the per-batch
    // received-rate sample so probe-commit logic is exercisable in unit tests.
    last_received_rate_kbps_ = acked_bitrate_kbps_;
    instant_acked_sample_valid_ = true;
    return;
  }

  instant_acked_sample_valid_ = false;

  // --- Sliding-window acked throughput (for AIMD increase/decrease) ---
  // Add each acked packet to the sliding window.
  for (const auto& pkt : feedback.packets) {
    if (pkt.recv_size == 0) {
      continue;
    }
    acked_window_.push_back(
        {pkt.send_time_us, pkt.arrival_time_us, pkt.recv_size});
    acked_window_bytes_ += pkt.recv_size;
  }
  if (acked_window_.empty()) {
    return;
  }

  // Evict samples older than the window relative to the newest arrival.
  int64_t newest_us = acked_window_.back().arrival_us;
  while (acked_window_.size() > 1 &&
         newest_us - acked_window_.front().arrival_us > kAckedWindowUs) {
    acked_window_bytes_ -= acked_window_.front().bytes;
    acked_window_.pop_front();
  }

  // Fast cross-batch received rate. The receiver's strict 10ms deadline often
  // emits one packet per feedback message after a capacity cliff, so a
  // per-message rate has no span. Use the most recent <=50ms suffix of the
  // continuous arrival history instead. This remains byte-aware and reacts
  // much faster than the 200ms AIMD window while avoiding stale pre-drop rates.
  auto instant_first = acked_window_.begin();
  while (instant_first != acked_window_.end()) {
    auto next = std::next(instant_first);
    if (next == acked_window_.end() ||
        newest_us - next->arrival_us <= kInstantAckedWindowUs) {
      break;
    }
    instant_first = next;
  }
  if (instant_first != acked_window_.end()) {
    int64_t instant_span_us = newest_us - instant_first->arrival_us;
    if (instant_span_us >= kMinInstantAckedSpanUs) {
      double instant_bytes = 0.0;
      int64_t first_send_us = instant_first->send_us;
      int64_t last_send_us = -1;
      for (auto it = std::next(instant_first); it != acked_window_.end(); ++it) {
        instant_bytes += it->bytes;
        if (it->send_us >= 0) {
          if (first_send_us < 0 || it->send_us < first_send_us) {
            first_send_us = it->send_us;
          }
          if (last_send_us < 0 || it->send_us > last_send_us) {
            last_send_us = it->send_us;
          }
        }
      }
      if (instant_bytes > 0.0) {
        last_received_rate_kbps_ =
            instant_bytes * 8.0 / 1000.0 / (instant_span_us / 1e6);
        instant_acked_sample_valid_ = true;
        if (first_send_us >= 0 && last_send_us > first_send_us) {
          instant_sent_bitrate_kbps_ =
              instant_bytes * 8.0 / 1000.0 /
              ((last_send_us - first_send_us) / 1e6);
          double reference = std::min(
              instant_sent_bitrate_kbps_,
              static_cast<double>(delay_based_bitrate_kbps_));
          instant_delivery_ratio_ = reference > 0.0
              ? last_received_rate_kbps_ / reference
              : 1.0;
        }
      }
    }
  }

  if (static_cast<int>(acked_window_.size()) < kAckedMinSamples) {
    return;
  }

  // Rate over the window span. Exclude the first sample's bytes: the rate is
  // measured over the gaps between arrivals (span = last - first).
  int64_t span_us = newest_us - acked_window_.front().arrival_us;
  if (span_us <= 0) {
    return;
  }
  double window_bytes = acked_window_bytes_ - acked_window_.front().bytes;
  // The window already smooths over time, so use it directly as the estimate.
  acked_bitrate_kbps_ = window_bytes * 8.0 / 1000.0 / (span_us / 1e6);

  // Measure the sender-side rate over the exact same acknowledged packets.
  // Absolute sender/receiver clock offsets cancel because each rate uses only
  // the span within its own clock domain.
  int64_t first_send_us = -1;
  int64_t last_send_us = -1;
  for (const auto& sample : acked_window_) {
    if (sample.send_us < 0) continue;
    if (first_send_us < 0 || sample.send_us < first_send_us) {
      first_send_us = sample.send_us;
    }
    if (last_send_us < 0 || sample.send_us > last_send_us) {
      last_send_us = sample.send_us;
    }
  }
  if (first_send_us >= 0 && last_send_us > first_send_us) {
    aligned_sent_bitrate_kbps_ =
        window_bytes * 8.0 / 1000.0 /
        ((last_send_us - first_send_us) / 1e6);
  }
}

// --- Trendline estimator (faithful to WebRTC trendline_estimator.cc) ---
//
// Packets first enter 5ms send-time groups. For each completed group we compute
// the one-way delay variation
//   delta = recv_delta - send_delta
// accumulate it, EWMA-smooth it, and keep a duration-based window
// (<= kTrendlineWindowMs of arrival span). When the window is trimmed we refit
// a least-squares line; its slope is the trend fed to Detect(). All times are
// in milliseconds, matching WebRTC.

GccController::BandwidthUsage GccController::UpdateTrendline(
    const TransportFeedback& feedback) {
  BandwidthUsage usage = BandwidthUsage::kNormal;
  BandwidthUsage batch_usage = BandwidthUsage::kNormal;
  for (const auto& pkt : feedback.packets) {
    if (pkt.send_time_us < 0) {
      continue;
    }

    double send_ms = pkt.send_time_us / 1000.0;
    double arrival_ms = pkt.arrival_time_us / 1000.0;

    if (current_group_.valid &&
        arrival_ms - current_group_.last_arrival_ms >
            kTrendlineDiscontinuityMs) {
      LOG(WARNING) << "[GCC] Reset delay trendline after "
                   << (arrival_ms - current_group_.last_arrival_ms)
                   << "ms arrival discontinuity";
      ResetTrendlineAfterDiscontinuity();
    }

    if (!current_group_.valid) {
      current_group_.valid = true;
      current_group_.first_send_ms = current_group_.last_send_ms = send_ms;
      current_group_.first_arrival_ms =
          current_group_.last_arrival_ms = arrival_ms;
      current_group_.bytes = pkt.recv_size;
      current_group_.packets = 1;
      current_group_.last_frame_sequence = pkt.frame_sequence;
      current_group_.last_packet_index = pkt.packet_index;
      continue;
    }

    double group_send_span_ms = send_ms - current_group_.first_send_ms;
    if (group_send_span_ms >= 0.0 &&
        group_send_span_ms <= kSendTimeGroupMs) {
      current_group_.last_send_ms = send_ms;
      current_group_.last_arrival_ms = arrival_ms;
      current_group_.bytes += pkt.recv_size;
      current_group_.packets++;
      current_group_.last_frame_sequence = pkt.frame_sequence;
      current_group_.last_packet_index = pkt.packet_index;
      continue;
    }

    PacketGroup completed_group = current_group_;
    current_group_ = PacketGroup{};
    current_group_.valid = true;
    current_group_.first_send_ms = current_group_.last_send_ms = send_ms;
    current_group_.first_arrival_ms = current_group_.last_arrival_ms = arrival_ms;
    current_group_.bytes = pkt.recv_size;
    current_group_.packets = 1;
    current_group_.last_frame_sequence = pkt.frame_sequence;
    current_group_.last_packet_index = pkt.packet_index;

    if (!previous_group_.valid) {
      previous_group_ = completed_group;
      continue;
    }

    double recv_delta_ms = completed_group.last_arrival_ms -
                           previous_group_.last_arrival_ms;
    double send_delta_ms = completed_group.last_send_ms -
                           previous_group_.last_send_ms;
    previous_group_ = completed_group;
    if (recv_delta_ms <= 0.0 || send_delta_ms <= 0.0) {
      continue;
    }
    double delta_ms = recv_delta_ms - send_delta_ms;

    num_deltas_++;
    if (num_deltas_ > kDeltaCounterMax) num_deltas_ = kDeltaCounterMax;

    // Exponential backoff filter.
    accumulated_delay_ += delta_ms;
    smoothed_delay_ = kTrendlineSmoothingCoeff * smoothed_delay_ +
                      (1.0 - kTrendlineSmoothingCoeff) * accumulated_delay_;

    if (!first_arrival_set_) {
      first_arrival_ms_ =
          static_cast<int64_t>(completed_group.last_arrival_ms);
      first_arrival_set_ = true;
    }
    double rel_arrival_ms = completed_group.last_arrival_ms - first_arrival_ms_;
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

    // Pass recv_delta (arrival-time elapsed), NOT send_delta, as the "time
    // over using" increment. WebRTC uses send_delta, which is fine when the
    // pacer output is smooth (send_delta ≈ real spacing). But our 2.5x burst
    // pacer compresses send_delta to ~0.01ms within a burst, so the 10ms
    // "sustained overuse" guard was never met mid-burst even when the trend was
    // 15x over threshold. Arrival deltas reflect real elapsed time (packets
    // arrive at link rate), so overuse accumulates correctly.
    usage = Detect(trend, recv_delta_ms,
                   static_cast<int64_t>(completed_group.last_arrival_ms));
    if (usage == BandwidthUsage::kOveruse) {
      batch_usage = BandwidthUsage::kOveruse;
    } else if (usage == BandwidthUsage::kUnderuse &&
               batch_usage == BandwidthUsage::kNormal) {
      batch_usage = BandwidthUsage::kUnderuse;
    }

    double group_delivery_kbps =
        static_cast<double>(completed_group.bytes) * 8.0 / recv_delta_ms;
    // One line per completed send-time group. It includes actual bytes and
    // achieved delivery rate so size-related capacity changes are observable.
    const char* u = usage == BandwidthUsage::kOveruse ? "overuse"
                  : usage == BandwidthUsage::kUnderuse ? "underuse" : "normal";
    LOG(VERBOSE) << "[CC_TRACE] f=" << completed_group.last_frame_sequence
                 << " p=" << static_cast<int>(completed_group.last_packet_index)
                 << " send_ms=" << completed_group.last_send_ms
                 << " arr_ms=" << completed_group.last_arrival_ms
                 << " group_packets=" << completed_group.packets
                 << " group_bytes=" << completed_group.bytes
                 << " group_delivery_kbps=" << group_delivery_kbps
                 << " recv_delta=" << recv_delta_ms
                 << " send_delta=" << send_delta_ms
                 << " delta=" << delta_ms
                 << " accum=" << accumulated_delay_
                 << " smooth=" << smoothed_delay_
                 << " win=" << trendline_window_.size()
                 << " span=" << (trendline_window_.back().arrival_time_ms -
                                 trendline_window_.front().arrival_time_ms)
                 << " dofit=" << dofit
                 << " slope=" << trend
                 << " mod_trend=" << last_modified_trend_
                 << " thr=" << adaptive_threshold_
                 << " ovuse_cnt=" << overuse_counter_
                 << " t_over=" << time_over_using_ms_
                 << " usage=" << u;
  }

  last_bandwidth_usage_ = batch_usage;
  return batch_usage;
}

void GccController::ResetTrendlineAfterDiscontinuity() {
  trendline_window_.clear();
  accumulated_delay_ = 0.0;
  smoothed_delay_ = 0.0;
  first_arrival_ms_ = 0;
  first_arrival_set_ = false;
  current_group_ = PacketGroup{};
  previous_group_ = PacketGroup{};
  num_deltas_ = 0;
  time_over_using_ms_ = -1.0;
  overuse_counter_ = 0;
  prev_trend_ = 0.0;
  last_modified_trend_ = 0.0;
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
                                                   double elapsed_ms,
                                                   int64_t now_ms) {
  if (num_deltas_ < 2) {
    return BandwidthUsage::kNormal;
  }

  // modified_trend = min(num_deltas, kMinNumDeltas) * trend * gain
  const double modified_trend =
      std::min(num_deltas_, kMinNumDeltas) * trend * kThresholdGain;
  last_modified_trend_ = modified_trend;  // expose for trace logging

  BandwidthUsage result = BandwidthUsage::kNormal;

  if (modified_trend > adaptive_threshold_) {
    if (time_over_using_ms_ < 0) {
      // Initialize: assume over-using half the time since the previous sample.
      time_over_using_ms_ = elapsed_ms / 2.0;
    } else {
      time_over_using_ms_ += elapsed_ms;
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

  // Store the bandwidth usage state for GetNetworkUsageState()
  last_bandwidth_usage_ = result;

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

double GccController::EffectiveAckedKbps() const {
  // In overuse, react to the instantaneous received rate — on a capacity
  // cliff it reports the collapsed rate immediately, whereas the sliding
  // window still averages in pre-drop throughput and lags ~1 window behind.
  if (use_instant_acked_ && instant_acked_sample_valid_ &&
      last_received_rate_kbps_ > 0.0) {
    return last_received_rate_kbps_;
  }
  return acked_bitrate_kbps_;
}

void GccController::UpdateDelayBasedRate(BandwidthUsage usage, int64_t now_ms) {
  // Latch the acked-rate source on state transitions: overuse -> react to the
  // instantaneous rate; underuse (queue draining) -> back to the smooth
  // window. kNormal holds whichever mode we're in.
  if (usage == BandwidthUsage::kOveruse) {
    // A per-batch ACK rate is intentionally burst-sensitive. Use it only when
    // the byte-delivery detector independently confirms a capacity cliff.
    // Ordinary trendline overuse follows WebRTC and decreases from the
    // smoothed acknowledged-throughput estimate, preventing codec frame/slice
    // burst patterns from producing different-sized reductions.
    use_instant_acked_ = byte_delivery_overuse_ && instant_acked_sample_valid_;
  } else if (usage == BandwidthUsage::kUnderuse) {
    use_instant_acked_ = false;
  }
  switch (usage) {
    case BandwidthUsage::kOveruse: {
      if (congestion_episode_active_) {
        // This feedback still belongs to the already-reduced queue episode.
        // Cwnd pushback below remains active, but do not permanently ratchet
        // the delay-based estimate down again from stale queued packets.
        last_overuse_time_ms_ = now_ms;
        if (last_suppressed_overuse_log_ms_ < 0 ||
            now_ms - last_suppressed_overuse_log_ms_ >= 1000) {
          LOG(INFO) << "[GCC] Overuse reduction suppressed while existing "
                       "congestion episode drains";
          last_suppressed_overuse_log_ms_ = now_ms;
        }
        break;
      }
      // WebRTC AimdRateControl decrease: snap to beta * measured throughput
      // (not beta * current estimate). This drains the self-induced queue and
      // lands the estimate right at ~capacity instead of just shaving the
      // (possibly inflated) current value. During overuse the throughput is
      // the INSTANTANEOUS received rate (see EffectiveAckedKbps).
      double eff_acked = EffectiveAckedKbps();
      int target = delay_based_bitrate_kbps_;
      if (eff_acked > 0.0) {
        int from_acked = static_cast<int>(eff_acked * kMultiplicativeDecrease);
        // Only decrease (never raise on overuse).
        target = std::min(delay_based_bitrate_kbps_, from_acked);
      } else {
        target = static_cast<int>(delay_based_bitrate_kbps_ * kMultiplicativeDecrease);
      }
      int previous_bitrate_kbps = delay_based_bitrate_kbps_;
      delay_based_bitrate_kbps_ = std::max(target, min_bitrate_kbps_);
      overuse_counter_ = 0;
      last_overuse_time_ms_ = now_ms;
      if (delay_based_bitrate_kbps_ < previous_bitrate_kbps) {
        congestion_episode_active_ = true;
        congestion_recovery_start_ms_ = -1;
        last_suppressed_overuse_log_ms_ = -1;
      }
      LOG(INFO) << "[GCC] Overuse → decrease to " << delay_based_bitrate_kbps_
                << " kbps (eff_acked=" << static_cast<int>(eff_acked)
                << " instant=" << (use_instant_acked_ ? 1 : 0)
                << " window_acked=" << static_cast<int>(acked_bitrate_kbps_) << ")";
      break;
    }
    case BandwidthUsage::kUnderuse:
    case BandwidthUsage::kNormal: {
      // The permanent estimate remains fixed while cwnd pushback drains an
      // active episode. Normal additive growth resumes only after the recovery
      // hysteresis in ApplyByteDeliverySignal has rearmed the controller.
      if (congestion_episode_active_) break;

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
      double eff_acked = EffectiveAckedKbps();
      if (eff_acked > 0.0) {
        int increase_limit = static_cast<int>(1.5 * eff_acked) + 10;
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

void GccController::MaybeSyncLossToDelay() {
  // Low-loss regime: loss_based tracks delay_based exactly (WebRTC: loss-based
  // target is capped at delay-based, and with no loss they coincide). Done every
  // batch so a stale loss_based can't pin the final target below a just-updated
  // delay_based — the post-probe rate collapse we saw at static_10mbps t≈1.16s,
  // where delay_based=2542 but loss_based lingered at 531 for ~0.4s.
  if (last_loss_fraction_ < kLossIncreaseThreshold) {
    loss_based_bitrate_kbps_ = std::clamp(delay_based_bitrate_kbps_,
                                          min_bitrate_kbps_, max_bitrate_kbps_);
  }
}

void GccController::UpdateCongestionWindowSize() {
  // WebRTC goog_cc_network_control.cc::UpdateCongestionWindowSize.
  // time_window = min(feedback_max_rtt over the window) + additional time.
  if (feedback_max_rtts_.empty()) return;
  int64_t min_feedback_max_rtt_ms = feedback_max_rtts_.front().second;
  for (const auto& s : feedback_max_rtts_) {
    if (s.second < min_feedback_max_rtt_ms) min_feedback_max_rtt_ms = s.second;
  }
  int64_t time_window_ms = min_feedback_max_rtt_ms + cwnd_queue_size_ms_;
  // data_window = loss_based_rate * time_window. loss_based is kbps, time is
  // ms: bytes = kbps * 1000 / 8 * (ms/1000) = loss_based_kbps * time_ms / 8.
  int64_t data_window =
      static_cast<int64_t>(loss_based_bitrate_kbps_) * time_window_ms / 8;
  if (current_data_window_bytes_ > 0) {
    // EWMA with prior: (new + old) / 2, floored at kMinCwndBytes.
    data_window =
        std::max<int64_t>(kMinCwndBytes,
                          (data_window + current_data_window_bytes_) / 2);
  } else {
    data_window = std::max<int64_t>(kMinCwndBytes, data_window);
  }
  current_data_window_bytes_ = data_window;
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
