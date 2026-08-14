#include "slice_paced_encoder.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include "config/config.h"
#include "log_system/log_system.h"

namespace {
constexpr double kBandwidthUtilization = 0.9;
constexpr double kVbvNormalRatio = 0.5;
constexpr double kOveruseThreshold = 2.0;
constexpr int kDefaultSliceCount = 9;

void AppendEncodedData(EncodedData* dst, EncodedData* src) {
  if (!dst || !src) return;
  for (size_t i = 0; i < src->data_ptrs.size(); i++) {
    dst->AddData(src->data_ptrs[i], src->data_sizes[i]);
    src->data_ptrs[i] = nullptr;
  }
  src->data_ptrs.clear();
  src->data_sizes.clear();
  src->size = 0;
}
}  // namespace

SlicePacedEncoder::SlicePacedEncoder()
    : encoder_(nullptr),
      output_stream_(nullptr),
      initialized_(false),
      frame_in_progress_(false),
      width_(0),
      height_(0),
      fps_(0),
      slice_count_(kDefaultSliceCount),
      sequence_number_(0),
      target_bitrate_kbps_(kDefaultInitialBitrateKbps),
      network_usage_state_(0.0),
      frame_start_effective_bitrate_kbps_(
          std::max(1, static_cast<int>(kDefaultInitialBitrateKbps *
                                       kBandwidthUtilization))),
      slice_rc_active_(false) {
  std::memset(&params_, 0, sizeof(params_));
  std::memset(&pic_in_, 0, sizeof(pic_in_));
  std::memset(&pic_out_, 0, sizeof(pic_out_));
}

SlicePacedEncoder::~SlicePacedEncoder() { Cleanup(); }

int SlicePacedEncoder::Initialize(int width, int height, int fps,
                                  int /*framesToBeEncoded*/) {
  if (initialized_) {
    LOG(WARNING) << "[SlicePacedEncoder] Already initialized";
    return 0;
  }

  width_ = width;
  height_ = height;
  fps_ = fps;

  if (x264_param_default_preset(&params_, "superfast", "zerolatency") < 0) {
    LOG(ERROR) << "[SlicePacedEncoder] x264_param_default_preset failed";
    return -1;
  }

  params_.i_threads = 1;
  params_.b_sliced_threads = 0;
  params_.i_slice_count = slice_count_;
  params_.i_width = width;
  params_.i_height = height;
  params_.i_frame_total = 0;
  params_.i_keyint_max = 1500;
  params_.i_bitdepth = 8;
  params_.i_csp = X264_CSP_I420;
  params_.b_repeat_headers = 1;
  params_.i_log_level = X264_LOG_INFO;
  params_.i_bframe = 0;
  params_.b_open_gop = 0;
  params_.i_fps_den = 1;
  params_.i_fps_num = fps_;
  params_.b_vfr_input = 0;

  params_.rc.i_rc_method = X264_RC_ABR;
  ApplyBitrateReconfig(target_bitrate_kbps_.load());

  x264_param_apply_profile(&params_, "baseline");

  encoder_ = x264_encoder_open(&params_);
  if (!encoder_) {
    LOG(ERROR) << "[SlicePacedEncoder] Failed to create encoder";
    return -1;
  }

  x264_picture_alloc(&pic_in_, X264_CSP_I420, width, height);

  initialized_ = true;
  sequence_number_ = 0;
  LOG(INFO) << "[SlicePacedEncoder] Initialized " << width << "x" << height
            << " fps=" << fps << " slices=" << slice_count_;
  return 0;
}

void SlicePacedEncoder::SetOutputStream(std::ofstream* output_stream) {
  output_stream_ = output_stream;
}

void SlicePacedEncoder::SetTargetBitrate(int bitrate_kbps) {
  if (bitrate_kbps <= 0) return;
  int old = target_bitrate_kbps_.exchange(bitrate_kbps);
  if (old != bitrate_kbps) {
    LOG(INFO) << "[SlicePacedEncoder] Target bitrate now "
              << bitrate_kbps << " kbps";
  }
}

void SlicePacedEncoder::SetNetworkUsageState(double network_usage_state) {
  network_usage_state_.store(network_usage_state);
}

void SlicePacedEncoder::ApplyBitrateReconfig(int bitrate_kbps) {
  int effective_bitrate = std::max(1, static_cast<int>(
      bitrate_kbps * kBandwidthUtilization));
  params_.rc.i_bitrate = effective_bitrate;
  params_.rc.i_vbv_max_bitrate = effective_bitrate;

  double vbv_ratio = network_usage_state_.load() >= kOveruseThreshold
                         ? 1.0 / std::max(1, fps_)
                         : kVbvNormalRatio;
  params_.rc.i_vbv_buffer_size =
      std::max(1, static_cast<int>(effective_bitrate * vbv_ratio));

  if (encoder_) {
    x264_encoder_reconfig(encoder_, &params_);
  }
}

bool SlicePacedEncoder::ShouldUseSliceRateControl() const {
  std::cout << "[SlicePacedEncoder] Network usage state: "
            << network_usage_state_.load() << std::endl;
  std::cout << "[SlicePacedEncoder] Overuse threshold: " << kOveruseThreshold
            << std::endl;
  return network_usage_state_.load() >= kOveruseThreshold;
}

void SlicePacedEncoder::CopyInputToPicture(YUVBuffer* input_buffer) {
  for (int i = 0; i < 3; i++) {
    const YUVBuffer::Plane& src_plane = input_buffer->planes[i];
    uint8_t* dst_plane = pic_in_.img.plane[i];
    if (!src_plane.ptr || !dst_plane) continue;

    int plane_height = (i == 0) ? height_ : height_ / 2;
    int plane_width = (i == 0) ? width_ : width_ / 2;
    for (int y = 0; y < plane_height; y++) {
      std::memcpy(dst_plane + y * pic_in_.img.i_stride[i],
                  src_plane.ptr + y * src_plane.stride,
                  plane_width);
    }
  }
}

bool SlicePacedEncoder::StartFrame(YUVBuffer* input_buffer) {
  if (!initialized_ || !input_buffer || frame_in_progress_) {
    return false;
  }

  int current_bitrate = std::max(1, target_bitrate_kbps_.load());
  ApplyBitrateReconfig(current_bitrate);
  frame_start_effective_bitrate_kbps_ = std::max(1, params_.rc.i_bitrate);
  slice_rc_active_ = false;

  CopyInputToPicture(input_buffer);
  pic_in_.i_pts = sequence_number_;
  pic_in_.i_type = X264_TYPE_AUTO;

  if (x264_encoder_start_frame(encoder_, &pic_in_) < 0) {
    LOG(ERROR) << "[SlicePacedEncoder] Failed to start frame "
               << sequence_number_;
    return false;
  }

  frame_in_progress_ = true;
  return true;
}


std::unique_ptr<EncodedData> SlicePacedEncoder::EncodeSlice(int slice_idx) {
  if (!frame_in_progress_ || slice_idx < 0 || slice_idx >= slice_count_) {
    return nullptr;
  }

  int current_bitrate = std::max(1, target_bitrate_kbps_.load());
  int rc_bitrate = frame_start_effective_bitrate_kbps_;
  int slice_rc_target = -1;
  bool use_slice_rc = ShouldUseSliceRateControl();
  if (use_slice_rc) {
    ApplyBitrateReconfig(current_bitrate);
    rc_bitrate = std::max(1, params_.rc.i_bitrate);
    slice_rc_target = rc_bitrate;
  } else if (slice_rc_active_) {
    rc_bitrate = std::max(1, params_.rc.i_bitrate);
  }
  slice_rc_active_ = use_slice_rc;

  x264_nal_t* nals = nullptr;
  int nal_count = 0;
  auto start_time = std::chrono::high_resolution_clock::now();
  int slice_size = x264_encoder_encode_slice(encoder_, &nals, &nal_count,
                                             slice_idx, -1, slice_rc_target);
  if (slice_size < 0) {
    LOG(ERROR) << "[SlicePacedEncoder] Failed to encode slice " << slice_idx;
    return nullptr;
  }

  auto encoded_data = std::make_unique<EncodedData>();
  encoded_data->sequence_number = sequence_number_;
  for (int i = 0; i < nal_count; i++) {
    int payload = nals[i].i_payload;
    if (payload <= 0 || !nals[i].p_payload) continue;
    uint8_t* nal_copy = new uint8_t[payload];
    std::memcpy(nal_copy, nals[i].p_payload, payload);
    encoded_data->AddData(nal_copy, payload);
    if (output_stream_ && output_stream_->is_open()) {
      output_stream_->write(reinterpret_cast<const char*>(nals[i].p_payload),
                            payload);
    }
  }

  auto end_time = std::chrono::high_resolution_clock::now();
  std::chrono::duration<double, std::milli> duration = end_time - start_time;
  LOG(INFO) << "[SlicePacedEncoder] Frame " << sequence_number_
            << " slice=" << slice_idx
            << " bitrate=" << current_bitrate
            << " rc_mode=" << (slice_rc_active_ ? "slice" : "frame")
            << " rc_bitrate=" << rc_bitrate
            << " usage_state=" << network_usage_state_.load()
            << " size=" << encoded_data->size
            << " bytes time=" << duration.count() << " ms";

  if (encoded_data->data_ptrs.empty()) {
    return nullptr;
  }
  return encoded_data;
}

bool SlicePacedEncoder::FinishFrame() {
  if (!frame_in_progress_) {
    return false;
  }

  if (x264_encoder_finish_frame(encoder_, &pic_out_) < 0) {
    LOG(ERROR) << "[SlicePacedEncoder] Failed to finish frame "
               << sequence_number_;
    frame_in_progress_ = false;
    return false;
  }

  LOG(INFO) << "[SlicePacedEncoder] Finished frame " << sequence_number_;
  sequence_number_++;
  frame_in_progress_ = false;
  return true;
}

std::unique_ptr<EncodedData> SlicePacedEncoder::EncodeFrame(
    YUVBuffer* input_buffer) {
  if (!StartFrame(input_buffer)) {
    return nullptr;
  }

  auto frame = std::make_unique<EncodedData>();
  frame->sequence_number = sequence_number_;
  for (int i = 0; i < slice_count_; i++) {
    auto slice = EncodeSlice(i);
    if (!slice) {
      return nullptr;
    }
    AppendEncodedData(frame.get(), slice.get());
  }

  if (!FinishFrame()) {
    return nullptr;
  }
  return frame;
}

void SlicePacedEncoder::Cleanup() {
  if (!encoder_ && !initialized_) {
    return;
  }

  if (encoder_) {
    x264_encoder_close(encoder_);
    encoder_ = nullptr;
  }

  if (pic_in_.img.plane[0]) {
    x264_picture_clean(&pic_in_);
  }

  initialized_ = false;
  frame_in_progress_ = false;
  slice_rc_active_ = false;
}

void SlicePacedEncoder::PrintSummary() const {
  LOG(INFO) << "[SlicePacedEncoder] Summary: encoded "
            << sequence_number_ << " frames";
}
