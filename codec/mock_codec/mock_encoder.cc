#include "mock_encoder.h"

#include <chrono>
#include <cstring>

#include "config/config.h"
#include "log_system/log_system.h"

int64_t MockEncoder::NowMs() const {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

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

  // Read the encoder-mode config (see config.cc). Static mode leaves
  // variable_mode_ = false and the encoder always fills the target.
  auto& parser = CmdLineParser::GetInstance();
  variable_mode_ = parser.GetFlag<int>("encoder_variable_mode") != 0;
  alr_low_ratio_ = parser.GetFlag<int>("encoder_alr_low_ratio_x100") / 100.0;
  period_ms_ = parser.GetFlag<int>("encoder_period_ms");
  alr_fraction_ = parser.GetFlag<int>("encoder_alr_fraction_x100") / 100.0;

  LOG(INFO) << "[MockEncoder] Fake encoder: " << width << "x" << height
            << " fps=" << fps << " target_bitrate=" << target_bitrate_kbps_
            << " kbps -> " << bytes_per_frame_ << " bytes/frame";
  if (variable_mode_) {
    LOG(INFO) << "[MockEncoder] VARIABLE mode: period=" << period_ms_
              << "ms alr_fraction=" << alr_fraction_
              << " low_ratio=" << alr_low_ratio_;
  } else {
    LOG(INFO) << "[MockEncoder] STATIC mode (always fills CC target)";
  }
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

  // Decide this frame's size. Static mode always emits the full target. In
  // variable mode the "content demand" follows a duty cycle: full target for
  // the high portion of each period, then alr_low_ratio_ x target for the low
  // (application-limited) portion — simulating a simple scene the encoder
  // can't fill the target with.
  size_t frame_bytes = bytes_per_frame_;
  if (variable_mode_ && period_ms_ > 0) {
    if (first_frame_time_ms_ < 0) first_frame_time_ms_ = NowMs();
    int64_t phase_ms = (NowMs() - first_frame_time_ms_) % period_ms_;
    // Low phase is the final alr_fraction_ portion of each period.
    bool low = phase_ms >= period_ms_ * (1.0 - alr_fraction_);
    if (low) {
      frame_bytes = static_cast<size_t>(bytes_per_frame_ * alr_low_ratio_);
      if (frame_bytes == 0) frame_bytes = 1;
    }
    if (low != in_low_phase_) {
      in_low_phase_ = low;
      LOG(INFO) << "[MockEncoder] Content phase -> "
                << (low ? "LOW (app-limited)" : "HIGH (full rate)")
                << " frame_bytes=" << frame_bytes;
    }
  }

  uint8_t* data = new uint8_t[frame_bytes];
  std::memset(data, 0, frame_bytes);

  auto encoded_data = std::make_unique<EncodedData>();
  encoded_data->sequence_number = sequence_number_;
  encoded_data->AddData(data, frame_bytes);

  if (output_stream_ && output_stream_->is_open()) {
    output_stream_->write(reinterpret_cast<const char*>(data), frame_bytes);
  }

  LOG(INFO) << "[MockEncoder] Fake frame " << sequence_number_
            << " size=" << frame_bytes << " bytes (zeros)";

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
