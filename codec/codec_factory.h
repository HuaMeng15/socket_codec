#ifndef CODEC_CODEC_FACTORY_H
#define CODEC_CODEC_FACTORY_H

#include "encoder.h"
#include "decoder.h"
#include <memory>
#include <string>

enum class CodecType {
  VVENC,  // VVenC/VVdec
  X264    // x264 (encoder only, decoding requires libavcodec)
};

class CodecFactory {
 public:
  // Create encoder based on codec type
  static std::unique_ptr<Encoder> CreateEncoder(CodecType type);

  // Create decoder based on codec type
  static std::unique_ptr<Decoder> CreateDecoder(CodecType type);

  // Parse codec type from string
  static CodecType ParseCodecType(const std::string& codec_name);
};

#endif  // CODEC_CODEC_FACTORY_H

