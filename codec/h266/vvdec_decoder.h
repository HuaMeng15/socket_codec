#ifndef CODEC_VVDEC_DECODER_H
#define CODEC_VVDEC_DECODER_H

#ifdef VVENC

#include "../decoder.h"
#include "vvdec/vvdec.h"
#include <map>

class VVdecDecoder : public Decoder {
 public:
  VVdecDecoder();
  ~VVdecDecoder() override;

  // Decoder interface
  int Initialize(int width, int height) override;
  YUVBuffer* DecodeFrame(const uint8_t* frame_data, size_t frame_size) override;
  void ReleaseFrame(YUVBuffer* frame) override;
  void Cleanup() override;

 private:
  // Convert vvdecFrame to YUVBuffer
  YUVBuffer* ConvertVVdecFrame(vvdecFrame* vvdec_frame);

  vvdecDecoder* decoder_;
  vvdecParams params_;
  bool initialized_;
  vvdecAccessUnit access_unit_;

  // Map to track vvdecFrame to YUVBuffer conversions
  std::map<vvdecFrame*, YUVBuffer*> frame_conversions_;
};

#endif  // VVENC

#endif  // CODEC_VVDEC_DECODER_H

