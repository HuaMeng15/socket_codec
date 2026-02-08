#include "x264_encoder.h"

#include <chrono>
#include <cstring>

#include "log_system/log_system.h"

static const int INITIAL_BITRATE = 10000; // 10M
static const int SLICE_MAX_SIZE = 0;

X264Encoder::X264Encoder()
    : encoder_(nullptr),
      output_stream_(nullptr),
      initialized_(false),
      sequence_number_(0),
      width_(0),
      height_(0),
      fps_(0) {
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

  x264_param_default_preset(&params_, "superfast", "zerolatency");

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
  params_.i_log_level = X264_LOG_DEBUG;
  params_.i_log_level = X264_LOG_INFO;
  params_.i_slice_max_size = SLICE_MAX_SIZE;

  // ABR
  params_.rc.i_rc_method = X264_RC_ABR;
  params_.rc.i_bitrate = INITIAL_BITRATE;

  params_.rc.i_vbv_max_bitrate = INITIAL_BITRATE;

  params_.rc.i_vbv_buffer_size = INITIAL_BITRATE * 0.5; // kbit / 8 * 1000 = byte

  // param.rc.b_filler = 1;

  params_.i_bframe = 0;
  params_.b_open_gop = 0;
  params_.i_fps_den = 1;
  params_.i_fps_num = fps_;
  params_.b_vfr_input = 0;
  params_.b_cabac = 1;  // 0 for CAVLC， 1 for higher complexity

  // Open encoder
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

  params_.rc.i_bitrate = bitrate_kbps;
  params_.rc.i_vbv_max_bitrate = bitrate_kbps;
  params_.rc.i_vbv_buffer_size = bitrate_kbps * 0.5; // kbit / 8 * 1000 = byte

  LOG(INFO) << "[X264Encoder] Set target bitrate to " << bitrate_kbps << " kbps";
  x264_encoder_reconfig(encoder_, &params_);
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

  auto encoded_data = std::make_unique<EncodedData>();
  encoded_data->sequence_number = sequence_number_;
  for (int i = 0; i < i_nals; i++) {
    // Copy NAL data (x264 owns the original pointers, we need our own copy)
    uint8_t* nal_copy = new uint8_t[nals[i].i_payload];
    std::memcpy(nal_copy, nals[i].p_payload, nals[i].i_payload);
    encoded_data->AddData(nal_copy, nals[i].i_payload);
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

