#ifndef FRAME_CAPTURE_H
#define FRAME_CAPTURE_H

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>

#include "tools/yuv_file_io.h"
#include "vvenc/vvenc.h"

const vvencChromaFormat kFileChromaFormat = VVENC_CHROMA_420;

class FrameCapture {
 public:
  FrameCapture();
  ~FrameCapture();

  // Initialize frame capture with input file and buffer dimensions
  int Initialize(const std::string& input_file, int width, int height);

  // Run the frame capture loop (to be called in a separate thread)
  void Run();

  // Signal that encoder is ready for next frame
  void SignalEncoderReady();

  // Wait for encoder to be ready, then get the next frame buffer pointer
  // Returns pointer to frame buffer if available, nullptr if EOF or error
  // Caller should copy the frame data before signaling ready for next frame
  vvencYUVBuffer* WaitForFrame();

  // Signal that frame capture should stop
  void Stop();

  // Check if frame capture is stopped
  bool IsStopped() const;

  // Check if EOF reached
  bool IsEof() const;

  // Get error message if any
  std::string GetError() const;

 private:
  YuvFileIO yuv_file_input_;
  std::string input_file_;
  int width_;
  int height_;
  vvencYUVBuffer frame_buffer_;

  // Synchronization
  std::mutex mutex_;
  std::condition_variable encoder_ready_cv_;
  std::condition_variable frame_ready_cv_;
  std::atomic<bool> encoder_ready_;
  std::atomic<bool> frame_ready_;
  std::atomic<bool> stop_requested_;
  std::atomic<bool> eof_reached_;
  std::string error_message_;

  // Frame reading state
  bool frame_available_;
};

#endif  // FRAME_CAPTURE_H

