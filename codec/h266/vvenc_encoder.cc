#ifdef VVENC

#include "vvenc_encoder.h"

#include <chrono>
#include <cstddef>
#include <cstring>

#include "log_system/log_system.h"

VVencEncodedData::VVencEncodedData() {
  data = nullptr;
  size = 0;
  sequence_number = 0;
}

VVencEncodedData::~VVencEncodedData() {
  // data_storage_ will be automatically cleaned up
}

void VVencEncodedData::SetData(const uint8_t* au_data, size_t au_size) {
  if (au_data && au_size > 0) {
    data_storage_.assign(au_data, au_data + au_size);
    data = data_storage_.data();
    size = data_storage_.size();
  } else {
    data = nullptr;
    size = 0;
  }
}

VVencEncoder::VVencEncoder()
    : encoder_(nullptr),
      yuv_input_buffer_(),
      access_unit_(),
      output_stream_(nullptr),
      initialized_(false),
      sequence_number_(0) {
  vvenc_YUVBuffer_default(&yuv_input_buffer_);
  vvenc_accessUnit_default(&access_unit_);
}

VVencEncoder::~VVencEncoder() { Cleanup(); }

int VVencEncoder::Initialize(int width, int height, int fps, int framesToBeEncoded) {
  if (initialized_) {
    LOG(WARNING) << "[VVencEncoder] Already initialized";
    return 0;
  }

  LOG(INFO) << "[VVencEncoder] Initializing encoder parameters";
  // Initialize encoder parameters
  InitializeEncoderParams(&params_, width, height, fps, framesToBeEncoded);

  // Initialize config parameters
  if (vvenc_init_config_parameter(&params_)) {
    LOG(ERROR) << "[VVencEncoder] Failed to initialize config parameters";
    return -1;
  }

  // Create encoder
  encoder_ = vvenc_encoder_create();
  if (!encoder_) {
    LOG(ERROR) << "[VVencEncoder] Failed to create encoder";
    return -1;
  }

  // Open encoder
  int iRet = vvenc_encoder_open(encoder_, &params_);
  if (0 != iRet) {
    LOG(ERROR) << "[VVencEncoder] Failed to open encoder: " << iRet << " "
               << vvenc_get_last_error(encoder_);
    vvenc_encoder_close(encoder_);
    encoder_ = nullptr;
    return iRet;
  }

  // Get the adapted config
  vvenc_get_config(encoder_, &params_);
  LOG(VERBOSE) << "[VVencEncoder] Adapted config";

  // Allocate and initialize the YUV Buffer
  vvenc_YUVBuffer_alloc_buffer(&yuv_input_buffer_, params_.m_internChromaFormat,
                                params_.m_SourceWidth, params_.m_SourceHeight);

  // Allocate and initialize the access unit storage for output packets
  const int auSizeScale =
      params_.m_internChromaFormat <= VVENC_CHROMA_420 ? 2 : 3;
  vvenc_accessUnit_alloc_payload(
      &access_unit_,
      auSizeScale * params_.m_SourceWidth * params_.m_SourceHeight + 1024);

  initialized_ = true;
  return 0;
}

void VVencEncoder::SetOutputStream(std::ofstream* output_stream) {
  output_stream_ = output_stream;
}

void VVencEncoder::ConvertYUVBufferToVVenc(const YUVBuffer* source, vvencYUVBuffer* dest) {
  if (!source || !dest) {
    return;
  }

  // Copy metadata
  dest->sequenceNumber = source->sequence_number;
  dest->cts = source->cts;
  dest->ctsValid = source->cts_valid;

  // Convert plane data (Y, U, V) from uint8_t* to int16_t*
  for (int i = 0; i < 3; i++) {
    const YUVBuffer::Plane& src_plane = source->planes[i];
    vvencYUVPlane& dst_plane = dest->planes[i];

    if (src_plane.ptr && dst_plane.ptr) {
      // Convert uint8_t to int16_t line by line
      for (int y = 0; y < src_plane.height; y++) {
        const uint8_t* src_line = src_plane.ptr + y * src_plane.stride;
        int16_t* dst_line = dst_plane.ptr + y * dst_plane.stride;
        
        for (int x = 0; x < src_plane.width; x++) {
          dst_line[x] = static_cast<int16_t>(src_line[x]);
        }
      }
    }
  }
}

std::unique_ptr<EncodedData> VVencEncoder::EncodeFrame(YUVBuffer* input_buffer) {
  if (!initialized_) {
    LOG(ERROR) << "[VVencEncoder] Encoder not initialized";
    return nullptr;
  }

  if (!input_buffer) {
    LOG(ERROR) << "[VVencEncoder] Input buffer is null";
    return nullptr;
  }

  // Convert YUVBuffer to vvencYUVBuffer (uint8_t* to int16_t*)
  yuv_input_buffer_.sequenceNumber = input_buffer->sequence_number;
  yuv_input_buffer_.cts = input_buffer->cts;
  yuv_input_buffer_.ctsValid = input_buffer->cts_valid;

  // Convert plane data from uint8_t to int16_t
  for (int i = 0; i < 3; i++) {
    const YUVBuffer::Plane& src_plane = input_buffer->planes[i];
    vvencYUVPlane& dst_plane = yuv_input_buffer_.planes[i];

    if (src_plane.ptr && dst_plane.ptr) {
      // Convert uint8_t to int16_t line by line
      for (int y = 0; y < src_plane.height; y++) {
        const uint8_t* src_line = src_plane.ptr + y * src_plane.stride;
        int16_t* dst_line = dst_plane.ptr + y * dst_plane.stride;
        
        for (int x = 0; x < src_plane.width; x++) {
          dst_line[x] = static_cast<int16_t>(src_line[x]);
        }
      }
    }
  }

  auto start_time = std::chrono::high_resolution_clock::now();
  bool bEncodeDone = false;
  int iRet = vvenc_encode(encoder_, &yuv_input_buffer_, &access_unit_, &bEncodeDone);
  if (0 != iRet) {
    LOG(ERROR) << "[VVencEncoder] Encoding failed: " << iRet << " "
               << vvenc_get_last_error(encoder_);
    return nullptr;
  }

  if (access_unit_.payloadUsedSize == 0) {
    // No output yet (encoder may need more frames)
    return nullptr;
  }

  // Create encoded data object
  auto encoded_data = std::make_unique<VVencEncodedData>();
  encoded_data->sequence_number = sequence_number_;
  encoded_data->SetData(access_unit_.payload, access_unit_.payloadUsedSize);

  // Write to file if output stream is set (for debugging)
  WriteEncodedDataToFile(encoded_data->data, encoded_data->size);

  auto end_time = std::chrono::high_resolution_clock::now();
  std::chrono::duration<double, std::milli> encode_duration = end_time - start_time;
  LOG(INFO) << "[VVencEncoder] Encoded frame " << sequence_number_
            << " size=" << encoded_data->size << " bytes"
            << " time=" << encode_duration.count() << " ms";

  sequence_number_++;
  return encoded_data;
}

void VVencEncoder::WriteEncodedDataToFile(const uint8_t* data, size_t size) {
  if (size > 0 && output_stream_ && output_stream_->is_open()) {
    output_stream_->write((const char*)data, size);
    if (output_stream_->fail()) {
      LOG(ERROR) << "[VVencEncoder] write bitstream file failed (disk full?)";
    }
  }
}

void VVencEncoder::Cleanup() {
  // Make cleanup idempotent - safe to call multiple times
  // If encoder_ is already nullptr, we've already cleaned up
  if (!encoder_ && !initialized_) {
    return;  // Already cleaned up
  }

  if (encoder_) {
    vvenc_encoder_close(encoder_);
    encoder_ = nullptr;
  }

  // Only free buffers if they were allocated
  if (yuv_input_buffer_.planes[0].ptr) {
    vvenc_YUVBuffer_free_buffer(&yuv_input_buffer_);
    // Clear pointers after freeing to avoid double-free
    yuv_input_buffer_.planes[0].ptr = nullptr;
    yuv_input_buffer_.planes[1].ptr = nullptr;
    yuv_input_buffer_.planes[2].ptr = nullptr;
  }
  
  if (access_unit_.payload) {
    vvenc_accessUnit_free_payload(&access_unit_);
    access_unit_.payload = nullptr;
  }

  initialized_ = false;
}

void VVencEncoder::PrintSummary() const {
  if (!encoder_) {
    LOG(WARNING) << "[VVencEncoder] Cannot print summary: encoder is null";
    return;
  }

  if (!initialized_) {
    LOG(WARNING) << "[VVencEncoder] Cannot print summary: encoder not initialized";
    return;
  }

  std::cout.flush();
  std::cerr.flush();
  
  // Try calling the summary function - it may print to stdout or stderr
  // TODO: print summary not output content
  vvenc_print_summary(encoder_);
  std::cout.flush();
  std::cerr.flush();
}

void VVencEncoder::InitializeEncoderParams(vvenc_config* params, int width,
                                       int height, int fps,
                                       int framesToBeEncoded) { /* vvenc real time configurations */
  // Initialize with default parameters
  vvenc_init_default(params, width, height, fps, VVENC_RC_OFF, VVENC_AUTO_QP,
                        vvencPresetMode::VVENC_FAST);
  params->m_internChromaFormat = VVENC_CHROMA_420;
  params->m_framesToBeEncoded = framesToBeEncoded;

  params->m_IntraPeriod = -1;
  params->m_DecodingRefreshType = VVENC_DRT_NONE;
  params->m_GOPSize = 1;
  params->m_sliceTypeAdapt = false;
  params->m_CTUSize = 64;
  params->m_poc0idr = 1;
  params->m_intraQPOffset = -1;
  params->m_picReordering = false;
  params->m_numThreads = 0;  // auto
  params->m_SearchRange = 64;
  params->m_numRefPics = 0;
  params->m_RDOQ = 2;
  params->m_SignDataHidingEnabled = true;
  params->m_maxMTTDepthI = 0;
  params->m_maxNumMergeCand = 4;
  params->m_Affine = 0;
  params->m_alfSpeed = 2;
  params->m_allowDisFracMMVD = 0;
  params->m_BDOF = 0;
  params->m_DepQuantEnabled = false;
  params->m_AMVRspeed = 0;
  params->m_JointCbCrMode = 0;
  params->m_LFNST = 0;
  params->m_vvencMCTF.MCTFSpeed = 4;
  params->m_vvencMCTF.MCTFFutureReference = 0;
  params->m_MMVD = 0;
  params->m_MRL = 0;
  params->m_PROF = 0;
  params->m_saoScc = 0;
  params->m_bUseSAO = false;
  params->m_SbTMVP = 0;
  params->m_IBCFastMethod = 6;
  params->m_TSsize = 3;
  params->m_qtbttSpeedUp = 7;
  params->m_usePbIntraFast = 2;
  params->m_fastHad = 1;
  params->m_FastInferMerge = 4;
  params->m_bIntegerET = 1;
  params->m_IntraEstDecBit = 3;
  params->m_useSelectiveRDOQ = 2;
  params->m_FirstPassMode = 4;
  params->m_AccessUnitDelimiter = 1;
  params->m_lumaReshapeEnable = 0;
  params->m_internalBitDepth[0] = 8;
  params->m_internalBitDepth[1] = 8;

  // Disable LookAhead for real-time encoding (it causes frame buffering)
  params->m_LookAhead = 0;

  // Configure for real-time low-latency encoding: output frames immediately
  params->m_maxParallelFrames =
      0;  // Disable parallel frame processing to enable immediate output
  params->m_leadFrames = 0;   // No lead frames buffering
  params->m_trailFrames = 0;  // No trail frames buffering

  // Configure GOP list
  for (int i = 0; i < 1; i++) {
    params->m_GOPList[i].m_sliceType = 'B';
    params->m_GOPList[i].m_POC = i + 1;
    params->m_GOPList[i].m_QPOffset = 5;
    params->m_GOPList[i].m_QPOffsetModelOffset = -6.5;
    params->m_GOPList[i].m_QPOffsetModelScale = 0.2590;
    params->m_GOPList[i].m_CbQPoffset = 0;
    params->m_GOPList[i].m_CrQPoffset = 0;
    params->m_GOPList[i].m_QPFactor = 1.0;
    params->m_GOPList[i].m_tcOffsetDiv2 = 0;
    params->m_GOPList[i].m_betaOffsetDiv2 = 0;
    int ref_num = 1;
    params->m_GOPList[i].m_numRefPicsActive[0] = ref_num;
    params->m_GOPList[i].m_numRefPicsActive[1] = 0;
    params->m_GOPList[i].m_numRefPics[0] = ref_num;
    params->m_GOPList[i].m_numRefPics[1] = 0;
    params->m_GOPList[i].m_deltaRefPics[0][0] = 1;
    params->m_GOPList[i].m_deltaRefPics[0][1] = 9;
    params->m_GOPList[i].m_deltaRefPics[0][2] = 17;
    params->m_GOPList[i].m_deltaRefPics[0][3] = 25;
    params->m_GOPList[i].m_deltaRefPics[1][0] = 1;
    params->m_GOPList[i].m_deltaRefPics[1][1] = 9;
    params->m_GOPList[i].m_deltaRefPics[1][2] = 17;
    params->m_GOPList[i].m_deltaRefPics[1][3] = 25;
    params->m_GOPList[i].m_temporalId = 0;
  }
}

#endif  // VVENC

