#ifndef TRANSMISSION_FEEDBACK_COLLECTOR_H
#define TRANSMISSION_FEEDBACK_COLLECTOR_H

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
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
  ~FeedbackCollector();

  FeedbackCollector(const FeedbackCollector&) = delete;
  FeedbackCollector& operator=(const FeedbackCollector&) = delete;

  /** Set callback for sending feedback bytes. */
  void SetSendCallback(SendCallback cb);

  /** Set feedback interval: send after this many packets accumulated. */
  void SetFeedbackInterval(int packet_count);

  /**
   * Set the max time between consecutive feedback reports (ms). Feedback is
   * sent on whichever comes first: feedback_interval_ packets, or this time
   * elapsed since the last feedback was sent. This ensures regular feedback
   * even when packet arrivals are bursty or have gaps between frames.
   * <=0 disables the time trigger.
   */
  void SetFeedbackMaxIntervalMs(int ms);

  /**
   * Record a packet arrival. Stores timestamp and the actual received byte
   * count, and triggers feedback send if interval is reached.
   */
  void OnPacketReceived(uint16_t frame_sequence, uint16_t packet_index,
                        uint16_t recv_size);

  /** Record a packet using a socket/kernel-provided arrival timestamp. */
  void OnPacketReceived(uint16_t frame_sequence, uint16_t packet_index,
                        uint16_t recv_size, int64_t arrival_time_us);

  /**
   * Detect lost packets: checks for gaps in expected sequence within a frame.
   * Call after a frame is complete or timed out.
   * Returns lost packets for the given frame.
   */
  static std::vector<LossReport::LostPacket> DetectLoss(
      uint16_t frame_sequence, uint16_t total_packets,
      const std::vector<bool>& received_mask);

  /** Force-send any accumulated feedback (e.g. on frame completion). */
  void Flush();

  /** Stop the deadline timer. Safe to call more than once. */
  void Stop();

  /** Send a loss report for the given lost packets. */
  void SendLossReport(const std::vector<LossReport::LostPacket>& lost_packets);

 private:
  void SendTransportFeedback();
  void TimerLoop();

  struct ArrivalEntry {
    uint16_t frame_sequence;
    uint16_t packet_index;
    uint16_t recv_size;
    int64_t arrival_time_us;
  };

  std::mutex mutex_;
  std::vector<ArrivalEntry> pending_entries_;
  SendCallback send_cb_;
  int feedback_interval_;  // send after this many packets
  int feedback_max_interval_ms_;  // or after this much time since last feedback (<=0 off)
  int64_t epoch_us_;
  bool epoch_set_;
  std::chrono::steady_clock::time_point last_feedback_time_;  // time of last feedback send
  std::chrono::steady_clock::time_point pending_since_;
  std::condition_variable timer_cv_;
  std::thread timer_thread_;
  bool stopping_;
};

#endif  // TRANSMISSION_FEEDBACK_COLLECTOR_H
