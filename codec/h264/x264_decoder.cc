#include "x264_decoder.h"

#include <cstring>

#include "log_system/log_system.h"

X264Decoder::X264Decoder()
    : codec_context_(nullptr),
      codec_(nullptr),
      frame_(nullptr),
      packet_(nullptr),
      initialized_(false),
      width_(0),
      height_(0) {
}

X264Decoder::~X264Decoder() {
  Cleanup();
}

int X264Decoder::Initialize(int width, int height) {
  if (initialized_) {
    LOG(WARNING) << "[X264Decoder] Already initialized";
    return 0;
  }

  width_ = width;
  height_ = height;

  LOG(INFO) << "[X264Decoder] Initializing H.264 decoder with resolution: " << width << "x" << height;

  // Find H.264 decoder
  codec_ = avcodec_find_decoder(AV_CODEC_ID_H264);
  if (!codec_) {
    LOG(ERROR) << "[X264Decoder] H.264 codec not found";
    return -1;
  }

  // Allocate codec context
  codec_context_ = avcodec_alloc_context3(codec_);
  if (!codec_context_) {
    LOG(ERROR) << "[X264Decoder] Failed to allocate codec context";
    return -1;
  }

  // Set codec parameters
  codec_context_->width = width;
  codec_context_->height = height;
  codec_context_->pix_fmt = AV_PIX_FMT_YUV420P;

  // Open codec
  int ret = avcodec_open2(codec_context_, codec_, nullptr);
  if (ret < 0) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
    LOG(ERROR) << "[X264Decoder] Failed to open codec: " << errbuf;
    avcodec_free_context(&codec_context_);
    return -1;
  }

  // Allocate frame
  frame_ = av_frame_alloc();
  if (!frame_) {
    LOG(ERROR) << "[X264Decoder] Failed to allocate frame";
    avcodec_free_context(&codec_context_);
    return -1;
  }

  // Allocate packet
  packet_ = av_packet_alloc();
  if (!packet_) {
    LOG(ERROR) << "[X264Decoder] Failed to allocate packet";
    av_frame_free(&frame_);
    avcodec_free_context(&codec_context_);
    return -1;
  }

  initialized_ = true;

  LOG(INFO) << "[X264Decoder] H.264 decoder initialized successfully";

  return 0;
}

YUVBuffer* X264Decoder::ConvertAVFrameToYUVBuffer(AVFrame* frame) {
  if (!frame || frame->format != AV_PIX_FMT_YUV420P) {
    LOG(WARNING) << "[X264Decoder] Invalid frame or unsupported pixel format";
    return nullptr;
  }

  YUVBuffer* yuv_buffer = new YUVBuffer();
  yuv_buffer->sequence_number = frame->pts != AV_NOPTS_VALUE ? frame->pts : 0;
  yuv_buffer->cts = frame->pts != AV_NOPTS_VALUE ? frame->pts : 0;
  yuv_buffer->cts_valid = (frame->pts != AV_NOPTS_VALUE);

  // Allocate memory for YUV planes
  // Y plane
  int y_size = frame->width * frame->height;
  uint8_t* y_data = new uint8_t[y_size];
  for (int y = 0; y < frame->height; y++) {
    memcpy(y_data + y * frame->width,
           frame->data[0] + y * frame->linesize[0],
           frame->width);
  }
  yuv_buffer->planes[0].ptr = y_data;
  yuv_buffer->planes[0].stride = frame->width;
  yuv_buffer->planes[0].width = frame->width;
  yuv_buffer->planes[0].height = frame->height;

  // U plane
  int u_size = (frame->width / 2) * (frame->height / 2);
  uint8_t* u_data = new uint8_t[u_size];
  for (int y = 0; y < frame->height / 2; y++) {
    memcpy(u_data + y * (frame->width / 2),
           frame->data[1] + y * frame->linesize[1],
           frame->width / 2);
  }
  yuv_buffer->planes[1].ptr = u_data;
  yuv_buffer->planes[1].stride = frame->width / 2;
  yuv_buffer->planes[1].width = frame->width / 2;
  yuv_buffer->planes[1].height = frame->height / 2;

  // V plane
  int v_size = (frame->width / 2) * (frame->height / 2);
  uint8_t* v_data = new uint8_t[v_size];
  for (int y = 0; y < frame->height / 2; y++) {
    memcpy(v_data + y * (frame->width / 2),
           frame->data[2] + y * frame->linesize[2],
           frame->width / 2);
  }
  yuv_buffer->planes[2].ptr = v_data;
  yuv_buffer->planes[2].stride = frame->width / 2;
  yuv_buffer->planes[2].width = frame->width / 2;
  yuv_buffer->planes[2].height = frame->height / 2;

  return yuv_buffer;
}

YUVBuffer* X264Decoder::DecodeFrame(const uint8_t* frame_data, size_t frame_size) {
  if (!initialized_ || !codec_context_ || !packet_ || !frame_) {
    LOG(ERROR) << "[X264Decoder] Decoder not properly initialized";
    return nullptr;
  }

  // Prepare packet
  av_packet_unref(packet_);
  packet_->data = const_cast<uint8_t*>(frame_data);
  packet_->size = frame_size;

  // Send packet to decoder
  int ret = avcodec_send_packet(codec_context_, packet_);
  if (ret < 0) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
    LOG(WARNING) << "[X264Decoder] Error sending packet: " << errbuf;
    return nullptr;
  }

  // Receive frame
  ret = avcodec_receive_frame(codec_context_, frame_);
  if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
    // Need more input or end of stream
    return nullptr;
  } else if (ret < 0) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
    LOG(WARNING) << "[X264Decoder] Error receiving frame: " << errbuf;
    return nullptr;
  }

  // Convert to YUVBuffer
  return ConvertAVFrameToYUVBuffer(frame_);
}

void X264Decoder::ReleaseFrame(YUVBuffer* frame) {
  if (frame) {
    // Free allocated plane data
    for (int i = 0; i < 3; i++) {
      if (frame->planes[i].ptr) {
        delete[] frame->planes[i].ptr;
        frame->planes[i].ptr = nullptr;
      }
    }
    delete frame;
  }
}

void X264Decoder::Cleanup() {
  if (!initialized_) {
    return;
  }

  LOG(INFO) << "[X264Decoder] Cleaning up decoder";

  // Free FFmpeg resources
  if (packet_) {
    av_packet_free(&packet_);
    packet_ = nullptr;
  }

  if (frame_) {
    av_frame_free(&frame_);
    frame_ = nullptr;
  }

  if (codec_context_) {
    avcodec_free_context(&codec_context_);
    codec_context_ = nullptr;
  }

  codec_ = nullptr;

  initialized_ = false;
}

