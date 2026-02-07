#include "config.h"

void InitializeFlags() {
  static bool initialized = false;
  if (initialized) {
    return;  // Already initialized, skip
  }
  initialized = true;
  
  auto& parser = CmdLineParser::GetInstance();
  
  parser.AddStringFlag("ip", "127.0.0.1", "receiver IP address");
  parser.AddIntFlag("port", 8888, "receiver port");
  parser.AddStringFlag(
      "file", "NONE",
      "NONE if is sender, otherwise specify the file for receiver to save");
  parser.AddIntFlag("width", kDefaultWidth, "video width");
  parser.AddIntFlag("height", kDefaultHeight, "video height");
  parser.AddIntFlag("fps", kDefaultFps, "video fps");
  parser.AddIntFlag("frames_to_encode", kDefaultFramesToEncode,
                    "number of frames to encode (0 or negative means encode all frames)");
  parser.AddStringFlag("input_video_file", "input/Lecture_5s.yuv",
                       "input YUV video file for sender");
  parser.AddStringFlag("output_video_file", "result/output.266",
                       "output encoded video file for receiver");
  parser.AddStringFlag("codec", "x264",
                       "codec type: 'vvenc' or 'x264'");
}