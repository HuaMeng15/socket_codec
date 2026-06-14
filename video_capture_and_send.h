#ifndef VIDEO_CAPTURE_AND_SEND_H
#define VIDEO_CAPTURE_AND_SEND_H

#include <atomic>
#include <fstream>
#include <memory>
#include <string>

#include "codec/encoder.h"
#include "codec/frame_capture.h"
#include "codec/codec_factory.h"
#include "tools/clock_thread.h"
#include "transmission/data_sender.h"

class PacketSendTimeStore;
class Pacer;

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

  /** Optional: set to record send times for latency stats (e.g. feedback handler). */
  void SetSendTimeStore(PacketSendTimeStore* store) { send_time_store_ = store; }
  /** Get encoder (e.g. for feedback handler to set bitrate). Non-owning. */
  Encoder* GetEncoder() { return encoder_.get(); }
  /** Get pacer (e.g. for feedback handler to set bitrate). Non-owning. */
  Pacer* GetPacer() { return pacer_.get(); }

 private:
  std::unique_ptr<Encoder> encoder_;
  std::unique_ptr<FrameCapture> frame_capture_;
  std::unique_ptr<DataSender> data_sender_;
  std::unique_ptr<Pacer> pacer_;
  ClockThread clock_;
  std::ofstream output_stream_;

  std::atomic<bool> stop_requested_;
  bool initialized_;
  int fps_;
  int max_frames_;
  PacketSendTimeStore* send_time_store_ = nullptr;
};

#endif  // VIDEO_CAPTURE_AND_SEND_H

