#include "frame_capture.h"

#include <thread>
#include <chrono>

#include "log_system/log_system.h"

FrameCapture::FrameCapture()
    : width_(0),
      height_(0),
      sequence_number_(0) {
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
  yuv_file_input_.rewind();
  LOG(INFO) << "[FrameCapture] Loop: rewound input to first frame";
}

std::unique_ptr<YUVBuffer> FrameCapture::ReadNextFrame(bool& is_eof) {
  auto frame_buffer = std::make_unique<YUVBuffer>(width_, height_);
  frame_buffer->sequence_number = sequence_number_;
  frame_buffer->cts = sequence_number_;
  frame_buffer->cts_valid = true;

  if (0 != yuv_file_input_.readYuvBuf(frame_buffer.get(), is_eof)) {
    LOG(ERROR) << "[FrameCapture] Read YUV file failed: "
               << yuv_file_input_.getLastError();
    return nullptr;
  }
  if (is_eof) {
    return nullptr;
  }
  sequence_number_++;
  return frame_buffer;
}
