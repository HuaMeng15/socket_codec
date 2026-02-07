#include <unistd.h>
#include <thread>
#include <chrono>

#include "codec/decoder.h"
#include "codec/codec_factory.h"
#include "video_capture_and_send.h"
#include "log_system/log_system.h"
#include "tools/thread_manager.h"
#include "transmission/data_receiver.h"
#include "transmission/received_frame_data_handler.h"
#include "transmission/feedback_handler.h"

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

  /* Initializations */
  VideoCaptureAndSend video_capture_and_send;
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