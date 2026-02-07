#ifndef VIDEO_CAPTURE_AND_SEND_H
#define VIDEO_CAPTURE_AND_SEND_H

#include <atomic>
#include <fstream>
#include <memory>
#include <string>

#include "codec/encoder.h"
#include "codec/frame_capture.h"
#include "codec/codec_factory.h"
#include "transmission/data_sender.h"

class VideoCaptureAndSend {
 public:
  VideoCaptureAndSend();
  ~VideoCaptureAndSend();

  // Initialize all components
  int Initialize(const std::string& input_file,
                  const std::string& output_file,
                  const std::string& dest_ip,
                  int dest_port,
                  int width,
                  int height,
                  int fps,
                  int frames_to_encode,
                  CodecType codec_type);

  // Run the capture and send loop (single thread)
  void Run();

  // Stop the capture and send loop
  void Stop();

  // Check if stopped
  bool IsStopped() const;

  // Cleanup all resources
  void Cleanup();

  // Print encoding summary
  void PrintSummary() const;

 private:
  std::unique_ptr<Encoder> encoder_;
  std::unique_ptr<FrameCapture> frame_capture_;
  std::unique_ptr<DataSender> data_sender_;
  std::ofstream output_stream_;

  std::atomic<bool> stop_requested_;
  bool initialized_;
  int fps_;
  int max_frames_;
};

#endif  // VIDEO_CAPTURE_AND_SEND_H

