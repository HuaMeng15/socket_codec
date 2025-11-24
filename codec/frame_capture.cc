#include "frame_capture.h"

#include <thread>
#include <chrono>

#include "log_system/log_system.h"

FrameCapture::FrameCapture()
    : width_(0),
      height_(0),
      encoder_ready_(false),
      frame_ready_(false),
      stop_requested_(false),
      eof_reached_(false),
      frame_available_(false) {
  vvenc_YUVBuffer_default(&frame_buffer_);
}

FrameCapture::~FrameCapture() {
  Stop();
  
  // Only free buffer if it was allocated
  if (frame_buffer_.planes[0].ptr) {
    vvenc_YUVBuffer_free_buffer(&frame_buffer_);
    // Clear the pointer to avoid double free
    frame_buffer_.planes[0].ptr = nullptr;
    frame_buffer_.planes[1].ptr = nullptr;
    frame_buffer_.planes[2].ptr = nullptr;
  }
  
  if (yuv_file_input_.isOpen()) {
    yuv_file_input_.close();
  }
}

int FrameCapture::Initialize(const std::string& input_file, int width,
                              int height) {
  input_file_ = input_file;
  width_ = width;
  height_ = height;

  // Open input file
  if (0 != yuv_file_input_.open(input_file)) {
    error_message_ = "Failed to open input file: " + yuv_file_input_.getLastError();
    LOG(ERROR) << "[FrameCapture] " << error_message_;
    return -1;
  }

  // Allocate frame buffer
  vvenc_YUVBuffer_alloc_buffer(&frame_buffer_, kFileChromaFormat, width_, height_);

  encoder_ready_ = false;  // Start by signaling ready for first frame
  frame_ready_ = false;
  stop_requested_ = false;
  eof_reached_ = false;

  LOG(INFO) << "[FrameCapture] Initialized with file: " << input_file_
            << " resolution: " << width_ << "x" << height_;

  return 0;
}

void FrameCapture::Run() {
  LOG(INFO) << "[FrameCapture] Frame capture thread started";

  int64_t sequence_number = 0;

  while (!stop_requested_ && !eof_reached_) {
    // Wait for encoder to be ready
    {
      std::unique_lock<std::mutex> lock(mutex_);
      encoder_ready_cv_.wait(lock, [this]() {
        return encoder_ready_.load() || stop_requested_ || eof_reached_;
      });

      if (stop_requested_ || eof_reached_) {
        break;
      }
    }

    // TODO: later control fps interval
    // std::this_thread::sleep_for(std::chrono::seconds(1));

    // Read next frame
    bool bEof = false;
    if (0 != yuv_file_input_.readYuvBuf(frame_buffer_, bEof)) {
      error_message_ = "Read YUV file failed: " + yuv_file_input_.getLastError();
      LOG(ERROR) << "[FrameCapture] " << error_message_;
      stop_requested_ = true;
      break;
    }

    if (bEof) {
      LOG(INFO) << "[FrameCapture] Reached end of file";
      eof_reached_ = true;
      break;
    }

    // Signal that frame is ready
    {
      std::unique_lock<std::mutex> lock(mutex_);
      frame_available_ = true;
      frame_ready_ = true;
      encoder_ready_ = false;  // Wait for encoder to process
    }
    frame_ready_cv_.notify_one();

    LOG(INFO) << "[FrameCapture] Frame " << sequence_number << " ready";

    sequence_number++;
  }

  // Signal EOF to encoder
  {
    std::unique_lock<std::mutex> lock(mutex_);
    frame_ready_ = true;
    frame_available_ = false;  // No more frames
  }
  frame_ready_cv_.notify_one();

  LOG(INFO) << "[FrameCapture] Frame capture thread finished. Total frames: "
            << sequence_number;
}

void FrameCapture::SignalEncoderReady() {
  std::unique_lock<std::mutex> lock(mutex_);
  encoder_ready_ = true;
  frame_ready_ = false;
  frame_available_ = false;
  encoder_ready_cv_.notify_one();
}

vvencYUVBuffer* FrameCapture::WaitForFrame() {
  std::unique_lock<std::mutex> lock(mutex_);
  frame_ready_cv_.wait(lock, [this]() {
    return frame_ready_.load() || stop_requested_ || eof_reached_;
  });

  if (stop_requested_ || (eof_reached_ && !frame_available_)) {
    return nullptr;
  }

  if (frame_available_) {
    return &frame_buffer_;
  }

  return nullptr;
}

void FrameCapture::Stop() {
  std::unique_lock<std::mutex> lock(mutex_);
  stop_requested_ = true;
  encoder_ready_ = true;  // Wake up waiting threads
  frame_ready_ = true;
  encoder_ready_cv_.notify_all();
  frame_ready_cv_.notify_all();
}

bool FrameCapture::IsStopped() const { return stop_requested_.load(); }

bool FrameCapture::IsEof() const { return eof_reached_.load(); }

std::string FrameCapture::GetError() const { return error_message_; }

