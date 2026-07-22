#include "pacer.h"

#include <algorithm>
#include <arpa/inet.h>
#include <chrono>

#include "config/config.h"
#include "log_system/log_system.h"
#include "packet_header.h"

static const int kDefaultBitrateKbps = kDefaultInitialBitrateKbps;

Pacer::Pacer() : bitrate_kbps_(kDefaultBitrateKbps) {}

Pacer::~Pacer() { Stop(); }

void Pacer::SetPaceMultiplier(double m) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (m > 0.0) pace_multiplier_ = m;
}

void Pacer::SetBurstCapMs(double ms) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (ms > 0.0) burst_cap_ms_ = ms;
}

void Pacer::SetTargetBitrate(int bitrate_kbps) {
  std::lock_guard<std::mutex> lock(mutex_);
  bitrate_kbps_ = bitrate_kbps > 0 ? bitrate_kbps : kDefaultBitrateKbps;
  cv_.notify_all();
}

void Pacer::SetProbing(bool probing) {
  std::lock_guard<std::mutex> lock(mutex_);
  probing_ = probing;
  cv_.notify_all();  // wake the drain thread to start/stop padding promptly
}

void Pacer::Enqueue(const uint8_t* data, size_t size,
                    uint16_t frame_sequence, uint8_t packet_index) {
  if (!data || size == 0) return;
  std::lock_guard<std::mutex> lock(mutex_);
  if (queue_.size() >= kMaxQueuePackets) {
    // Drop oldest to bound memory/latency if a producer outpaces the drain.
    queue_.pop_front();
    static int64_t dropped = 0;
    if ((++dropped % 500) == 1) {
      LOG(WARNING) << "[Pacer] Queue full (" << kMaxQueuePackets
                   << " pkts); dropping oldest. total_dropped=" << dropped;
    }
  }
  QueuedPacket p;
  p.data.assign(data, data + size);
  p.frame_sequence = frame_sequence;
  p.packet_index = packet_index;
  queue_.push_back(std::move(p));
  cv_.notify_all();
}

void Pacer::Start() {
  if (running_.exchange(true)) return;  // already running
  thread_ = std::thread([this] { Run(); });
}

void Pacer::Stop() {
  if (!running_.exchange(false)) return;  // already stopped
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
}

// Current pace rate in bits/s. During a probe we pace at exactly the target
// (the probe target IS the rate we want to test on the wire); outside probes we
// pace at multiplier× to absorb encoder bursts.
double Pacer::EffectiveRateBps() {
  double mult = probing_ ? 1.0 : pace_multiplier_;
  return static_cast<double>(bitrate_kbps_) * 1000.0 * mult;
}

void Pacer::SendPadding(size_t payload_bytes) {
  const size_t header_size = sizeof(FramePacketHeader);
  std::vector<uint8_t> pkt(header_size + payload_bytes, 0);
  auto* h = reinterpret_cast<FramePacketHeader*>(pkt.data());
  h->frame_sequence = htons(kPaddingFrameSequence);
  h->packet_index = static_cast<uint8_t>(padding_counter_++ & 0xFF);
  h->total_packets = 0;  // padding sentinel: not a real assemblable frame
  h->payload_size = htons(static_cast<uint16_t>(payload_bytes));
  if (send_fn_) send_fn_(pkt.data(), pkt.size());
}

// Drain-thread body: a bounded token bucket. Tokens (send credit, in bits)
// refill continuously at EffectiveRateBps() and are capped at burst_cap_ms of
// data so an idle gap can never bank enough credit to dump a whole frame at
// once. Each real packet consumes its wire bits; when the real queue is empty
// and a probe is active, padding is emitted to keep the pipe full at the probe
// rate so the received-rate measurement reflects true capacity.
void Pacer::Run() {
  std::unique_lock<std::mutex> lock(mutex_);
  last_refill_ = std::chrono::steady_clock::now();
  refill_initialized_ = true;

  while (running_.load()) {
    // 1) Refill tokens for elapsed time, capped at the burst ceiling.
    auto now = std::chrono::steady_clock::now();
    double rate_bps = EffectiveRateBps();
    double elapsed_s =
        std::chrono::duration<double>(now - last_refill_).count();
    last_refill_ = now;
    tokens_bits_ += elapsed_s * rate_bps;
    double burst_cap_bits = (burst_cap_ms_ / 1000.0) * rate_bps;
    if (tokens_bits_ > burst_cap_bits) tokens_bits_ = burst_cap_bits;

    // Send-eligibility uses WebRTC's IntervalBudget rule: a packet may go out
    // whenever there is ANY positive credit; sending then drives tokens
    // negative by the packet's full cost, and the next packet waits until
    // credit recovers past zero. This is what lets a packet LARGER than the
    // burst cap (e.g. a 1460B packet under a 5ms/1Mbps = 625B cap) ever be
    // sent — a "tokens >= packet_bits" rule would starve it forever. The cap
    // still bounds the *positive* accumulation, so an idle gap can burst at
    // most burst_cap_ms of data before pacing resumes.

    // 2) Real packet waiting: send if we have credit, else wait for recovery.
    if (!queue_.empty()) {
      if (tokens_bits_ > 0.0) {
        double need_bits = static_cast<double>(queue_.front().data.size()) * 8.0;
        QueuedPacket p = std::move(queue_.front());
        queue_.pop_front();
        tokens_bits_ -= need_bits;
        lock.unlock();
        if (record_fn_) record_fn_(p.frame_sequence, p.packet_index);
        if (send_fn_) send_fn_(p.data.data(), p.data.size());
        lock.lock();
        continue;  // immediately try the next packet
      }
      // No credit: sleep until tokens climb back above zero.
      double wait_s = (-tokens_bits_) / std::max(rate_bps, 1.0);
      cv_.wait_for(lock, std::chrono::duration<double>(std::max(wait_s, 0.0005)));
      continue;
    }

    // 3) Queue empty. If probing, emit padding to fill the pipe at probe rate.
    if (probing_) {
      const size_t header_size = sizeof(FramePacketHeader);
      size_t payload = (max_packet_size_ > header_size)
                           ? (max_packet_size_ - header_size)
                           : 1;
      double pkt_bits = static_cast<double>(header_size + payload) * 8.0;
      if (tokens_bits_ > 0.0) {
        tokens_bits_ -= pkt_bits;
        lock.unlock();
        SendPadding(payload);
        lock.lock();
      } else {
        double wait_s = (-tokens_bits_) / std::max(rate_bps, 1.0);
        cv_.wait_for(lock, std::chrono::duration<double>(std::max(wait_s, 0.0005)));
      }
      continue;
    }

    // 4) Idle and not probing: wait until a packet is enqueued or state
    // changes. Tokens are already capped, so a burst can fire immediately on
    // arrival without having banked unbounded credit.
    cv_.wait(lock);
  }
}
