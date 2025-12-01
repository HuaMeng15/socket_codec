#ifndef CODEC_DECODER_H
#define CODEC_DECODER_H

#include <cstdint>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "transmission/message_handler.h"
#include "vvdec/vvdec.h"
#include "log_system/log_system.h"
#include "transmission/packet_header.h"

#define MAX_CODED_PICTURE_SIZE 800000

class Decoder : public MessageHandler {
 public:
  Decoder();
  ~Decoder();

  // Initialize decoder with video parameters
  // output_file: optional output file path for writing encoded frames
  int Initialize(int width, int height, const std::string& output_file = "");

  // HandlePacketMessage implementation from MessageHandler
  // Handles packet assembly and decodes complete frames
  int HandlePacketMessage(const uint8_t* packet_data,
                         size_t packet_size) override;

  // Cleanup resources
  void Cleanup();

 private:
  // Frame assembly state
  struct FrameAssembly {
    std::vector<std::vector<uint8_t>> packets;  // Packets for this frame
    uint16_t total_packets;                      // Expected total packets
    uint32_t received_packets;                  // Number of packets received
    bool complete;                              // Frame is complete
  };

  // Process a received packet (internal method)
  void ProcessPacket(const uint8_t* packet_data, size_t packet_size);

  // Decode a complete frame from assembled data
  // Returns decoded frame pointer (caller must call ReleaseFrame when done)
  vvdecFrame* DecodeFrame(const uint8_t* frame_data, size_t frame_size);

  // Release a decoded frame
  void ReleaseFrame(vvdecFrame* frame);

  // Write complete frame to file (if output file is set)
  void DecodeAndWriteFrame(uint32_t frame_sequence, const std::vector<uint8_t>& frame_data);

  vvdecDecoder* decoder_;
  vvdecParams params_;
  bool initialized_;

  // Frame assembly map: frame_sequence -> FrameAssembly
  std::map<uint32_t, FrameAssembly> frame_assemblies_;
  uint32_t last_completed_frame_;

  // Output file for writing encoded frames
  std::string output_file_;
  std::ofstream output_stream_;

  vvdecAccessUnit access_unit_;
};

#endif  // CODEC_DECODER_H