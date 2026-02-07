#ifndef CODEC_DECODER_H
#define CODEC_DECODER_H

#include <cstdint>
#include <string>

#include "encoder.h"  // For YUVBuffer

// Use YUVBuffer for decoded frames (same structure as encoder input)

// Decoder interface: Only responsible for decoding encoded data to raw YUV data
class Decoder {
 public:
  Decoder() = default;
  virtual ~Decoder() = default;

  // Initialize decoder with video parameters
  virtual int Initialize(int width, int height) = 0;

  // Decode a complete encoded frame to raw YUV data
  // frame_data: complete encoded frame data (already assembled)
  // frame_size: size of the encoded frame data
  // Returns decoded frame pointer (caller must call ReleaseFrame when done)
  // Returns nullptr on error or if more data is needed
  virtual YUVBuffer* DecodeFrame(const uint8_t* frame_data, size_t frame_size) = 0;

  // Release a decoded frame (free memory)
  virtual void ReleaseFrame(YUVBuffer* frame) = 0;

  // Cleanup decoder resources
  virtual void Cleanup() = 0;
};

#endif  // CODEC_DECODER_H

