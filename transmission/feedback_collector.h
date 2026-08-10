#ifndef TRANSMISSION_FEEDBACK_COLLECTOR_H
#define TRANSMISSION_FEEDBACK_COLLECTOR_H

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

#include "transport_feedback.h"

/**
 * FeedbackCollector: lives on the receiver side. Collects per-packet arrival
 * timestamps and sends TWCC-style TransportFeedback messages back to the
 * sender at regular intervals.
 *
 * Uses a callback for sending — decoupled from DataSender for testability.
 *
 * Usage:
 *   collector.SetSendCallback([&sender](const uint8_t* d, size_t s) {
 *     sender.SendRawFeedback(d, s);
 *   });
 *   collector.OnPacketReceived(frame_seq, packet_idx);  // for each packet
 */
class FeedbackCollector {
 public:
  using SendCallback = std::function<void(const uint8_t* data, size_t size)>;

  FeedbackCollector();
  ~FeedbackCollector() = default;

  /** Set callback for sending feedback bytes. */
  void SetSendCallback(SendCallback cb);

  /** Set feedback interval: send after this many packets accumulated. */
  void SetFeedbackInterval(int packet_count);

  /**
   * Set the max time a packet may wait before its batch is sent (ms). Feedback
   * is flushed on whichever comes first: feedback_interval_ packets, or a
   * pending packet aging past this window. This bounds feedback latency at low
   * bitrates, where the packet-count trigger would otherwise take far too long
   * (e.g. 20 pkts @ 1 Mbps ≈ 192 ms). <=0 disables the time trigger.
   */
  void SetFeedbackMaxIntervalMs(int ms);

  /**
   * Record a packet arrival. Stores timestamp and the actual received byte
   * count, and triggers feedback send if interval is reached.
   */
  void OnPacketReceived(uint16_t frame_sequence, uint8_t packet_index,
                        uint16_t recv_size);

  /**
   * Detect lost packets: checks for gaps in expected sequence within a frame.
   * Call after a frame is complete or timed out.
   * Returns lost packets for the given frame.
   */
  static std::vector<LossReport::LostPacket> DetectLoss(
      uint16_t frame_sequence, uint8_t total_packets,
      const std::vector<bool>& received_mask);

  /** Force-send any accumulated feedback (e.g. on frame completion). */
  void Flush();

  /** Send a loss report for the given lost packets. */
  void SendLossReport(const std::vector<LossReport::LostPacket>& lost_packets);

 private:
  void SendTransportFeedback();

  struct ArrivalEntry {
    uint16_t frame_sequence;
    uint8_t packet_index;
    uint16_t recv_size;
    std::chrono::steady_clock::time_point arrival_time;
  };

  std::mutex mutex_;
  std::vector<ArrivalEntry> pending_entries_;
  SendCallback send_cb_;
  int feedback_interval_;  // send after this many packets
  int feedback_max_interval_ms_;  // or after a packet waits this long (<=0 off)
  std::chrono::steady_clock::time_point epoch_;
  bool epoch_set_;
};

#endif  // TRANSMISSION_FEEDBACK_COLLECTOR_H
