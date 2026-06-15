#ifndef TRANSMISSION_FEEDBACK_HANDLER_H
#define TRANSMISSION_FEEDBACK_HANDLER_H

#include <cstdint>
#include <functional>
#include <string>
#include <deque>
#include "codec/encoder.h"
#include "transmission/message_handler.h"
#include "transmission/packet_header.h"
#include "transmission/transport_feedback.h"

class PacketSendTimeStore;
class Pacer;

/**
 * FeedbackHandler: lives on the sender side. Receives feedback messages
 * from the receiver (legacy ACKs, TransportFeedback, LossReports) and
 * dispatches them to the congestion controller.
 *
 * Callback-based: register handlers for transport feedback and loss reports.
 */
class FeedbackHandler : public MessageHandler {
 public:
  using TransportFeedbackCallback = std::function<void(const TransportFeedback&)>;
  using LossReportCallback = std::function<void(const LossReport&)>;

  FeedbackHandler();
  ~FeedbackHandler();

  int Initialize();

  int HandlePacketMessage(const uint8_t* packet_data,
                          size_t packet_size) override;

  uint32_t GetFeedbackCount() const { return feedback_count_; }

  void SetSendTimeStore(PacketSendTimeStore* store) { send_time_store_ = store; }
  void SetEncoder(Encoder* encoder) { encoder_ = encoder; }
  void SetPacer(Pacer* pacer) { pacer_ = pacer; }

  /** Register callback for TWCC transport feedback. */
  void SetTransportFeedbackCallback(TransportFeedbackCallback cb) {
    transport_feedback_cb_ = std::move(cb);
  }
  /** Register callback for loss reports. */
  void SetLossReportCallback(LossReportCallback cb) {
    loss_report_cb_ = std::move(cb);
  }

 private:
  int HandleLegacyFeedback(const uint8_t* data, size_t size);
  int HandleTransportFeedback(const uint8_t* data, size_t size);
  int HandleLossReport(const uint8_t* data, size_t size);

  bool initialized_;
  uint32_t feedback_count_;
  PacketSendTimeStore* send_time_store_ = nullptr;
  Encoder* encoder_ = nullptr;
  Pacer* pacer_ = nullptr;
  TransportFeedbackCallback transport_feedback_cb_;
  LossReportCallback loss_report_cb_;
};

#endif  // TRANSMISSION_FEEDBACK_HANDLER_H
