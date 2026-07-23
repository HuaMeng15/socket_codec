#ifndef CODEC_MOCK_ENCODER_H
#define CODEC_MOCK_ENCODER_H

#include "../encoder.h"
#include "config/config.h"
#include <cstddef>
#include <cstdint>
#include <fstream>

// Fake encoder for ablation: does not encode; outputs zero-filled data
// with size = target_bitrate / (8 * fps) bytes per frame to simulate
// network load at the target bitrate.
class MockEncoder : public Encoder {
 public:
  MockEncoder();
  ~MockEncoder() override;

  int Initialize(int width, int height, int fps, int framesToBeEncoded = -1) override;
  void SetOutputStream(std::ofstream* output_stream) override;
  std::unique_ptr<EncodedData> EncodeFrame(YUVBuffer* input_buffer) override;
  void Cleanup() override;
  void PrintSummary() const override;
  void SetTargetBitrate(int bitrate_kbps) override;

 private:
  std::ofstream* output_stream_;
  bool initialized_;
  uint16_t sequence_number_;
  int width_;
  int height_;
  int fps_;
  int target_bitrate_kbps_;
  size_t bytes_per_frame_;  // full-rate bytes: target_bitrate / (8 * fps)

  // --- Variable (content-adaptive VBR) mode ---
  // When enabled, EncodeFrame produces min(target, content_demand(t)) where
  // content_demand follows a duty cycle: full target for the "high" portion of
  // each period, then alr_low_ratio_ x target for the "low" (app-limited)
  // portion. Static mode (default) always produces the full target.
  bool variable_mode_ = false;
  double alr_low_ratio_ = 0.15;      // low-phase output as fraction of target
  int period_ms_ = 10000;            // one high/low cycle
  double alr_fraction_ = 0.40;       // fraction of the period in the low phase
  int64_t first_frame_time_ms_ = -1; // wall-clock anchor for the duty cycle
  bool in_low_phase_ = false;        // last phase, for transition logging

  int64_t NowMs() const;

  // Startup default; overridden by SetTargetBitrate(cc_initial) before the
  // first frame. Aligned with the GCC/pacer startup rate.
  static const int kDefaultTargetBitrateKbps = kDefaultInitialBitrateKbps;
};

#endif  // CODEC_MOCK_ENCODER_H
