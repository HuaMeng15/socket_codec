#include "mock_encoder.h"

#include <cstring>

#include "log_system/log_system.h"

MockEncoder::MockEncoder()
    : output_stream_(nullptr),
      initialized_(false),
      sequence_number_(0),
      width_(0),
      height_(0),
      fps_(0),
      target_bitrate_kbps_(kDefaultTargetBitrateKbps),
      bytes_per_frame_(0) {}

MockEncoder::~MockEncoder() { Cleanup(); }

int MockEncoder::Initialize(int width, int height, int fps, int /* framesToBeEncoded */) {
  if (initialized_) {
    LOG(WARNING) << "[MockEncoder] Already initialized";
    return 0;
  }

  width_ = width;
  height_ = height;
  fps_ = fps;
  // bytes_per_frame = (target_bitrate_kbps * 1000 / 8) / fps
  bytes_per_frame_ = static_cast<size_t>(target_bitrate_kbps_) * 1000 / (8 * fps_);
  if (bytes_per_frame_ == 0) {
    bytes_per_frame_ = 1;
  }

  LOG(INFO) << "[MockEncoder] Fake encoder: " << width << "x" << height
            << " fps=" << fps << " target_bitrate=" << target_bitrate_kbps_
            << " kbps -> " << bytes_per_frame_ << " bytes/frame";
  LOG(INFO) << "[Encoder] Initial bitrate " << target_bitrate_kbps_ << " kbps";

  initialized_ = true;
  sequence_number_ = 0;
  return 0;
}

void MockEncoder::SetTargetBitrate(int bitrate_kbps) {
  if (!initialized_) {
    LOG(ERROR) << "[MockEncoder] Encoder not initialized";
    return;
  }
  if (bitrate_kbps == target_bitrate_kbps_) {
    return;  // Same as last time, ignore
  }

  target_bitrate_kbps_ = bitrate_kbps;
  // A real encoder targets the full allocated bitrate; size frames to match it
  // exactly (no headroom factor).
  bytes_per_frame_ = static_cast<size_t>(target_bitrate_kbps_) * 1000 / (8 * fps_);
  if (bytes_per_frame_ == 0) bytes_per_frame_ = 1;

  LOG(INFO) << "[Encoder] Set target bitrate to " << bitrate_kbps << " kbps"
            << " -> " << bytes_per_frame_ << " bytes/frame";
}

void MockEncoder::SetOutputStream(std::ofstream* output_stream) {
  output_stream_ = output_stream;
}

std::unique_ptr<EncodedData> MockEncoder::EncodeFrame(YUVBuffer* /* input_buffer */) {
  if (!initialized_) {
    LOG(ERROR) << "[MockEncoder] Encoder not initialized";
    return nullptr;
  }

  uint8_t* data = new uint8_t[bytes_per_frame_];
  std::memset(data, 0, bytes_per_frame_);

  auto encoded_data = std::make_unique<EncodedData>();
  encoded_data->sequence_number = sequence_number_;
  encoded_data->AddData(data, bytes_per_frame_);

  if (output_stream_ && output_stream_->is_open()) {
    output_stream_->write(reinterpret_cast<const char*>(data), bytes_per_frame_);
  }

  LOG(INFO) << "[MockEncoder] Fake frame " << sequence_number_
            << " size=" << bytes_per_frame_ << " bytes (zeros)";

  sequence_number_++;
  return encoded_data;
}

void MockEncoder::Cleanup() {
  if (!initialized_) return;
  output_stream_ = nullptr;
  initialized_ = false;
}

void MockEncoder::PrintSummary() const {
  if (!initialized_) {
    LOG(WARNING) << "[MockEncoder] Cannot print summary: not initialized";
    return;
  }
  LOG(INFO) << "[MockEncoder] Summary: Fake encoded " << sequence_number_ << " frames";
}
