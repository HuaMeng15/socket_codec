#include <unistd.h>
#include <thread>
#include <chrono>

#include "codec/encoder.h"
#include "config/config.h"
#include "codec/frame_capture.h"
#include "log_system/log_system.h"
#include "tools/thread_manager.h"
#include "transmission/message_receiver.h"
#include "transmission/message_sender.h"

int sender_create_and_run(CmdLineParser& parser, const std::string& dest_ip, int dest_port) {
  // ========== SENDER MODE ==========
  LOG(INFO) << "[socket_codec_main] Running in sender mode";

  std::string input_video_file =
      parser.GetFlag<std::string>("input_video_file");
  std::string output_video_file =
      parser.GetFlag<std::string>("output_video_file");

  // Get video parameters
  int width = parser.GetFlag<int>("width");
  int height = parser.GetFlag<int>("height");
  int fps = parser.GetFlag<int>("fps");
  int framesToBeEncoded = parser.GetFlag<int>("frames_to_encode");

  // Open output file
  std::ofstream cOutBitstream;
  cOutBitstream.open(output_video_file,
                     std::ios::out | std::ios::binary | std::ios::trunc);
  if (!cOutBitstream.is_open()) {
    LOG(ERROR) << "[socket_codec_main] Failed to open output file "
               << output_video_file;
    return -1;
  }

  // Create frame capture instance
  FrameCapture frame_capture;
  if (0 != frame_capture.Initialize(input_video_file, width, height)) {
    LOG(ERROR) << "[socket_codec_main] Failed to initialize frame capture";
    return -1;
  }

  // Create and initialize message sender
  MessageSender message_sender;
  if (0 != message_sender.Initialize(dest_ip, dest_port)) {
    LOG(ERROR) << "[socket_codec_main] Failed to initialize message sender";
    return -1;
  }
  LOG(INFO) << "[socket_codec_main] Message sender initialized: " << dest_ip
            << ":" << dest_port;

  // Create and initialize encoder
  LOG(INFO) << "[socket_codec_main] Initializing encoder";
  Encoder encoder;
  if (0 != encoder.Initialize(width, height, fps, framesToBeEncoded)) {
    LOG(ERROR) << "[socket_codec_main] Failed to initialize encoder";
    return -1;
  }
  LOG(INFO) << "[socket_codec_main] Encoder initialized";

  // Link encoder with frame capture and message sender
  encoder.SetFrameCapture(&frame_capture);
  encoder.SetOutputStream(&cOutBitstream);
  encoder.SetMessageSender(&message_sender);

  LOG(INFO) << "[socket_codec_main] Starting frame capture and encoder threads";

  // Create threads for frame capture and encoder
  Thread frame_capture_thread([&frame_capture]() { frame_capture.Run(); });
  Thread encoder_thread([&encoder]() { encoder.Run(); });

  // Wait for both threads to complete
  LOG(INFO) << "[socket_codec_main] Waiting for threads to finish...";
  frame_capture_thread.Join();
  encoder_thread.Join();

  LOG(INFO) << "[socket_codec_main] All threads finished";

  // Print summary before cleanup
  encoder.PrintSummary();

  // Cleanup
  encoder.SetFrameCapture(nullptr);
  encoder.Cleanup();
  frame_capture.Stop();
  message_sender.Close();
  if (cOutBitstream.is_open()) {
    cOutBitstream.close();
  }
  return 0;
}

int receiver_create_and_run(int dest_port, const std::string& filename) {
  // ========== RECEIVER MODE ==========
  LOG(INFO) << "[socket_codec_main] Running in receiver mode, saving to file: "
            << filename;

  // Create and initialize message receiver
  MessageReceiver message_receiver;
  if (0 != message_receiver.Initialize(dest_port, filename)) {
    LOG(ERROR) << "[socket_codec_main] Failed to initialize message receiver";
    return -1;
  }

  LOG(INFO) << "[socket_codec_main] Message receiver initialized, listening on port "
            << dest_port;

  // Run receiver (blocks until stopped)
  message_receiver.Run();

  LOG(INFO) << "[socket_codec_main] Receiver stopped";

  // Cleanup
  message_receiver.Close();
  return 0;
}

int main(int argc, char* argv[]) {
  // Parse command-line arguments
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
    receiver_create_and_run(dest_port, filename);
  }

  return 0;
}