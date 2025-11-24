#ifndef CODEC_ENCODER_H
#define CODEC_ENCODER_H

#include <atomic>
#include <condition_variable>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>

#include "tools/yuv_file_io.h"
#include "vvenc/vvenc.h"
#include "vvenc/vvencCfg.h"

// Forward declaration
class FrameCapture;
class MessageSender;

class Encoder {
 public:
  Encoder();
  ~Encoder();

  // Initialize encoder with configuration
  int Initialize(int width, int height, int fps, int framesToBeEncoded = -1);

  // Set output stream for encoded data
  void SetOutputStream(std::ofstream* output_stream);

  // Set message sender for network transmission
  void SetMessageSender(MessageSender* message_sender);

  // Set frame capture reference for thread-safe encoding
  void SetFrameCapture(FrameCapture* frame_capture);

  // Run encoder in thread-safe mode (to be called in a separate thread)
  // Works with FrameCapture for synchronized frame-by-frame encoding
  void Run();

  // Encode a single frame (for frame-by-frame encoding)
  // Returns 0 on success, negative value on error
  int EncodeFrame(vvencYUVBuffer* input_buffer, bool& bEncodeDone);

  // Signal that encoder is ready for next frame
  void SignalReadyForNextFrame();

  // Stop the encoder
  void Stop();

  // Check if encoder is stopped
  bool IsStopped() const;

  // Cleanup resources
  void Cleanup();

  // Get encoder statistics
  void PrintSummary() const;

 private:
  // Initialize encoder parameters
  void InitializeEncoderParams(vvenc_config* params, int width, int height,
                               int fps, int framesToBeEncoded);

  // Copy frame buffer data from source to encoder's buffer
  void CopyFrameBuffer(const vvencYUVBuffer* source);

  // Write encoded access unit to output stream
  void WriteEncodedData(
      const std::chrono::high_resolution_clock::time_point& start_time);

  FrameCapture* frame_capture_;
  MessageSender* message_sender_;

  vvencEncoder* encoder_;
  vvenc_config params_;
  vvencYUVBuffer yuv_input_buffer_;
  vvencAccessUnit access_unit_;
  std::ofstream* output_stream_;
  bool initialized_;
  int64_t sequence_number_;
  int64_t max_frames_;

  // Thread synchronization
  std::atomic<bool> stop_requested_;
};

#endif  // CODEC_ENCODER_H
