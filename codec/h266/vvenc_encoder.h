#ifndef CODEC_VVENC_ENCODER_H
#define CODEC_VVENC_ENCODER_H

#ifdef VVENC

#include "../encoder.h"
#include "vvenc/vvenc.h"
#include "vvenc/vvencCfg.h"

// VVenc-specific encoded data structure
// Stores access unit data from vvenc encoder
class VVencEncodedData : public EncodedData {
  public:
   VVencEncodedData();
   ~VVencEncodedData() override;
 
   // Set encoded data from vvenc access unit
   void SetData(const uint8_t* au_data, size_t au_size);
 
  private:
   // Storage for encoded data
   std::vector<uint8_t> data_storage_;
 };
 
class VVencEncoder : public Encoder {
 public:
  VVencEncoder();
  ~VVencEncoder() override;

  // Encoder interface
  int Initialize(int width, int height, int fps, int framesToBeEncoded = -1) override;
  void SetOutputStream(std::ofstream* output_stream) override;
  std::unique_ptr<EncodedData> EncodeFrame(YUVBuffer* input_buffer) override;
  void Cleanup() override;
  void PrintSummary() const override;

 private:
  // Initialize encoder parameters
  void InitializeEncoderParams(vvenc_config* params, int width, int height,
                               int fps, int framesToBeEncoded);

  // Convert YUVBuffer to vvencYUVBuffer
  void ConvertYUVBufferToVVenc(const YUVBuffer* source, vvencYUVBuffer* dest);

  // Copy frame buffer data from source to encoder's buffer
  void CopyFrameBuffer(const vvencYUVBuffer* source);

  // Write encoded data to file (for debugging)
  void WriteEncodedDataToFile(const uint8_t* data, size_t size);

  vvencEncoder* encoder_;
  vvenc_config params_;
  vvencYUVBuffer yuv_input_buffer_;
  vvencAccessUnit access_unit_;
  std::ofstream* output_stream_;
  bool initialized_;
  uint16_t sequence_number_;
};

#endif  // VVENC

#endif  // CODEC_VVENC_ENCODER_H

