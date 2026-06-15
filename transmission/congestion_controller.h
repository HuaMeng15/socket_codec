#ifndef TRANSMISSION_CONGESTION_CONTROLLER_H
#define TRANSMISSION_CONGESTION_CONTROLLER_H

#include <cstdint>

#include "transport_feedback.h"

/**
 * CongestionController: base class for congestion control algorithms.
 * Receives transport feedback and produces a target bitrate.
 */
class CongestionController {
 public:
  virtual ~CongestionController() = default;

  /** Process a batch of transport feedback (arrival times). */
  virtual void OnTransportFeedback(const TransportFeedback& feedback) = 0;

  /** Process a loss report. */
  virtual void OnLossReport(const LossReport& report) = 0;

  /** Get the current target bitrate in kbps. */
  virtual int GetTargetBitrateKbps() const = 0;

  /** Set bitrate bounds. */
  virtual void SetBitrateRange(int min_kbps, int max_kbps) = 0;
};

#endif  // TRANSMISSION_CONGESTION_CONTROLLER_H
