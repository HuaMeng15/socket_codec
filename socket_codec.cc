#include "include/vvenc.h"
#include "log_system/log_system.h"
#include "receiver.h"
#include "sender.h"
#include "tools/config.h"
#include "tools/yuv_file_io.h"

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
  vvencPresetMode preset = VVENC_FAST;

  vvenc_init_default(&params, 1920, 1080, 30, 1000000, 20, preset);

  vvencEncoder* encoder = vvenc_encoder_create();
  int iRet = vvenc_encoder_open(encoder, &params);

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
  bool bEncodeDone = false;

  while (!bEncodeDone) {
    vvencYUVBuffer* ptrYUVInputBuffer = nullptr;
    if (1 == cYuvFileInput.readYuvBuf(cYUVInputBuffer)) {
      LOG(INFO) << "[socket_codec_main] Finish reading yuv file."
                << cYuvFileInput.getLastError();
      vvenc_YUVBuffer_free_buffer(&cYUVInputBuffer);
      vvenc_accessUnit_free_payload(&AU);
      vvenc_encoder_close(encoder);
      return -1;
    }
    cYUVInputBuffer.sequenceNumber = iSeqNumber;
    cYUVInputBuffer.cts = iSeqNumber;
    cYUVInputBuffer.ctsValid = true;
    ptrYUVInputBuffer = &cYUVInputBuffer;
    iSeqNumber++;

    // call encode
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
  }

  cYuvFileInput.close();

  // int luma_size = params.m_SourceWidth * params.m_SourceHeight;
  // int chroma_size = luma_size / 4;
  // cYUVInputBuffer.planes[0].ptr = (int16_t*)malloc(luma_size);
  // cYUVInputBuffer.planes[1].ptr = (int16_t*)malloc(chroma_size);
  // cYUVInputBuffer.planes[2].ptr = (int16_t*)malloc(chroma_size);

  // for (;; i_frame++) {
  //   LOG(INFO) << "Encoding frame: " << i_frame;
  //   memset(cYUVInputBuffer.planes[0].ptr, 0, luma_size * sizeof(int16_t));
  //   memset(cYUVInputBuffer.planes[1].ptr, 0, chroma_size * sizeof(int16_t));
  //   memset(cYUVInputBuffer.planes[2].ptr, 0, chroma_size * sizeof(int16_t));
  //   if (fread(cYUVInputBuffer.planes[0].ptr, 1, luma_size, input_yuv_file) !=
  //   (unsigned long)luma_size) break; if (fread(cYUVInputBuffer.planes[1].ptr,
  //   1, chroma_size, input_yuv_file) != (unsigned long)chroma_size) break; if
  //   (fread(cYUVInputBuffer.planes[2].ptr, 1, chroma_size, input_yuv_file) !=
  //   (unsigned long)chroma_size) break;

  //   LOG(INFO) << "Read frame " << i_frame << " from input file.";

  //   // cYUVInputBuffer.planes[0].ptr = luma_buf;
  //   // cYUVInputBuffer.planes[0].width = params.m_SourceWidth;
  //   // cYUVInputBuffer.planes[0].height = params.m_SourceHeight;
  //   // cYUVInputBuffer.planes[0].stride = params.m_SourceWidth;

  //   // cYUVInputBuffer.planes[1].ptr = chroma_u_buf;
  //   // cYUVInputBuffer.planes[1].width = params.m_SourceWidth / 2;
  //   // cYUVInputBuffer.planes[1].height = params.m_SourceHeight / 2;
  //   // cYUVInputBuffer.planes[1].stride = params.m_SourceWidth;

  //   // cYUVInputBuffer.planes[2].ptr = chroma_v_buf;
  //   // cYUVInputBuffer.planes[2].width = params.m_SourceWidth / 2;
  //   // cYUVInputBuffer.planes[2].height = params.m_SourceHeight / 2;
  //   // cYUVInputBuffer.planes[2].stride = params.m_SourceWidth;

  //   LOG(INFO) << "Prepared YUV buffer for frame " << i_frame;

  //   cYUVInputBuffer.cts = i_frame;
  //   cYUVInputBuffer.ctsValid = true;

  //   // Pass the yuv input packet to the encoder
  //   int ret = vvenc_encode(encoder, &cYUVInputBuffer, &AU, &encDone);
  //   if (ret != VVENC_OK) {
  //     LOG(ERROR) << "Encoding failed for frame " << i_frame << " with error
  //     code: " << ret; return -1;    // abort on error for simplicity
  //   }
  //   if (AU.payloadUsedSize > 0) {
  //     LOG(INFO) << "Encoded AU of size: " << AU.payloadUsedSize;
  //       //   process the encoder access unit
  //   }
  // }

  // vvencYUVBuffer* yuvFlush = NULL;
  // while (!encDone) {
  //   int ret = vvenc_encode(encoder, yuvFlush, &AU, &encDone);
  //   if (ret != VVENC_OK) {
  //       return -1;    // abort on error for simplicity
  //   }
  //   if (AU.payloadUsedSize > 0) {
  //     LOG(INFO) << "Flushed AU of size: " << AU.payloadUsedSize;
  //     //   process the encoder access unit
  //   }
  // }

  vvenc_encoder_close(encoder);
  vvenc_YUVBuffer_free(&cYUVInputBuffer,
                       true);        // release storage and payload memory
  vvenc_accessUnit_free(&AU, true);  // release storage and payload memory
  // fclose(input_yuv_file);
  // fclose(encoded_file_out);

  return 0;
}