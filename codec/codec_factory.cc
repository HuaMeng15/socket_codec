#include "codec_factory.h"

#include "h264/x264_encoder.h"
#include "h264/x264_decoder.h"
#ifdef VVENC
#include "h266/vvenc_encoder.h"
#include "h266/vvdec_decoder.h"
#endif
#include "log_system/log_system.h"

std::unique_ptr<Encoder> CodecFactory::CreateEncoder(CodecType type) {
  switch (type) {
#ifdef VVENC
    case CodecType::VVENC:
      LOG(INFO) << "[CodecFactory] Creating VVencEncoder";
      return std::make_unique<VVencEncoder>();
#endif
    case CodecType::X264:
      LOG(INFO) << "[CodecFactory] Creating X264Encoder";
      return std::make_unique<X264Encoder>();
    default:
      LOG(ERROR) << "[CodecFactory] VVENC not enabled, defaulting to X264Encoder";
      return std::make_unique<X264Encoder>();
  }
}

std::unique_ptr<Decoder> CodecFactory::CreateDecoder(CodecType type) {
  switch (type) {
#ifdef VVENC
    case CodecType::VVENC:
      LOG(INFO) << "[CodecFactory] Creating VVdecDecoder";
      return std::make_unique<VVdecDecoder>();
#endif
    case CodecType::X264:
      LOG(INFO) << "[CodecFactory] Creating X264Decoder";
      return std::make_unique<X264Decoder>();
    default:
      LOG(ERROR) << "[CodecFactory] VVENC not enabled, defaulting to X264Decoder";
      return std::make_unique<X264Decoder>();
  }
}

CodecType CodecFactory::ParseCodecType(const std::string& codec_name) {
  if (codec_name == "vvenc" || codec_name == "VVENC") {
    return CodecType::VVENC;
  } else if (codec_name == "x264" || codec_name == "X264") {
    return CodecType::X264;
  } else {
    LOG(WARNING) << "[CodecFactory] Unknown codec name: " << codec_name << ", defaulting to X264";
    return CodecType::X264;
  }
}

