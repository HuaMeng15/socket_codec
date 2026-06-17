#include <unistd.h>
#include <thread>
#include <atomic>
#include <chrono>
#include <sstream>
#include <vector>

#include "codec/codec_factory.h"
#include "video_capture_and_send.h"
#include "log_system/log_system.h"
#include "tools/thread_manager.h"
#include "transmission/data_receiver.h"
#include "transmission/received_frame_data_handler.h"
#include "transmission/feedback_handler.h"
#include "transmission/gcc_controller.h"
#include "transmission/network_simulator.h"
#include "transmission/pacer.h"
#include "transmission/packet_send_time_store.h"

// Parse "t_sec:kbps,t_sec:kbps,..." into sorted (time_ms, kbps) steps.
static std::vector<std::pair<int64_t, int>> ParseBandwidthSchedule(
    const std::string& spec) {
  std::vector<std::pair<int64_t, int>> steps;
  std::stringstream ss(spec);
  std::string token;
  while (std::getline(ss, token, ',')) {
    auto colon = token.find(':');
    if (colon == std::string::npos) continue;
    double t_sec = std::stod(token.substr(0, colon));
    int kbps = std::stoi(token.substr(colon + 1));
    steps.emplace_back(static_cast<int64_t>(t_sec * 1000), kbps);
  }
  return steps;
}


/* Sender has two key components:
*  1. Video Capture and Send (frame capture + encoding + sending in one thread)
*  2. Feedback Handler and Receiver
*/
int sender_create_and_run(CmdLineParser& parser, const std::string& dest_ip, int dest_port) {
  LOG(INFO) << "[socket_codec_main] Running in sender mode";

  /* Read Configurations */
  std::string input_video_file =
      parser.GetFlag<std::string>("input_video_file");
  std::string output_video_file =
      parser.GetFlag<std::string>("output_video_file");
  int width = parser.GetFlag<int>("width");
  int height = parser.GetFlag<int>("height");
  int fps = parser.GetFlag<int>("fps");
  int framesToBeEncoded = parser.GetFlag<int>("frames_to_encode");
  std::string codec_name = parser.GetFlag<std::string>("codec");
  CodecType codec_type = CodecFactory::ParseCodecType(codec_name);

  if (width <= 0 || height <= 0 || width > 7680 || height > 4320) {
    LOG(ERROR) << "[socket_codec_main] Invalid width/height: " << width << "x" << height;
    return -1;
  }

  /* Initializations */
  PacketSendTimeStore send_time_store;

  // Set up network simulator if any sim flags are non-zero
  NetworkSimulator simulator;
  NetworkSimulator* sim_ptr = nullptr;
  std::string bw_schedule_spec = parser.GetFlag<std::string>("sim_bandwidth_schedule");
  auto bw_schedule = ParseBandwidthSchedule(bw_schedule_spec);
  {
    int sim_bw = parser.GetFlag<int>("sim_bandwidth_kbps");
    int sim_delay = parser.GetFlag<int>("sim_delay_ms");
    int sim_loss = parser.GetFlag<int>("sim_loss_percent");
    int sim_jitter = parser.GetFlag<int>("sim_jitter_ms");
    int sim_max_queue = parser.GetFlag<int>("sim_max_queue_ms");
    // Schedule's first step (t=0) sets the initial bandwidth if present.
    if (!bw_schedule.empty() && sim_bw == 0) {
      sim_bw = bw_schedule.front().second;
    }
    if (sim_bw > 0 || sim_delay > 0 || sim_loss > 0 || sim_jitter > 0 ||
        !bw_schedule.empty()) {
      NetworkSimulator::Config sim_config;
      sim_config.bandwidth_kbps = sim_bw;
      sim_config.max_queue_ms = sim_max_queue;
      sim_config.propagation_delay_ms = sim_delay;
      sim_config.loss_rate = sim_loss / 100.0;
      sim_config.jitter_ms = sim_jitter;
      simulator.SetConfig(sim_config);
      sim_ptr = &simulator;
      LOG(INFO) << "[socket_codec_main] Network simulator enabled: bw="
                << sim_bw << "kbps delay=" << sim_delay << "ms loss="
                << sim_loss << "% jitter=" << sim_jitter << "ms"
                << (bw_schedule.empty() ? "" : " (scheduled)");
    }
  }

  // Background thread applies the bandwidth schedule at the given times.
  std::atomic<bool> schedule_stop{false};
  std::thread schedule_thread;
  if (sim_ptr && !bw_schedule.empty()) {
    schedule_thread = std::thread([&]() {
      auto start = std::chrono::steady_clock::now();
      for (const auto& [t_ms, kbps] : bw_schedule) {
        while (!schedule_stop.load()) {
          auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - start).count();
          if (elapsed >= t_ms) break;
          std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (schedule_stop.load()) break;
        simulator.SetBandwidthKbps(kbps);
        LOG(INFO) << "[socket_codec_main] [SCHEDULE] t=" << t_ms
                  << "ms set bandwidth to " << kbps << " kbps";
      }
    });
  }

  VideoCaptureAndSend video_capture_and_send;
  video_capture_and_send.SetSendTimeStore(&send_time_store);
  if (sim_ptr) {
    video_capture_and_send.SetSimulator(sim_ptr);
  }
  if (0 != video_capture_and_send.Initialize(input_video_file,
                                             output_video_file,
                                             dest_ip,
                                             dest_port,
                                             width,
                                             height,
                                             fps,
                                             framesToBeEncoded,
                                             codec_type)) {
    LOG(ERROR) << "[socket_codec_main] Failed to initialize video capture and send";
    return -1;
  }

  FeedbackHandler feedback_handler;
  if (0 != feedback_handler.Initialize()) {
    LOG(ERROR) << "[socket_codec_main] Failed to initialize feedback handler";
    return -1;
  }
  feedback_handler.SetSendTimeStore(&send_time_store);
  feedback_handler.SetEncoder(video_capture_and_send.GetEncoder());
  feedback_handler.SetPacer(video_capture_and_send.GetPacer());

  // Set up GCC congestion controller
  GccController gcc;
  gcc.SetInitialBitrate(5000);  // Start at 5 Mbps
  gcc.SetBitrateRange(200, 20000);

  // Wire actual sent packet count from DataSender into GCC
  DataSender* data_sender_ptr = video_capture_and_send.GetDataSender();
  if (data_sender_ptr) {
    data_sender_ptr->SetPacketsSentCallback([&gcc](int count) {
      gcc.OnPacketsSent(count);
    });
  }

  Encoder* encoder_ptr = video_capture_and_send.GetEncoder();
  Pacer* pacer_ptr = video_capture_and_send.GetPacer();
  feedback_handler.SetTransportFeedbackCallback(
      [&gcc, encoder_ptr, pacer_ptr](const TransportFeedback& fb) {
        gcc.OnTransportFeedback(fb);
        int target = gcc.GetTargetBitrateKbps();
        if (encoder_ptr) encoder_ptr->SetTargetBitrate(target);
        if (pacer_ptr) pacer_ptr->SetTargetBitrate(target);
      });
  feedback_handler.SetLossReportCallback(
      [&gcc, encoder_ptr, pacer_ptr](const LossReport& report) {
        gcc.OnLossReport(report);
        int target = gcc.GetTargetBitrateKbps();
        if (encoder_ptr) encoder_ptr->SetTargetBitrate(target);
        if (pacer_ptr) pacer_ptr->SetTargetBitrate(target);
      });

  int feedback_port = dest_port + 1;
  DataReceiver feedback_receiver;
  if (0 != feedback_receiver.Initialize(feedback_port)) {
    LOG(ERROR) << "[socket_codec_main] Failed to initialize feedback receiver";
    return -1;
  }
  feedback_receiver.SetMessageHandler(&feedback_handler);


  LOG(INFO) << "[socket_codec_main] Starting feedback receiver thread";
  Thread feedback_receiver_thread([&feedback_receiver]() {
    feedback_receiver.Run();
  });

  LOG(INFO) << "[socket_codec_main] Starting video capture and send";
  video_capture_and_send.Run();

  LOG(INFO) << "[socket_codec_main] Stopping video capture and send";

  /* Stop and cleanup */
  feedback_receiver.Stop();
  feedback_receiver_thread.Join();

  // Stop the bandwidth schedule thread
  schedule_stop.store(true);
  if (schedule_thread.joinable()) {
    schedule_thread.join();
  }

  LOG(INFO) << "[socket_codec_main] All threads finished";

  video_capture_and_send.PrintSummary();
  video_capture_and_send.Cleanup();
  feedback_receiver.Close();

  return 0;
}

/* Receiver has three key components:
*  1. Data Receiver
*  2. Received Frame Data Handler
*/
int receiver_create_and_run(CmdLineParser& parser, int dest_port, const std::string& filename) {
  LOG(INFO) << "[socket_codec_main] Running in receiver mode, saving to file: "
            << filename;

  /* Read Configurations */
  int width = parser.GetFlag<int>("width");
  int height = parser.GetFlag<int>("height");
  std::string codec_name = parser.GetFlag<std::string>("codec");
  CodecType codec_type = CodecFactory::ParseCodecType(codec_name);

  // To receive encoded data.
  DataReceiver data_receiver;
  if (0 != data_receiver.Initialize(dest_port)) {
    LOG(ERROR) << "[socket_codec_main] Failed to initialize data receiver";
    return -1;
  }

  // Create received frame data handler with feedback support
  // Feedback will be sent to sender on dest_port + 1
  // Decoder will be created inside the handler
  int feedback_port = dest_port + 1;
  ReceivedFrameDataHandler received_frame_data_handler(codec_type, width, height,
                                            &data_receiver, feedback_port, filename);
  if (0 != received_frame_data_handler.Initialize()) {
    LOG(ERROR) << "[socket_codec_main] Failed to initialize received frame data handler";
    return -1;
  }
  data_receiver.SetMessageHandler(&received_frame_data_handler);

  /* Start Running */
  data_receiver.Run();

  LOG(INFO) << "[socket_codec_main] Receiver stopped";
  data_receiver.Close();
  return 0;
}

/* Program Entry */
/*  1. Parse Command Line Arguments
*  2. Create and Run Sender or Receiver
*  3. Return 0 if successful, -1 if error
*/
int main(int argc, char* argv[]) {
  InitializeFlags();
  auto& parser = CmdLineParser::GetInstance();
  parser.Parse(argc, argv);

  std::string filename = parser.GetFlag<std::string>("file");

  std::string dest_ip = parser.GetFlag<std::string>("ip");
  int dest_port = parser.GetFlag<int>("port");

  if (filename == "NONE") {
    int ret = sender_create_and_run(parser, dest_ip, dest_port);
    if (ret != 0) {
      LOG(ERROR) << "[socket_codec_main] Failed to run sender";
      return -1;
    }
  } else {
    receiver_create_and_run(parser, dest_port, filename);
  }

  return 0;
}