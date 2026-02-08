#ifndef CODEC_MOCK_ENCODER_H
#define CODEC_MOCK_ENCODER_H

#include "../encoder.h"
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
  size_t bytes_per_frame_;  // target_bitrate / (8 * fps)

  static const int kDefaultTargetBitrateKbps = 9000;  // 9 Mbps
};

#endif  // CODEC_MOCK_ENCODER_H
