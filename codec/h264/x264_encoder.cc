#include "x264_encoder.h"

#include <chrono>
#include <cstring>

#include "config/config.h"
#include "log_system/log_system.h"

// Startup default; overridden by SetTargetBitrate(cc_initial) before the first
// frame. Aligned with the GCC/pacer startup rate.
static const int INITIAL_BITRATE = kDefaultInitialBitrateKbps;
static const int SLICE_MAX_SIZE = 0;
static const double BANDWIDTH_UTILIZATION = 0.9;
static const double VBV_NORMAL_RATIO = 0.5;   // normal: vbv_buffer = this * bitrate (kbits)
// VBV_REDUCED_RATIO and VBV_RECOVERY_FRAMES removed - now using network_usage_state_


X264Encoder::X264Encoder()
    : encoder_(nullptr),
      output_stream_(nullptr),
      initialized_(false),
      target_bitrate_kbps_(INITIAL_BITRATE),
      sequence_number_(0),
      width_(0),
      height_(0),
      fps_(0),
      network_usage_state_(0.0) {
  memset(&params_, 0, sizeof(params_));
  memset(&pic_in_, 0, sizeof(pic_in_));
  memset(&pic_out_, 0, sizeof(pic_out_));
}

X264Encoder::~X264Encoder() { Cleanup(); }

int X264Encoder::Initialize(int width, int height, int fps, int /* framesToBeEncoded */) {
  if (initialized_) {
    LOG(WARNING) << "[X264Encoder] Already initialized";
    return 0;
  }

  width_ = width;
  height_ = height;
  fps_ = fps;

  LOG(INFO) << "[X264Encoder] Initializing encoder with resolution: " << width << "x" << height << " fps: " << fps;

  x264_param_default(&params_);

  if (x264_param_default_preset(&params_, "superfast", "zerolatency") < 0) {
    LOG(ERROR) << "[X264Encoder] x264_param_default_preset failed";
    return -1;
  }

  /* Configure non-default params */
  params_.i_threads = 1;
  params_.b_sliced_threads = 1;
  params_.i_width = width;
  params_.i_height = height;
  params_.i_frame_total = 0;
  params_.i_keyint_max = 1500;
  params_.i_bitdepth = 8;
  params_.i_csp = X264_CSP_I420;
  params_.b_repeat_headers = 1;
  params_.i_log_level = X264_LOG_INFO;
  params_.i_slice_max_size = SLICE_MAX_SIZE;

  // ABR
  params_.rc.i_rc_method = X264_RC_ABR;
  double bitrate = INITIAL_BITRATE * BANDWIDTH_UTILIZATION;
  params_.rc.i_bitrate = bitrate;

  params_.rc.i_vbv_max_bitrate = bitrate;

  params_.rc.i_vbv_buffer_size = static_cast<int>(bitrate * VBV_NORMAL_RATIO);

  LOG(INFO) << "bitrate: " << params_.rc.i_bitrate << " vbv_max_bitrate: " << params_.rc.i_vbv_max_bitrate << " vbv_buffer_size: " << params_.rc.i_vbv_buffer_size;

  // param.rc.b_filler = 1;

  params_.i_bframe = 0;
  params_.b_open_gop = 0;
  params_.i_fps_den = 1;
  params_.i_fps_num = fps_;
  params_.b_vfr_input = 0;
  params_.b_cabac = 1;  // 0 for CAVLC， 1 for higher complexity

  encoder_ = x264_encoder_open(&params_);
  if (!encoder_) {
    LOG(ERROR) << "[X264Encoder] Failed to create encoder";
    return -1;
  }

  // Allocate picture buffer
  x264_picture_alloc(&pic_in_, X264_CSP_I420, width, height);

  initialized_ = true;
  sequence_number_ = 0;

  LOG(INFO) << "[X264Encoder] Encoder initialized successfully";
  LOG(INFO) << "[Encoder] Initial bitrate " << INITIAL_BITRATE << " kbps";
  return 0;
}

void X264Encoder::SetOutputStream(std::ofstream* output_stream) {
  output_stream_ = output_stream;
}

void X264Encoder::SetTargetBitrate(int bitrate_kbps) {
  if (!initialized_) {
    LOG(ERROR) << "[X264Encoder] Encoder not initialized";
    return;
  }

  if (bitrate_kbps == target_bitrate_kbps_) {
    return;  // Same as last time, ignore
  }

  target_bitrate_kbps_ = bitrate_kbps;
  double bitrate = bitrate_kbps * BANDWIDTH_UTILIZATION;

  params_.rc.i_bitrate = static_cast<int>(bitrate);
  params_.rc.i_vbv_max_bitrate = static_cast<int>(bitrate);

  // VBV adaptation based on network usage state (sparkrtc-aligned):
  // When network_usage_state >= 2.0 (overuse), use tight VBV = bitrate/fps
  // Otherwise (normal/underuse), use relaxed VBV = bitrate/2
  double vbv_ratio;
  const double kOveruseThreshold = 2.0;

  if (network_usage_state_ >= kOveruseThreshold) {
    // Overuse: tight VBV for fast adaptation (sparkrtc: bitrate/fps)
    vbv_ratio = 1.0 / fps_;
    params_.rc.i_vbv_buffer_size = static_cast<int>(bitrate * vbv_ratio);
    if (params_.rc.i_vbv_buffer_size < 1) params_.rc.i_vbv_buffer_size = 1;
    LOG(INFO) << "[Encoder] Set target bitrate to " << bitrate_kbps
              << " kbps (OVERUSE VBV=" << params_.rc.i_vbv_buffer_size
              << " kbits, usage_state=" << network_usage_state_ << ")";
  } else {
    // Normal/underuse: relaxed VBV (sparkrtc: bitrate/2)
    vbv_ratio = 0.5;
    params_.rc.i_vbv_buffer_size = static_cast<int>(bitrate * vbv_ratio);
    if (params_.rc.i_vbv_buffer_size < 1) params_.rc.i_vbv_buffer_size = 1;
    LOG(INFO) << "[Encoder] Set target bitrate to " << bitrate_kbps
              << " kbps (NORMAL VBV=" << params_.rc.i_vbv_buffer_size
              << " kbits, usage_state=" << network_usage_state_ << ")";
  }

  x264_encoder_reconfig(encoder_, &params_);
}

void X264Encoder::SetNetworkUsageState(double network_usage_state) {
  // Store the network usage state from GCC for VBV adaptation in SetTargetBitrate.
  // This is called by VideoCaptureAndSend before each SetTargetBitrate call.
  network_usage_state_ = network_usage_state;
}

std::unique_ptr<EncodedData> X264Encoder::EncodeFrame(YUVBuffer* input_buffer) {
  if (!initialized_) {
    LOG(ERROR) << "[X264Encoder] Encoder not initialized";
    return nullptr;
  }

  if (!input_buffer) {
    LOG(ERROR) << "[X264Encoder] Input buffer is null";
    return nullptr;
  }

  // Copy YUV data from input buffer to x264 picture
  // x264 expects Y, U, V planes in separate buffers
  for (int i = 0; i < 3; i++) {
    const YUVBuffer::Plane& src_plane = input_buffer->planes[i];
    uint8_t* dst_plane = pic_in_.img.plane[i];

    if (src_plane.ptr && dst_plane) {
      int plane_height = (i == 0) ? height_ : height_ / 2;
      int plane_width = (i == 0) ? width_ : width_ / 2;
      
      for (int y = 0; y < plane_height; y++) {
        std::memcpy(dst_plane + y * pic_in_.img.i_stride[i],
                   src_plane.ptr + y * src_plane.stride,
                   plane_width);
      }
    }
  }

  pic_in_.i_pts = sequence_number_;
  pic_in_.i_type = X264_TYPE_AUTO;

  auto start_time = std::chrono::high_resolution_clock::now();
  x264_nal_t* nals;
  int i_nals;
  int frame_size = x264_encoder_encode(encoder_, &nals, &i_nals, &pic_in_, &pic_out_);

  if (frame_size < 0) {
    LOG(ERROR) << "[X264Encoder] Encoding failed";
    return nullptr;
  }

  if (frame_size == 0) {
    return nullptr;
  }

  // VBV adaptation is now handled in SetTargetBitrate based on network_usage_state_
  // (no more frame-count-based recovery logic)

  auto encoded_data = std::make_unique<EncodedData>();
  encoded_data->sequence_number = sequence_number_;
  const size_t kMaxNalPayload = 50 * 1024 * 1024;  // 50 MB sanity limit
  for (int i = 0; i < i_nals; i++) {
    int payload = nals[i].i_payload;
    if (payload <= 0 || nals[i].p_payload == nullptr ||
        static_cast<size_t>(payload) > kMaxNalPayload) {
      LOG(ERROR) << "[X264Encoder] Invalid NAL " << i << " payload=" << payload << ", skip";
      continue;
    }
    uint8_t* nal_copy = new uint8_t[payload];
    std::memcpy(nal_copy, nals[i].p_payload, payload);
    encoded_data->AddData(nal_copy, payload);
  }
  if (encoded_data->data_ptrs.empty()) {
    LOG(ERROR) << "[X264Encoder] No valid NALs";
    return nullptr;
  }

  // Write to file if output stream is set (for debugging)
  for (int i = 0; i < i_nals; i++) {
    // Write to file if output stream is set (for debugging)
    if (output_stream_ && output_stream_->is_open()) {
      output_stream_->write((const char*)nals[i].p_payload, nals[i].i_payload);
      if (output_stream_->fail()) {
        LOG(ERROR) << "[X264Encoder] write bitstream file failed (disk full?)";
      }
    }
  }

  auto end_time = std::chrono::high_resolution_clock::now();
  std::chrono::duration<double, std::milli> encode_duration = end_time - start_time;
  LOG(INFO) << "[X264Encoder] Encoded frame " << sequence_number_
            << " size=" << encoded_data->size << " bytes"
            << " size in kbps=" << encoded_data->size * 8 * fps_ / 1000 << " kbps"
            << " time=" << encode_duration.count() << " ms";

  sequence_number_++;
  return encoded_data;
}

void X264Encoder::Cleanup() {
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
}

void X264Encoder::PrintSummary() const {
  if (!encoder_) {
    LOG(WARNING) << "[X264Encoder] Cannot print summary: encoder is null";
    return;
  }

  if (!initialized_) {
    LOG(WARNING) << "[X264Encoder] Cannot print summary: encoder not initialized";
    return;
  }

  LOG(INFO) << "[X264Encoder] Summary: Encoded " << sequence_number_ << " frames";
}

