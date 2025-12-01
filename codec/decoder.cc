#include "decoder.h"

#include <arpa/inet.h>
#include <cstring>
#include <fstream>

#include "log_system/log_system.h"
#include "tools/yuv_file_io.h"

Decoder::Decoder()
    : decoder_(nullptr),
      initialized_(false),
      last_completed_frame_(0) {
  vvdec_accessUnit_default(&access_unit_);
}

Decoder::~Decoder() {
  Cleanup();
}

int Decoder::Initialize(int width, int height, const std::string& output_file) {
  if (initialized_) {
    LOG(WARNING) << "[Decoder] Already initialized";
    return 0;
  }

  LOG(INFO) << "[Decoder] Initializing decoder with resolution: " << width << "x" << height;

  // Store output file path
  output_file_ = output_file;

  // Open output file if specified
  if (!output_file_.empty()) {
    output_stream_.open(output_file_, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!output_stream_.is_open()) {
      LOG(ERROR) << "[Decoder] Failed to open output file: " << output_file_;
      return -1;
    }
    LOG(VERBOSE) << "[Decoder] Output file opened: " << output_file_;
  }

  // Initialize decoder parameters
  vvdec_params_default(&params_);
  params_.logLevel = VVDEC_NOTICE;
  params_.enable_realtime = true;

  // Open decoder
  decoder_ = vvdec_decoder_open(&params_);
  if (decoder_ == nullptr) {
    LOG(ERROR) << "[Decoder] Failed to open decoder";
    if (output_stream_.is_open()) {
      output_stream_.close();
    }
    return -1;
  }

  // Allocate access unit for decoding
  vvdec_accessUnit_alloc_payload(&access_unit_, MAX_CODED_PICTURE_SIZE);
  if (access_unit_.payload == nullptr) {
    LOG(ERROR) << "[Decoder] Failed to allocate access unit payload";
    vvdec_decoder_close(decoder_);
    decoder_ = nullptr;
    if (output_stream_.is_open()) {
      output_stream_.close();
    }
    return -1;
  }
  access_unit_.cts = 0;
  access_unit_.ctsValid = true;
  access_unit_.dts = 0;
  access_unit_.dtsValid = true;

  initialized_ = true;
  last_completed_frame_ = 0;
  frame_assemblies_.clear();

  LOG(VERBOSE) << "[Decoder] Decoder initialized successfully";

  return 0;
}

int Decoder::HandlePacketMessage(const uint8_t* packet_data, size_t packet_size) {
  if (!decoder_ || !initialized_) {
    LOG(ERROR) << "[Decoder] Decoder not initialized";
    return -1;
  }

  // Process the packet (handles assembly and decoding when complete)
  ProcessPacket(packet_data, packet_size);
  return 0;
}

void Decoder::ProcessPacket(const uint8_t* packet_data, size_t packet_size) {
  if (packet_size < sizeof(PacketHeader)) {
    LOG(WARNING) << "[Decoder] Packet too small, ignoring";
    return;
  }

  // Extract header
  const PacketHeader* header = reinterpret_cast<const PacketHeader*>(packet_data);
  uint32_t frame_sequence = ntohl(header->frame_sequence);
  uint16_t packet_index = ntohs(header->packet_index);
  uint16_t total_packets = ntohs(header->total_packets);
  uint32_t payload_size = ntohl(header->payload_size);

  // Validate payload size
  size_t expected_packet_size = sizeof(PacketHeader) + payload_size;
  if (packet_size < expected_packet_size) {
    LOG(WARNING) << "[Decoder] Packet size mismatch for frame "
                 << frame_sequence << " packet " << packet_index;
    return;
  }

  // Get or create frame assembly
  auto& frame_assembly = frame_assemblies_[frame_sequence];
  if (frame_assembly.packets.empty()) {
    // New frame
    frame_assembly.total_packets = total_packets;
    frame_assembly.received_packets = 0;
    frame_assembly.complete = false;
    frame_assembly.packets.resize(total_packets);
    LOG(INFO) << "[Decoder] Starting frame " << frame_sequence
              << " expecting " << total_packets << " packets";
  }

  // Validate packet index
  if (packet_index >= total_packets) {
    LOG(WARNING) << "[Decoder] Invalid packet index " << packet_index
                 << " for frame " << frame_sequence;
    return;
  }

  // Store packet payload
  if (frame_assembly.packets[packet_index].empty()) {
    // New packet for this frame
    const uint8_t* payload = packet_data + sizeof(PacketHeader);
    frame_assembly.packets[packet_index].assign(payload, payload + payload_size);
    frame_assembly.received_packets++;

    LOG(INFO) << "[Decoder] Received packet " << packet_index << "/"
              << (total_packets - 1) << " for frame " << frame_sequence
              << " (" << frame_assembly.received_packets << "/" << total_packets
              << " complete)";
  }

  // Check if frame is complete
  if (frame_assembly.received_packets == total_packets && !frame_assembly.complete) {
    frame_assembly.complete = true;

    // Reassemble frame
    std::vector<uint8_t> frame_data;
    for (const auto& packet : frame_assembly.packets) {
      frame_data.insert(frame_data.end(), packet.begin(), packet.end());
    }

    // Write frame to file and decode
    DecodeAndWriteFrame(frame_sequence, frame_data);

    // Clean up old frame assemblies (keep only recent ones)
    if (frame_sequence > last_completed_frame_) {
      last_completed_frame_ = frame_sequence;
      // Remove frames older than 1 frames
      auto it = frame_assemblies_.begin();
      while (it != frame_assemblies_.end()) {
        if (it->first < frame_sequence - 10) {
          it = frame_assemblies_.erase(it);
        } else {
          ++it;
        }
      }
    }
  }
}

void Decoder::DecodeAndWriteFrame(uint32_t frame_sequence,
                         const std::vector<uint8_t>& frame_data) {
  // Write encoded bitstream to file if output stream is set
  // if (output_stream_.is_open()) {
  //   output_stream_.write(reinterpret_cast<const char*>(frame_data.data()),
  //                        frame_data.size());
  //   output_stream_.flush();

  //   if (output_stream_.fail()) {
  //     LOG(ERROR) << "[Decoder] Failed to write encoded frame " << frame_sequence
  //                << " to file";
  //   } else {
  //     LOG(INFO) << "[Decoder] Wrote encoded frame " << frame_sequence
  //               << " size=" << frame_data.size() << " bytes to file";
  //   }
  // }

  // Decode the complete frame (frame_data is already a complete NAL unit/access unit)
  LOG(INFO) << "[Decoder] Decoding frame " << frame_sequence
            << " size=" << frame_data.size() << " bytes";

  vvdecFrame* decoded_frame = DecodeFrame(frame_data.data(), frame_data.size());

  if (decoded_frame != nullptr) {
    if (output_stream_.is_open()) {
      if ( 0 != writeYUVToFile(&output_stream_, decoded_frame, true, false)) {
        LOG(ERROR) << "[Decoder] Failed to write decoded frame to file " << frame_sequence;
      } else {
        // Flush the stream to ensure data is written to disk immediately
        output_stream_.flush();
        if (output_stream_.fail()) {
          LOG(ERROR) << "[Decoder] Failed to flush output stream for frame " << frame_sequence;
        }
        LOG(VERBOSE) << "[Decoder] Successfully wrote decoded frame to file " << frame_sequence << " packet size: " << frame_data.size() << " bytes";
      }
    }

    // Release the frame after processing
    // ReleaseFrame(decoded_frame);
  } else {
    LOG(ERROR) << "[Decoder] Failed to decode frame " << frame_sequence;
  }
}

vvdecFrame* Decoder::DecodeFrame(const uint8_t* frame_data, size_t frame_size) {
  if (!decoder_ || !initialized_) {
    LOG(ERROR) << "[Decoder] Decoder not initialized";
    return nullptr;
  }

  if (!frame_data || frame_size == 0) {
    LOG(ERROR) << "[Decoder] Invalid frame data";
    return nullptr;
  }

  // Copy frame data to access unit (frame_data is already a complete NAL unit)
  std::memcpy(access_unit_.payload, frame_data, frame_size);
  access_unit_.payloadUsedSize = frame_size;

  vvdecNalType eNalType = vvdec_get_nal_unit_type( &access_unit_ );
  bool bIsSlice  = vvdec_is_nal_unit_slice( eNalType );

  // Decode
  vvdecFrame* frame = nullptr;
  int ret = vvdec_decode(decoder_, &access_unit_, &frame);

  if (bIsSlice) {
    access_unit_.cts++;
    access_unit_.dts++;
  }

  // Check result
  if (ret == VVDEC_OK && frame != nullptr) {
    return frame;
  } else if (ret == VVDEC_TRY_AGAIN) {
    LOG(WARNING) << "[Decoder] More data needed to decode frame (VVDEC_TRY_AGAIN) " << vvdec_get_error_msg(ret);
    return nullptr;
  } else if (ret == VVDEC_EOF) {
    LOG(INFO) << "[Decoder] End of stream (VVDEC_EOF)";
    return nullptr;
  } else {
    LOG(ERROR) << "[Decoder] Decoding failed: " << vvdec_get_error_msg(ret);
    if (decoder_) {
      std::string err = vvdec_get_last_error(decoder_);
      if (!err.empty()) {
        LOG(ERROR) << "[Decoder] Error details: " << err;
      }
    }
    return nullptr;
  }
}

void Decoder::ReleaseFrame(vvdecFrame* frame) {
  if (frame && decoder_) {
    vvdec_frame_unref(decoder_, frame);
  }
}

void Decoder::Cleanup() {
  if (!decoder_ && !initialized_) {
    return;
  }

  LOG(INFO) << "[Decoder] Cleaning up decoder";

  if (decoder_) {
    vvdec_decoder_close(decoder_);
    decoder_ = nullptr;
  }

  // Free access unit
  if (access_unit_.payload) {
    vvdec_accessUnit_free_payload(&access_unit_);
    access_unit_.payload = nullptr;
  }

  if (output_stream_.is_open()) {
    // Flush before closing to ensure all data is written to disk
    output_stream_.flush();
    output_stream_.close();
  }

  frame_assemblies_.clear();
  initialized_ = false;
}