#include "frame_capture.h"

#include <thread>
#include <chrono>

#include "log_system/log_system.h"

FrameCapture::FrameCapture()
    : width_(0),
      height_(0),
      sequence_number_(0),
      frame_size_bytes_(0),
      forward_(true) {
}

FrameCapture::~FrameCapture() {
  if (yuv_file_input_.isOpen()) {
    yuv_file_input_.close();
  }
}

int FrameCapture::Initialize(const std::string& input_file, int width,
                              int height, int fps) {
  input_file_ = input_file;
  width_ = width;
  height_ = height;
  fps_ = fps;
  frame_interval_ms_ = 1000 / fps_;
  sequence_number_ = 0;
  forward_ = true;
  // YUV420: Y + U + V = width*height + (width/2)*(height/2) * 2
  frame_size_bytes_ = static_cast<size_t>(width) * height * 3 / 2;

  if (0 != yuv_file_input_.open(input_file)) {
    LOG(ERROR) << "[FrameCapture] Failed to open input file: " << input_file;
    return -1;
  }

  LOG(INFO) << "[FrameCapture] Initialized with file: " << input_file_
            << " resolution: " << width_ << "x" << height_;

  return 0;
}

void FrameCapture::Reset() {
  if (!yuv_file_input_.isOpen()) return;
  if (forward_) {
    yuv_file_input_.seekToEnd();
    std::streampos file_size = yuv_file_input_.tell();
    if (file_size >= static_cast<std::streampos>(frame_size_bytes_)) {
      forward_ = false;
      yuv_file_input_.seekToLastFrame(frame_size_bytes_);
      LOG(INFO) << "[FrameCapture] Loop: switching to tail→head";
    } else {
      yuv_file_input_.rewind();
      LOG(INFO) << "[FrameCapture] Loop: file shorter than one frame, rewind to head";
    }
  } else {
    // Backward pass done: switch to forward, start from head
    forward_ = true;
    yuv_file_input_.rewind();
    LOG(INFO) << "[FrameCapture] Loop: switching to head→tail";
  }
}

std::unique_ptr<YUVBuffer> FrameCapture::ReadNextFrame(bool& is_eof) {
  auto frame_buffer = std::make_unique<YUVBuffer>(width_, height_);
  frame_buffer->sequence_number = sequence_number_;
  frame_buffer->cts = sequence_number_;
  frame_buffer->cts_valid = true;

  if (forward_) {
    if (0 != yuv_file_input_.readYuvBuf(frame_buffer.get(), is_eof)) {
      LOG(ERROR) << "[FrameCapture] Read YUV file failed: " << yuv_file_input_.getLastError();
      return nullptr;
    }
    if (is_eof) {
      return nullptr;
    }
    sequence_number_++;
    return frame_buffer;
  }

  // Backward pass: read one frame at current position
  is_eof = false;
  if (0 != yuv_file_input_.readYuvBuf(frame_buffer.get(), is_eof)) {
    LOG(ERROR) << "[FrameCapture] Read YUV file failed (backward): " << yuv_file_input_.getLastError();
    return nullptr;
  }
  if (is_eof) {
    return nullptr;
  }
  sequence_number_++;

  // Move to previous frame for next read (avoid seeking before start)
  std::streampos pos_after_read = yuv_file_input_.tell();
  if (pos_after_read <= static_cast<std::streampos>(frame_size_bytes_)) {
    // Just read frame 0; next pass is forward from head
    yuv_file_input_.rewind();
    forward_ = true;
    LOG(INFO) << "[FrameCapture] Loop: switching to head→tail";
  } else {
    yuv_file_input_.seekBack(2 * frame_size_bytes_);
  }

  return frame_buffer;
}