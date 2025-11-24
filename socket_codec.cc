#include <unistd.h>

#include "config/config.h"
#include "log_system/log_system.h"
#include "receiver.h"
#include "sender.h"
#include "tools/yuv_file_io.h"
#include "vvenc/vvenc.h"
#include "vvenc/vvencCfg.h"

int main(int argc, char* argv[]) {
  // Parse command-line arguments
  auto& parser = CmdLineParser::GetInstance();
  parser.Parse(argc, argv);

  std::string filename = parser.GetFlag<std::string>("file");
  if (filename == "NONE") {
    LOG(INFO) << "[socket_codec_main] Running in sender mode";
  } else {
    LOG(INFO)
        << "[socket_codec_main] Running in receiver mode, saving to file: "
        << filename;
  }

  std::string input_video_file =
      parser.GetFlag<std::string>("input_video_file");
  std::string output_video_file =
      parser.GetFlag<std::string>("output_video_file");
  // FILE *input_yuv_file = fopen(input_video_file.c_str(), "rb");
  // FILE *encoded_file_out = fopen(output_video_file.c_str(), "wb");

  // if (!input_yuv_file || !encoded_file_out) {
  //     std::cout << "fopen failed input_yuv_file:" << !input_yuv_file << "
  //     encoded_file_out:" << !encoded_file_out << std::endl; return -1;
  // }

  // open output file
  std::ofstream cOutBitstream;
  cOutBitstream.open(output_video_file,
                     std::ios::out | std::ios::binary | std::ios::trunc);
  if (!cOutBitstream.is_open()) {
    LOG(ERROR) << "[socket_codec_main] Failed to open output file "
               << output_video_file;
    return -1;
  }

  // Initialize the encoder
  vvenc_config params;
  // vvencPresetMode preset = VVENC_FAST;

  // vvenc_init_default(&params, 1920, 1080, 30, 1000000, 20, preset);
  vvenc_init_default(&params, 1920, 1080, 60, VVENC_RC_OFF, VVENC_AUTO_QP,
                     vvencPresetMode::VVENC_FAST);
  params.m_internChromaFormat = VVENC_CHROMA_420;
  params.m_framesToBeEncoded = 10;

  params.m_IntraPeriod = -1;
  params.m_DecodingRefreshType = VVENC_DRT_NONE;
  params.m_GOPSize = 1;
  params.m_sliceTypeAdapt = false;
  params.m_CTUSize = 64;
  params.m_poc0idr = 1;
  params.m_intraQPOffset = -1;
  params.m_picReordering = false;
  params.m_numThreads = 0;  // auto
  params.m_SearchRange = 64;
  params.m_numRefPics = 0;
  params.m_RDOQ = 2;
  params.m_SignDataHidingEnabled = true;
  params.m_maxMTTDepthI = 0;
  params.m_maxNumMergeCand = 4;
  params.m_Affine = 0;
  params.m_alfSpeed = 2;
  params.m_allowDisFracMMVD = 0;
  params.m_BDOF = 0;
  params.m_DepQuantEnabled = false;
  params.m_AMVRspeed = 0;
  params.m_JointCbCrMode = 0;
  params.m_LFNST = 0;
  params.m_vvencMCTF.MCTFSpeed = 4;
  params.m_vvencMCTF.MCTFFutureReference = 0;
  params.m_MMVD = 0;
  params.m_MRL = 0;
  params.m_PROF = 0;
  params.m_saoScc = 0;
  params.m_bUseSAO = false;
  params.m_SbTMVP = 0;
  params.m_IBCFastMethod = 6;
  params.m_TSsize = 3;
  params.m_qtbttSpeedUp = 7;
  params.m_usePbIntraFast = 2;
  params.m_fastHad = 1;
  params.m_FastInferMerge = 4;
  params.m_bIntegerET = 1;
  params.m_IntraEstDecBit = 3;
  params.m_useSelectiveRDOQ = 2;
  params.m_FirstPassMode = 4;
  params.m_AccessUnitDelimiter = 1;
  params.m_lumaReshapeEnable = 0;

  // Disable LookAhead for real-time encoding (it causes frame buffering)
  params.m_LookAhead = 0;
  
  // Configure for real-time low-latency encoding: output frames immediately
  params.m_maxParallelFrames =
      0;  // Disable parallel frame processing to enable immediate output
  params.m_leadFrames = 0;   // No lead frames buffering
  params.m_trailFrames = 0;  // No trail frames buffering

  // Configure tiles to split frames into multiple slices for packet loss
  // resilience Each tile becomes a separate slice, reducing the impact of
  // packet loss Adjust these values based on your desired slice size:
  // - More tiles = smaller slices = better packet loss resilience but
  // slightly lower compression efficiency
  // - For 1920x1080: 4x2 tiles = 8 slices per frame (each slice ~240x540
  // pixels)
  // - For lower resolutions or smaller desired slice sizes, increase tile
  // count params.m_picPartitionFlag = true;  // Enable picture partitioning
  // (tiles) params.m_numTileCols = 4;  // Number of tile columns
  // (horizontal splits) params.m_numTileRows = 2;  // Number of tile rows
  // (vertical splits) This creates 4x2 = 8 tiles/slices per frame

  for (int i = 0; i < 1; i++) {
    params.m_GOPList[i].m_sliceType = 'B';
    params.m_GOPList[i].m_POC = i + 1;
    params.m_GOPList[i].m_QPOffset = 5;
    params.m_GOPList[i].m_QPOffsetModelOffset = -6.5;
    params.m_GOPList[i].m_QPOffsetModelScale = 0.2590;
    params.m_GOPList[i].m_CbQPoffset = 0;
    params.m_GOPList[i].m_CrQPoffset = 0;
    params.m_GOPList[i].m_QPFactor = 1.0;
    params.m_GOPList[i].m_tcOffsetDiv2 = 0;
    params.m_GOPList[i].m_betaOffsetDiv2 = 0;
    int ref_num = 1;
    params.m_GOPList[i].m_numRefPicsActive[0] = ref_num;
    params.m_GOPList[i].m_numRefPicsActive[1] = 0;
    params.m_GOPList[i].m_numRefPics[0] = ref_num;
    params.m_GOPList[i].m_numRefPics[1] = 0;
    params.m_GOPList[i].m_deltaRefPics[0][0] = 1;
    params.m_GOPList[i].m_deltaRefPics[0][1] = 9;
    params.m_GOPList[i].m_deltaRefPics[0][2] = 17;
    params.m_GOPList[i].m_deltaRefPics[0][3] = 25;
    params.m_GOPList[i].m_deltaRefPics[1][0] = 1;
    params.m_GOPList[i].m_deltaRefPics[1][1] = 9;
    params.m_GOPList[i].m_deltaRefPics[1][2] = 17;
    params.m_GOPList[i].m_deltaRefPics[1][3] = 25;
    params.m_GOPList[i].m_temporalId = 0;
  }

  if (vvenc_init_config_parameter(&params)) {
    return -1;
  }

  vvencEncoder* encoder = vvenc_encoder_create();
  int iRet = vvenc_encoder_open(encoder, &params);
  if (0 != iRet) {
    LOG(ERROR) << "[socket_codec_main] Failed to open encoder: " << iRet
               << " " << vvenc_get_last_error(encoder);
    vvenc_encoder_close(encoder);
    return iRet;
  }

  // get the adapted config
  vvenc_get_config(encoder, &params);

  // Allocate and initialize the YUV Buffer
  vvencYUVBuffer cYUVInputBuffer;
  vvenc_YUVBuffer_default(&cYUVInputBuffer);
  vvenc_YUVBuffer_alloc_buffer(&cYUVInputBuffer, params.m_internChromaFormat,
                               params.m_SourceWidth, params.m_SourceHeight);

  // Allocate and initialize the access unit storage for output packets
  vvencAccessUnit AU;
  vvenc_accessUnit_default(&AU);
  const int auSizeScale =
      params.m_internChromaFormat <= VVENC_CHROMA_420 ? 2 : 3;
  vvenc_accessUnit_alloc_payload(
      &AU, auSizeScale * params.m_SourceWidth * params.m_SourceHeight + 1024);

  YuvFileIO cYuvFileInput;

  if (0 != cYuvFileInput.open(input_video_file)) {
    LOG(ERROR) << "[socket_codec_main] Open input file failed: "
               << cYuvFileInput.getLastError();
    vvenc_YUVBuffer_free_buffer(&cYUVInputBuffer);
    vvenc_accessUnit_free_payload(&AU);
    vvenc_encoder_close(encoder);
    return -1;
  }

  int64_t iSeqNumber = 0;
  int64_t iShowNumber = 0;
  bool bEncodeDone = false;
  bool bEof = false;
  const int64_t iMaxFrames  = params.m_framesToBeEncoded;

  while (!bEof || !bEncodeDone) {
    auto start_time = std::chrono::high_resolution_clock::now();

    vvencYUVBuffer* ptrYUVInputBuffer = nullptr;
    if (!bEof) {
      if (0 != cYuvFileInput.readYuvBuf(cYUVInputBuffer, bEof)) {
        LOG(ERROR) << "[socket_codec_main] Read YUV file failed: "
                   << cYuvFileInput.getLastError();
        vvenc_YUVBuffer_free_buffer(&cYUVInputBuffer);
        vvenc_accessUnit_free_payload(&AU);
        vvenc_encoder_close(encoder);
        return -1;
      }
      if (!bEof) {
        cYUVInputBuffer.sequenceNumber = iSeqNumber;
        cYUVInputBuffer.cts = iSeqNumber;
        cYUVInputBuffer.ctsValid = true;
        ptrYUVInputBuffer = &cYUVInputBuffer;
        iSeqNumber++;
      }
    }

    // call encode
    LOG(INFO) << "[socket_codec_main] Encoding frame: " << iSeqNumber;
    iRet = vvenc_encode(encoder, ptrYUVInputBuffer, &AU, &bEncodeDone);
    if (0 != iRet) {
      LOG(ERROR) << "[socket_codec_main] Encoding failed" << iRet
                 << vvenc_get_last_error(encoder);
      vvenc_YUVBuffer_free_buffer(&cYUVInputBuffer);
      vvenc_accessUnit_free_payload(&AU);
      vvenc_encoder_close(encoder);
      return iRet;
    }

    if (AU.payloadUsedSize > 0) {
      if (cOutBitstream.is_open()) {
        // write output
        cOutBitstream.write((const char*)AU.payload, AU.payloadUsedSize);
        LOG(INFO) << "[socket_codec_main] Write encoded AU of size: "
                  << AU.payloadUsedSize << " bytes"
                  << " kbps: " << AU.payloadUsedSize * 240 / 1000
                  << " iShowNumber: " << iShowNumber;
        auto end_time = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double, std::milli> encode_duration =
            end_time - start_time;
        LOG(INFO) << "[socket_codec_main] Encoding Frame " << iShowNumber
                  << " time (ms): " << encode_duration.count();
        iShowNumber++;
        if (cOutBitstream.fail()) {
          LOG(ERROR)
              << "[socket_codec_main] write bitstream file failed (disk full?)";
          vvenc_YUVBuffer_free_buffer(&cYUVInputBuffer);
          vvenc_accessUnit_free_payload(&AU);
          vvenc_encoder_close(encoder);
          return -1;
        }
      }
    }
    if (iMaxFrames > 0 && iSeqNumber >= iMaxFrames) {
      bEof = true;
    }
    // sleep(1);  // simulate real-time encoding delay
  }

  LOG(INFO) << "[socket_codec_main] Encoding completed. Total frames: "
            << iShowNumber;
  cYuvFileInput.close();
  vvenc_print_summary(encoder);
  vvenc_encoder_close(encoder);
  vvenc_YUVBuffer_free_buffer(&cYUVInputBuffer);
  vvenc_accessUnit_free_payload(&AU);
  if (cOutBitstream.is_open()) {
    cOutBitstream.close();
  }

  return 0;
}