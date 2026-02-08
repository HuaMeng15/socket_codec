#include "mock_decoder.h"

#include <cstring>

#include "log_system/log_system.h"

MockDecoder::MockDecoder()
    : initialized_(false),
      width_(0),
      height_(0) {}

MockDecoder::~MockDecoder() { Cleanup(); }

int MockDecoder::Initialize(int width, int height) {
  if (initialized_) {
    LOG(WARNING) << "[MockDecoder] Already initialized";
    return 0;
  }

  width_ = width;
  height_ = height;

  LOG(INFO) << "[MockDecoder] Fake decoder: " << width << "x" << height
            << " (returns zero YUV, ignores input)";

  initialized_ = true;
  return 0;
}

YUVBuffer* MockDecoder::DecodeFrame(const uint8_t* /* frame_data */, size_t /* frame_size */) {
  if (!initialized_) {
    LOG(ERROR) << "[MockDecoder] Decoder not initialized";
    return nullptr;
  }

  YUVBuffer* yuv = new YUVBuffer(width_, height_);
  yuv->sequence_number = 0;
  yuv->cts_valid = false;

  // All planes are already allocated by YUVBuffer ctor; zero them
  for (int i = 0; i < 3; i++) {
    size_t plane_bytes = static_cast<size_t>(yuv->planes[i].stride) * yuv->planes[i].height;
    std::memset(yuv->planes[i].ptr, 0, plane_bytes);
  }

  return yuv;
}

void MockDecoder::ReleaseFrame(YUVBuffer* frame) {
  if (frame) {
    delete frame;
  }
}

void MockDecoder::Cleanup() {
  if (!initialized_) return;
  LOG(INFO) << "[MockDecoder] Cleaning up fake decoder";
  initialized_ = false;
}
