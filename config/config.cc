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
  parser.AddStringFlag("input_video_file",
                       "/home/menghua/Research/VideoResources/Lecture.yuv",
                       "input YUV video file for sender");
  parser.AddStringFlag("output_video_file", "",
                       "optional sender-side encoded bitstream dump; empty disables");
  parser.AddStringFlag("codec", "mock",
                       "codec type: 'vvenc', 'x264', 'x264_slice', or 'mock'");
  parser.AddStringFlag(
      "experiment_mode", "auto",
      "experiment policy: auto, default, x264_slice, salsify, cbr, or "
      "webrtc_no_mae (alias: webrtc_disable_mae); explicit modes select the "
      "matching x264 encoder path and override --codec");
  parser.AddIntFlag(
      "periodic_alr_probing", -1,
      "periodic ALR probe policy: -1=experiment default, 0=disabled, "
      "1=enabled; startup probing is unaffected");

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

  // Congestion-window pushback (WebRTC "WebRTC-CongestionWindow" field trial).
  // Crashes the target to cc_cwnd_min_bitrate_kbps within ~1 RTT under a
  // capacity drop and holds until the in-flight backlog drains — no loss
  // required. cc_cwnd_queue_size_ms is the additional time added to the min
  // RTT when sizing the window. Set queue_size_ms <= 0 to disable.
  parser.AddIntFlag("cc_cwnd_queue_size_ms", 350,
                    "congestion-window pushback: additional time (ms) added to "
                    "min RTT for window sizing; <=0 disables pushback");
  parser.AddIntFlag("cc_cwnd_min_bitrate_kbps", 30,
                    "congestion-window pushback: target floor in kbps");

  // Pacer tuning. The pacer is a bounded token bucket: it releases packets at
  // pace_multiplier × target, with bursts capped at pace_burst_cap_ms of data.
  // Default 2.5× matches WebRTC's kDefaultPaceMultiplier. It MUST exceed 1.0×:
  // a greedy encoder produces target/fps bits per frame, so at exactly 1.0× the
  // pacer drains one frame in precisely one frame interval (=1/fps) — 100%
  // utilization with zero headroom. That is only marginally stable: any queue
  // that ever forms (an AIMD sawtooth peak above the link, or a startup-probe
  // overshoot) can never be drained, because the pacer's output rate equals the
  // encoder's input rate at every target. The queue becomes a permanent standing
  // delay (~40ms on a 10 Mbps link). At 2.5× the pacer empties each frame in
  // ~1/(2.5·fps) and sits idle the rest of the interval, so that idle headroom
  // flushes any accumulated queue within a frame and keeps latency at ~one
  // interval. The burst cap below stops the faster drain from becoming a flood.
  parser.AddIntFlag("cc_pace_multiplier_x100", 250,
                    "pacer rate as a multiple of target, ×100 (250 = 2.5x, "
                    "WebRTC's kDefaultPaceMultiplier; must be >1.0x or a standing "
                    "queue at the pacer can never drain)");
  parser.AddIntFlag("cc_pace_burst_cap_ms", 5,
                    "max pacer burst in ms of data at the current rate "
                    "(token-bucket depth; prevents idle-gap credit buildup)");

  // Receiver-side feedback timing. Feedback is sent on whichever fires first:
  // 20 packets accumulated, or a pending packet aging past this window. The
  // time bound keeps feedback fast at low bitrates, where the packet-count
  // trigger alone would lag ~one window of arrivals (20 pkts @ 1 Mbps ≈ 192ms),
  // starving the sender's congestion controller. <=0 disables the time trigger.
  parser.AddIntFlag("feedback_max_interval_ms", 10,
                    "max time (ms) a packet waits before its feedback batch is "
                    "sent; bounds feedback latency at low bitrate; <=0 disables");

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
  // to WebRTC with a source that saturates the link. (The pacer already
  // defaults to 2.5× — see cc_pace_multiplier_x100 above — which absorbs
  // encoder-rate variance in variable mode and keeps the pacer queue drained
  // in static mode.)
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
