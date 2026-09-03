#ifndef TRANSMISSION_RECEIVED_FRAME_DATA_HANDLER_H
#define TRANSMISSION_RECEIVED_FRAME_DATA_HANDLER_H

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "message_handler.h"
#include "data_receiver.h"
#include "data_sender.h"
#include "feedback_collector.h"
#include "codec/decoder.h"
#include "codec/codec_factory.h"
#include "packet_header.h"
#include <memory>

class ReceivedFrameDataHandler : public MessageHandler {
 public:
  ReceivedFrameDataHandler(CodecType codec_type, int width, int height,
                            DataReceiver* data_receiver,
                            int feedback_port, const std::string& output_file = "",
                            bool feedback_mux = false);
  ~ReceivedFrameDataHandler();

  int HandlePacketMessage(const uint8_t* packet_data,
                          size_t packet_size) override;
  int HandlePacketMessageWithTimestamp(const uint8_t* packet_data,
                                       size_t packet_size,
                                       int64_t arrival_time_us) override;

  // Initialize the handler (creates and initializes decoder)
  int Initialize();

  // Max time (ms) a packet may wait before its feedback batch is flushed.
  // Bounds feedback latency at low bitrates. Forwards to the FeedbackCollector.
  void SetFeedbackMaxIntervalMs(int ms);

 private:
  // Frame assembly state
  struct FrameAssembly {
    std::vector<std::vector<uint8_t>> packets;  // Packets for this frame
    uint16_t total_packets;                    // Expected total packets
    uint16_t received_packets;                 // Number of packets received
    bool complete;                              // Frame is complete
  };

  void ProcessPacket(const uint8_t* packet_data, size_t packet_size,
                     int64_t arrival_time_us);

  void SendFeedback(uint16_t frame_sequence, uint16_t packet_index,
                    uint16_t recv_size, int64_t arrival_time_us);

  void ReportFrameLoss(uint16_t frame_sequence, const FrameAssembly& assembly);

  void HandleCompleteFrame(uint32_t frame_sequence, const std::vector<uint8_t>& frame_data);

  std::vector<uint8_t> CopyYUVFrame(const YUVBuffer* yuv_buffer) const;
  void OutputWriterLoop();
  void StopOutputWriter();

  bool InitializeFeedbackSender();

  std::unique_ptr<Decoder> decoder_;
  CodecType codec_type_;
  int width_;
  int height_;
  DataReceiver* data_receiver_;
  DataSender feedback_sender_;
  FeedbackCollector feedback_collector_;
  int feedback_port_;
  bool feedback_sender_initialized_;
  bool feedback_mux_;
  bool initialized_;

  // Frame assembly map: frame_sequence -> FrameAssembly
  std::map<uint32_t, FrameAssembly> frame_assemblies_;
  uint32_t last_completed_frame_;
  // Frames older than (highest_seen - kFrameLookback) are evicted; if still
  // incomplete, their missing packets are reported as lost.
  static constexpr uint16_t kFrameLookback = 10;

  // Output file for writing decoded frames
  std::string output_file_;
  std::ofstream output_stream_;
  std::mutex output_mutex_;
  std::condition_variable output_cv_;
  std::deque<std::vector<uint8_t>> output_queue_;
  std::thread output_thread_;
  bool output_stopping_ = false;
  bool output_write_failed_ = false;
};

#endif  // TRANSMISSION_RECEIVED_FRAME_DATA_HANDLER_H
