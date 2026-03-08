#ifndef FRAME_CAPTURE_H
#define FRAME_CAPTURE_H

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "tools/yuv_file_io.h"
#include "encoder.h"

class FrameCapture {
 public:
  FrameCapture();
  ~FrameCapture();

  int Initialize(const std::string& input_file, int width, int height, int fps);

  /** Rewind input to the first frame (for looping when max_frames > input length). */
  void Reset();

  std::unique_ptr<YUVBuffer> ReadNextFrame(bool& is_eof);

 private:
  YuvFileIO yuv_file_input_;
  std::string input_file_;
  int width_;
  int height_;
  int fps_;
  int frame_interval_ms_;
  int64_t sequence_number_;
  size_t frame_size_bytes_;  ///< YUV420 frame size for seek/rewind
  bool forward_;             ///< true = head→tail, false = tail→head
};

#endif  // FRAME_CAPTURE_H

