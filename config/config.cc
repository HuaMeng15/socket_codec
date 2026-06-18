#include "config.h"

void InitializeFlags() {
  static bool initialized = false;
  if (initialized) {
    return;  // Already initialized, skip
  }
  initialized = true;

  auto& parser = CmdLineParser::GetInstance();

  parser.AddStringFlag("ip", "10.0.0.4", "receiver IP address");
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
  parser.AddStringFlag("codec", "mock",
                       "codec type: 'vvenc', 'x264', or 'mock'");

  // Network simulator flags (sender-side, 0 = disabled)
  parser.AddIntFlag("sim_bandwidth_kbps", 0,
                    "simulated bandwidth in kbps (0 = unlimited)");
  parser.AddIntFlag("sim_delay_ms", 0,
                    "simulated one-way delay in ms (0 = none)");
  parser.AddIntFlag("sim_loss_percent", 0,
                    "simulated packet loss percentage (0 = none)");
  parser.AddIntFlag("sim_jitter_ms", 0,
                    "simulated jitter in ms (0 = none)");
  parser.AddIntFlag("sim_max_queue_ms", 300,
                    "bottleneck buffer size in ms; packets queued longer are "
                    "dropped (default 300ms, a realistic router buffer)");
  parser.AddStringFlag("sim_bandwidth_schedule", "",
                       "time-based bandwidth steps as 't_sec:kbps,...' "
                       "e.g. '0:10000,10:1000' (overrides sim_bandwidth_kbps)");

  // GCC congestion controller bitrate bounds (kbps)
  parser.AddIntFlag("cc_initial_bitrate_kbps", 500,
                    "initial GCC target bitrate in kbps (ramps/probes up)");
  parser.AddIntFlag("cc_min_bitrate_kbps", 200,
                    "minimum GCC target bitrate in kbps (floor)");
  parser.AddIntFlag("cc_max_bitrate_kbps", 20000,
                    "maximum GCC target bitrate in kbps (ceiling; also caps "
                    "probe targets)");
}