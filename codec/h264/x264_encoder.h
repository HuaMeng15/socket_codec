#ifndef CODEC_X264_ENCODER_H
#define CODEC_X264_ENCODER_H

#include "../encoder.h"
#include "x264/x264_config.h"
#include "x264/x264.h"
#include <vector>

class X264Encoder : public Encoder {
 public:
  X264Encoder();
  ~X264Encoder() override;

  // Encoder interface
  int Initialize(int width, int height, int fps, int framesToBeEncoded = -1) override;
  void SetOutputStream(std::ofstream* output_stream) override;
  std::unique_ptr<EncodedData> EncodeFrame(YUVBuffer* input_buffer) override;
  void Cleanup() override;
  void PrintSummary() const override;
  void SetTargetBitrate(int bitrate_kbps) override;

 private:
  x264_t* encoder_;
  x264_param_t params_;
  x264_picture_t pic_in_;
  x264_picture_t pic_out_;
  
  std::ofstream* output_stream_;
  bool initialized_;
  uint16_t sequence_number_;
  int width_;
  int height_;
  int fps_;
};

#endif  // CODEC_X264_ENCODER_H

