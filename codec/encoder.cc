#include "encoder.h"

#include <chrono>
#include <cstddef>
#include <cstring>
#include <iostream>

#include "frame_capture.h"
#include "log_system/log_system.h"
#include "transmission/message_sender.h"

Encoder::Encoder()
    : frame_capture_(nullptr),
      message_sender_(nullptr),
      encoder_(nullptr),
      yuv_input_buffer_(),
      access_unit_(),
      output_stream_(nullptr),
      initialized_(false),
      sequence_number_(0),
      max_frames_(-1) {
  vvenc_YUVBuffer_default(&yuv_input_buffer_);
  vvenc_accessUnit_default(&access_unit_);
}

Encoder::~Encoder() { Cleanup(); }

int Encoder::Initialize(int width, int height, int fps, int framesToBeEncoded) {
  if (initialized_) {
    LOG(WARNING) << "[Encoder] Already initialized";
    return 0;
  }

  max_frames_ = framesToBeEncoded;

  LOG(INFO) << "[Encoder] Initializing encoder parameters";
  // Initialize encoder parameters
  InitializeEncoderParams(&params_, width, height, fps, framesToBeEncoded);

  // Initialize config parameters
  if (vvenc_init_config_parameter(&params_)) {
    LOG(ERROR) << "[Encoder] Failed to initialize config parameters";
    return -1;
  }

  // Create encoder
  encoder_ = vvenc_encoder_create();
  if (!encoder_) {
    LOG(ERROR) << "[Encoder] Failed to create encoder";
    return -1;
  }

  // Open encoder
  int iRet = vvenc_encoder_open(encoder_, &params_);
  if (0 != iRet) {
    LOG(ERROR) << "[Encoder] Failed to open encoder: " << iRet << " "
               << vvenc_get_last_error(encoder_);
    vvenc_encoder_close(encoder_);
    encoder_ = nullptr;
    return iRet;
  }

  // Get the adapted config
  vvenc_get_config(encoder_, &params_);
  LOG(VERBOSE) << "[Encoder] Adapted config";

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

void Encoder::SetOutputStream(std::ofstream* output_stream) {
  output_stream_ = output_stream;
}

void Encoder::SetMessageSender(MessageSender* message_sender) {
  message_sender_ = message_sender;
}

void Encoder::SetFrameCapture(FrameCapture* frame_capture) {
  frame_capture_ = frame_capture;
}

int Encoder::EncodeFrame(vvencYUVBuffer* input_buffer, bool& bEncodeDone) {
  if (!initialized_) {
    LOG(ERROR) << "[Encoder] Encoder not initialized";
    return -1;
  }

  if (!input_buffer) {
    LOG(ERROR) << "[Encoder] Input buffer is null";
    return -1;
  }

  // Set frame metadata
  input_buffer->sequenceNumber = sequence_number_;
  input_buffer->cts = sequence_number_;
  input_buffer->ctsValid = true;

  auto start_time = std::chrono::high_resolution_clock::now();
  int iRet = vvenc_encode(encoder_, input_buffer, &access_unit_, &bEncodeDone);
  if (0 != iRet) {
    LOG(ERROR) << "[Encoder] Encoding failed: " << iRet << " "
               << vvenc_get_last_error(encoder_);
    return iRet;
  }
  if (access_unit_.payloadUsedSize > 0) {
    WriteEncodedData(start_time);
  }

  sequence_number_++;

  return 0;
}

void Encoder::Run() {
  if (!initialized_) {
    LOG(ERROR) << "[Encoder] Encoder not initialized";
    return;
  }

  if (!frame_capture_) {
    LOG(ERROR) << "[Encoder] FrameCapture not set";
    return;
  }

  LOG(INFO) << "[Encoder] Encoder thread started";

  sequence_number_ = 0;

  // Signal ready for first frame
  SignalReadyForNextFrame();

  while (!stop_requested_) {
    // Wait for frame from frame capture
    vvencYUVBuffer* frame_buffer = frame_capture_->WaitForFrame();

    if (!frame_buffer) {
      if (frame_capture_->IsEof()) {
        LOG(INFO) << "[Encoder] EOF reached, flushing encoder";
      } else if (frame_capture_->IsStopped()) {
        LOG(INFO) << "[Encoder] Frame capture stopped";
      }
      break;
    }

    // Copy frame data to encoder's buffer
    CopyFrameBuffer(frame_buffer);

    // Encode the frame
    bool bEncodeDone = false;
    LOG(INFO) << "[Encoder] Encoding frame: " << sequence_number_;
    int iRet = EncodeFrame(frame_buffer, bEncodeDone);
    if (0 != iRet) {
      LOG(ERROR) << "[Encoder] Encoding failed: " << iRet;
      break;
    }

    // Check if encoding is complete or max frames reached
    bool should_stop = false;
    if (bEncodeDone) {
      LOG(INFO) << "[Encoder] Encoding completed (bEncodeDone=true)";
      should_stop = true;
    } else if (max_frames_ > 0 && sequence_number_ >= max_frames_) {
      LOG(INFO) << "[Encoder] Max frames limit reached: " << max_frames_;
      should_stop = true;
    }

    if (should_stop) {
      // Flush encoder before stopping to ensure all data is processed
      LOG(INFO) << "[Encoder] Flushing encoder before stopping";

      // Stop frame capture since we're done encoding
      if (frame_capture_) {
        frame_capture_->Stop();
      }
      break;
    }

    // Signal ready for next frame (this allows frame capture to read next frame)
    SignalReadyForNextFrame();
  }

  LOG(INFO) << "[Encoder] Encoder thread finished. Total frames encoded: "
            << sequence_number_;
  
  // Print summary right after encoding is complete, while encoder is still valid
  PrintSummary();
}

void Encoder::SignalReadyForNextFrame() {
  // Check if frame_capture_ is still valid before accessing
  FrameCapture* fc = frame_capture_;
  if (fc) {
    fc->SignalEncoderReady();
  }
}

void Encoder::Stop() {
  stop_requested_ = true;
}

bool Encoder::IsStopped() const {
  return stop_requested_.load();
}

void Encoder::CopyFrameBuffer(const vvencYUVBuffer* source) {
  if (!source) {
    return;
  }

  // Copy metadata
  yuv_input_buffer_.sequenceNumber = source->sequenceNumber;
  yuv_input_buffer_.cts = source->cts;
  yuv_input_buffer_.ctsValid = source->ctsValid;

  // Copy plane data (Y, U, V)
  for (int i = 0; i < 3; i++) {
    const vvencYUVPlane& src_plane = source->planes[i];
    vvencYUVPlane& dst_plane = yuv_input_buffer_.planes[i];

    if (src_plane.ptr && dst_plane.ptr) {
      // Calculate size to copy
      size_t plane_size = src_plane.height * src_plane.stride * sizeof(int16_t);
      std::memcpy(dst_plane.ptr, src_plane.ptr, plane_size);
    }
  }
}

void Encoder::WriteEncodedData(
    const std::chrono::high_resolution_clock::time_point& start_time) {
  if (access_unit_.payloadUsedSize > 0) {
    // Write to file if output stream is set
    if (output_stream_ && output_stream_->is_open()) {
      output_stream_->write((const char*)access_unit_.payload,
                            access_unit_.payloadUsedSize);
      if (output_stream_->fail()) {
        LOG(ERROR) << "[Encoder] write bitstream file failed (disk full?)";
      }
    }

    // Send via network if message sender is set
    if (message_sender_ && message_sender_->IsInitialized()) {
      int ret = message_sender_->SendData(
          access_unit_.payload,
          access_unit_.payloadUsedSize,
          static_cast<uint32_t>(sequence_number_));
      if (ret != 0) {
        LOG(ERROR) << "[Encoder] Failed to send encoded data via network";
      }
    }

    LOG(INFO) << "[Encoder] Write encoded AU of size: "
              << access_unit_.payloadUsedSize << " bytes"
              << " kbps: " << access_unit_.payloadUsedSize * 240 / 1000;
    auto end_time = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> encode_duration =
        end_time - start_time;
    LOG(INFO) << "[Encoder] Encoding Frame " << sequence_number_
              << " time (ms): " << encode_duration.count();
  }
}

void Encoder::Cleanup() {
  // Make cleanup idempotent - safe to call multiple times
  // If encoder_ is already nullptr, we've already cleaned up
  if (!encoder_ && !initialized_) {
    return;  // Already cleaned up
  }

  // Clear references first to avoid dangling pointers
  frame_capture_ = nullptr;
  message_sender_ = nullptr;

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

void Encoder::PrintSummary() const {
  if (!encoder_) {
    LOG(WARNING) << "[Encoder] Cannot print summary: encoder is null";
    return;
  }

  if (!initialized_) {
    LOG(WARNING) << "[Encoder] Cannot print summary: encoder not initialized";
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

void Encoder::InitializeEncoderParams(vvenc_config* params, int width,
                                       int height, int fps,
                                       int framesToBeEncoded) { /* vvenc real time configurations */
  // Initialize with default parameters
  vvenc_init_default(params, width, height, fps, VVENC_RC_OFF, VVENC_AUTO_QP,
                        vvencPresetMode::VVENC_FAST);
  params->m_internChromaFormat = kFileChromaFormat;
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

