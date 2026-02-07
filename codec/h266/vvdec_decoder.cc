#ifdef VVENC

#include "vvdec_decoder.h"

#include <cstring>

#include "log_system/log_system.h"

#define MAX_CODED_PICTURE_SIZE 800000

VVdecDecoder::VVdecDecoder()
    : decoder_(nullptr),
      initialized_(false) {
  vvdec_accessUnit_default(&access_unit_);
}

VVdecDecoder::~VVdecDecoder() {
  Cleanup();
}

int VVdecDecoder::Initialize(int width, int height) {
  if (initialized_) {
    LOG(WARNING) << "[VVdecDecoder] Already initialized";
    return 0;
  }

  LOG(INFO) << "[VVdecDecoder] Initializing decoder with resolution: " << width << "x" << height;

  // Initialize decoder parameters
  vvdec_params_default(&params_);
  params_.logLevel = VVDEC_NOTICE;
  params_.enable_realtime = true;

  // Open decoder
  decoder_ = vvdec_decoder_open(&params_);
  if (decoder_ == nullptr) {
    LOG(ERROR) << "[VVdecDecoder] Failed to open decoder";
    return -1;
  }

  // Allocate access unit for decoding
  vvdec_accessUnit_alloc_payload(&access_unit_, MAX_CODED_PICTURE_SIZE);
  if (access_unit_.payload == nullptr) {
    LOG(ERROR) << "[VVdecDecoder] Failed to allocate access unit payload";
    vvdec_decoder_close(decoder_);
    decoder_ = nullptr;
    return -1;
  }
  access_unit_.cts = 0;
  access_unit_.ctsValid = true;
  access_unit_.dts = 0;
  access_unit_.dtsValid = true;

  initialized_ = true;

  LOG(VERBOSE) << "[VVdecDecoder] Decoder initialized successfully";

  return 0;
}

YUVBuffer* VVdecDecoder::ConvertVVdecFrame(vvdecFrame* vvdec_frame) {
  if (!vvdec_frame) {
    return nullptr;
  }

  YUVBuffer* yuv_buffer = new YUVBuffer();
  yuv_buffer->sequence_number = vvdec_frame->sequenceNumber;
  yuv_buffer->cts = vvdec_frame->sequenceNumber;  // Use sequence number as CTS
  yuv_buffer->cts_valid = true;

  for (int i = 0; i < 3 && i < static_cast<int>(vvdec_frame->numPlanes); i++) {
    yuv_buffer->planes[i].ptr = const_cast<uint8_t*>(vvdec_frame->planes[i].ptr);
    yuv_buffer->planes[i].stride = vvdec_frame->planes[i].stride;
    yuv_buffer->planes[i].width = vvdec_frame->planes[i].width;
    yuv_buffer->planes[i].height = vvdec_frame->planes[i].height;
  }

  // Store conversion for cleanup
  frame_conversions_[vvdec_frame] = yuv_buffer;

  return yuv_buffer;
}

YUVBuffer* VVdecDecoder::DecodeFrame(const uint8_t* frame_data, size_t frame_size) {
  if (!decoder_ || !initialized_) {
    LOG(ERROR) << "[VVdecDecoder] Decoder not properly initialized";
    return nullptr;
  }

  // Copy frame data to access unit
  if (frame_size > MAX_CODED_PICTURE_SIZE) {
    LOG(ERROR) << "[VVdecDecoder] Frame size " << frame_size 
               << " exceeds maximum " << MAX_CODED_PICTURE_SIZE;
    return nullptr;
  }

  std::memcpy(access_unit_.payload, frame_data, frame_size);
  access_unit_.payloadUsedSize = frame_size;

  vvdecNalType eNalType = vvdec_get_nal_unit_type(&access_unit_);
  bool bIsSlice = vvdec_is_nal_unit_slice(eNalType);

  // Decode
  vvdecFrame* decoded_vvdec_frame = nullptr;
  int ret = vvdec_decode(decoder_, &access_unit_, &decoded_vvdec_frame);

  if (bIsSlice) {
    access_unit_.cts++;
    access_unit_.dts++;
  }

  // Check result
  if (ret == VVDEC_OK && decoded_vvdec_frame != nullptr) {
    // Convert to YUVBuffer
    YUVBuffer* yuv_buffer = ConvertVVdecFrame(decoded_vvdec_frame);
    // Note: decoded_vvdec_frame will be released when ReleaseFrame is called
    return yuv_buffer;
  } else if (ret == VVDEC_TRY_AGAIN) {
    LOG(WARNING) << "[VVdecDecoder] More data needed to decode frame (VVDEC_TRY_AGAIN)";
    return nullptr;
  } else if (ret == VVDEC_EOF) {
    LOG(INFO) << "[VVdecDecoder] End of stream (VVDEC_EOF)";
    return nullptr;
  } else {
    LOG(ERROR) << "[VVdecDecoder] Decoding failed: " << vvdec_get_error_msg(ret);
    if (decoder_) {
      std::string err = vvdec_get_last_error(decoder_);
      if (!err.empty()) {
        LOG(ERROR) << "[VVdecDecoder] Error details: " << err;
      }
    }
    return nullptr;
  }
}

void VVdecDecoder::ReleaseFrame(YUVBuffer* frame) {
  if (frame) {
    // Find and release the corresponding vvdecFrame
    vvdecFrame* vvdec_frame = nullptr;
    for (auto it = frame_conversions_.begin(); it != frame_conversions_.end(); ++it) {
      if (it->second == frame) {
        vvdec_frame = it->first;
        frame_conversions_.erase(it);
        break;
      }
    }
    
    // Release vvdecFrame if found
    if (vvdec_frame && decoder_) {
      vvdec_frame_unref(decoder_, vvdec_frame);
    }
    
    delete frame;
  }
}

void VVdecDecoder::Cleanup() {
  if (!decoder_ && !initialized_) {
    return;
  }

  LOG(INFO) << "[VVdecDecoder] Cleaning up decoder";

  if (decoder_) {
    vvdec_decoder_close(decoder_);
    decoder_ = nullptr;
  }

  // Free access unit
  if (access_unit_.payload) {
    vvdec_accessUnit_free_payload(&access_unit_);
    access_unit_.payload = nullptr;
  }

  // Clean up frame conversions
  for (auto& pair : frame_conversions_) {
    if (decoder_) {
      vvdec_frame_unref(decoder_, pair.first);
    }
    delete pair.second;
  }
  frame_conversions_.clear();

  initialized_ = false;
}

#endif  // VVENC

