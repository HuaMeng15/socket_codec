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
  parser.AddIntFlag("sim_max_queue_ms", 0,
                    "bottleneck buffer size in ms; packets queued longer are "
                    "dropped (0 = unlimited queue, no buffer-overflow drops)");
  parser.AddStringFlag("sim_bandwidth_schedule", "",
                       "time-based bandwidth steps as 't_sec:kbps,...' "
                       "e.g. '0:10000,10:1000' (overrides sim_bandwidth_kbps)");

  // GCC congestion controller bitrate bounds (kbps)
  parser.AddIntFlag("cc_initial_bitrate_kbps", kDefaultInitialBitrateKbps,
                    "initial GCC target bitrate in kbps (ramps/probes up)");
  parser.AddIntFlag("cc_min_bitrate_kbps", 200,
                    "minimum GCC target bitrate in kbps (floor)");
  parser.AddIntFlag("cc_max_bitrate_kbps", 20000,
                    "maximum GCC target bitrate in kbps (ceiling; also caps "
                    "probe targets)");

  // Pacer tuning. The pacer is a bounded token bucket: it releases packets at
  // pace_multiplier × target, with bursts capped at pace_burst_cap_ms of data.
  // Default 1.0× matches a greedy encoder (mock or real) that always fills the
  // target rate — the frame occupies the full interval and there is no idle
  // gap for bursting. Set to 2.5× (250) to match WebRTC's kDefaultPaceMultiplier
  // when using a real encoder that sometimes undershoots its target.
  parser.AddIntFlag("cc_pace_multiplier_x100", 100,
                    "pacer rate as a multiple of target, ×100 (100 = 1.0x for "
                    "greedy encoders; 250 = 2.5x for real encoders that undershoot)");
  parser.AddIntFlag("cc_pace_burst_cap_ms", 5,
                    "max pacer burst in ms of data at the current rate "
                    "(token-bucket depth; prevents idle-gap credit buildup)");

  // Mock encoder mode. Set encoder_variable_mode=1 to enable a content-adaptive
  // VBR simulation: during each period the encoder spends encoder_alr_fraction%
  // of time in a low-demand (app-limited) phase producing only
  // encoder_alr_low_ratio_x100 / 100 of the CC target, and the rest of the time
  // at full target (no ALR). This makes the AlrDetector enter ALR during the low
  // phase → fires the WebRTC periodic ALR probe → exercises the full CC probing
  // pipeline.
  //
  // Set encoder_variable_mode=0 (default) for the static / greedy mode used in
  // CC performance benchmarks: the encoder always fills the CC target exactly,
  // the AlrDetector never enters ALR, and steady state is pure AIMD — faithful
  // to WebRTC with a source that saturates the link.
  //
  // When using variable mode, also set cc_pace_multiplier_x100=250 to match
  // WebRTC's kDefaultPaceMultiplier (2.5×), which absorbs encoder-rate variance.
  parser.AddIntFlag("encoder_variable_mode", 0,
                    "0=static/greedy (always fills CC target); "
                    "1=variable/VBR (duty-cycle ALR simulation)");
  parser.AddIntFlag("encoder_alr_low_ratio_x100", 15,
                    "production rate during the low/ALR phase as "
                    "percent of CC target (default 15 = 15%)");
  parser.AddIntFlag("encoder_period_ms", 10000,
                    "length of one high/low duty-cycle in ms (default 10 s)");
  parser.AddIntFlag("encoder_alr_fraction_x100", 40,
                    "percent of each period spent in the ALR (low) phase "
                    "(default 40 = 40%; the rest is full-rate)");
}