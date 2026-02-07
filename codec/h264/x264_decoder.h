#ifndef CODEC_X264_DECODER_H
#define CODEC_X264_DECODER_H

#include "../decoder.h"

// FFmpeg includes
extern "C" {
#include "libavcodec/avcodec.h"
#include "libavutil/avutil.h"
#include "libavutil/imgutils.h"
}

class X264Decoder : public Decoder {
 public:
  X264Decoder();
  ~X264Decoder() override;

  // Decoder interface
  int Initialize(int width, int height) override;
  YUVBuffer* DecodeFrame(const uint8_t* frame_data, size_t frame_size) override;
  void ReleaseFrame(YUVBuffer* frame) override;
  void Cleanup() override;

 private:
  // Convert AVFrame to YUVBuffer
  YUVBuffer* ConvertAVFrameToYUVBuffer(AVFrame* frame);

  AVCodecContext* codec_context_;
  const AVCodec* codec_;
  AVFrame* frame_;
  AVPacket* packet_;
  bool initialized_;
  int width_;
  int height_;
};

#endif  // CODEC_X264_DECODER_H

