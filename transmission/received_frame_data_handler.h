#ifndef TRANSMISSION_RECEIVED_FRAME_DATA_HANDLER_H
#define TRANSMISSION_RECEIVED_FRAME_DATA_HANDLER_H

#include <map>
#include <string>
#include <vector>
#include <fstream>

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
                            int feedback_port, const std::string& output_file = "");
  ~ReceivedFrameDataHandler();

  int HandlePacketMessage(const uint8_t* packet_data,
                          size_t packet_size) override;

  // Initialize the handler (creates and initializes decoder)
  int Initialize();

 private:
  // Frame assembly state
  struct FrameAssembly {
    std::vector<std::vector<uint8_t>> packets;  // Packets for this frame
    uint8_t total_packets;                      // Expected total packets
    uint8_t received_packets;                  // Number of packets received
    bool complete;                              // Frame is complete
  };

  void ProcessPacket(const uint8_t* packet_data, size_t packet_size);

  void SendFeedback(uint16_t frame_sequence, uint8_t packet_index);

  void ReportFrameLoss(uint16_t frame_sequence, const FrameAssembly& assembly);

  void HandleCompleteFrame(uint32_t frame_sequence, const std::vector<uint8_t>& frame_data);

  void WriteYUVFrameToFile(YUVBuffer* yuv_buffer);

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
};

#endif  // TRANSMISSION_RECEIVED_FRAME_DATA_HANDLER_H

