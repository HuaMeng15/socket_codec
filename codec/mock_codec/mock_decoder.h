#ifndef CODEC_MOCK_DECODER_H
#define CODEC_MOCK_DECODER_H

#include "../decoder.h"

// Fake decoder for ablation: does not decode; ignores input and returns
// a zero-filled YUV buffer with the expected resolution so the pipeline
// and network behavior can be tested without a real codec.
class MockDecoder : public Decoder {
 public:
  MockDecoder();
  ~MockDecoder() override;

  int Initialize(int width, int height) override;
  YUVBuffer* DecodeFrame(const uint8_t* frame_data, size_t frame_size) override;
  void ReleaseFrame(YUVBuffer* frame) override;
  void Cleanup() override;

 private:
  bool initialized_;
  int width_;
  int height_;
};

#endif  // CODEC_MOCK_DECODER_H
