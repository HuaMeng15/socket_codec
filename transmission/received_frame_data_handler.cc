#include "received_frame_data_handler.h"

#include <arpa/inet.h>
#include <chrono>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <sstream>

#include "log_system/log_system.h"
#include "tools/yuv_file_io.h"
#include "codec/codec_factory.h"

ReceivedFrameDataHandler::ReceivedFrameDataHandler(CodecType codec_type,
                                         int width,
                                         int height,
                                         DataReceiver* data_receiver,
                                         int feedback_port,
                                         const std::string& output_file)
    : codec_type_(codec_type),
      width_(width),
      height_(height),
      data_receiver_(data_receiver),
      feedback_port_(feedback_port),
      feedback_sender_initialized_(false),
      initialized_(false),
      last_completed_frame_(0),
      output_file_(output_file) {
}

ReceivedFrameDataHandler::~ReceivedFrameDataHandler() {
  if (decoder_) {
    decoder_->Cleanup();
  }
  if (feedback_sender_initialized_) {
    feedback_sender_.Close();
  }
  if (output_stream_.is_open()) {
    output_stream_.flush();
    output_stream_.close();
  }
}

int ReceivedFrameDataHandler::Initialize() {
  if (initialized_) {
    LOG(WARNING) << "[ReceivedFrameDataHandler] Already initialized";
    return 0;
  }

  // Create decoder
  decoder_ = CodecFactory::CreateDecoder(codec_type_);
  if (!decoder_) {
    LOG(ERROR) << "[ReceivedFrameDataHandler] Failed to create decoder";
    return -1;
  }

  // Initialize decoder
  if (0 != decoder_->Initialize(width_, height_)) {
    LOG(ERROR) << "[ReceivedFrameDataHandler] Failed to initialize decoder";
    return -1;
  }

  // Don't initialize feedback sender here - wait until we receive first packet
  // so we know the sender's IP address
  feedback_sender_initialized_ = false;

  // Open output file if specified
  if (!output_file_.empty()) {
    output_stream_.open(output_file_, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!output_stream_.is_open()) {
      LOG(ERROR) << "[ReceivedFrameDataHandler] Failed to open output file: " << output_file_;
      return -1;
    }
    LOG(VERBOSE) << "[ReceivedFrameDataHandler] Output file opened: " << output_file_;
  }

  frame_assemblies_.clear();
  last_completed_frame_ = 0;
  initialized_ = true;

  LOG(INFO) << "[ReceivedFrameDataHandler] Initialized";
  return 0;
}

int ReceivedFrameDataHandler::HandlePacketMessage(const uint8_t* packet_data,
                                             size_t packet_size) {
  if (!initialized_) {
    LOG(ERROR) << "[ReceivedFrameDataHandler] Not initialized";
    return -1;
  }

  ProcessPacket(packet_data, packet_size);
  return 0;
}

void ReceivedFrameDataHandler::ProcessPacket(const uint8_t* packet_data, size_t packet_size) {
  if (packet_size < sizeof(FramePacketHeader)) {
    LOG(WARNING) << "[ReceivedFrameDataHandler] Packet too small, ignoring";
    return;
  }

  // Extract header
  const FramePacketHeader* header = reinterpret_cast<const FramePacketHeader*>(packet_data);
  uint16_t frame_sequence = ntohs(header->frame_sequence);
  uint8_t packet_index = header->packet_index;
  uint8_t total_packets = header->total_packets;
  uint16_t payload_size = ntohs(header->payload_size);

  // Validate payload size
  size_t expected_packet_size = sizeof(FramePacketHeader) + payload_size;
  if (packet_size < expected_packet_size) {
    LOG(WARNING) << "[ReceivedFrameDataHandler] Packet size mismatch for frame "
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
    LOG(INFO) << "[ReceivedFrameDataHandler] Starting frame " << frame_sequence
              << " expecting " << (int)total_packets << " packets";
  }

  // Validate packet index
  if (packet_index >= total_packets) {
    LOG(WARNING) << "[ReceivedFrameDataHandler] Invalid packet index " << packet_index
                 << " for frame " << frame_sequence;
    return;
  }

  // Store packet payload (only if not already received)
  if (frame_assembly.packets[packet_index].empty()) {
    const uint8_t* payload = packet_data + sizeof(FramePacketHeader);
    frame_assembly.packets[packet_index].assign(payload, payload + payload_size);
    frame_assembly.received_packets++;

    LOG(VERBOSE) << "[ReceivedFrameDataHandler] Received packet " << (int)packet_index
                 << " for frame " << (int)frame_sequence
                 << " bytes=" << (int)payload_size
                 << " (" << (int)frame_assembly.received_packets << "/" << (int)total_packets
                 << " complete)";

    // Send feedback for received packet
    SendFeedback(frame_sequence, packet_index);
  }

  // Check if frame is complete
  if (frame_assembly.received_packets == total_packets && !frame_assembly.complete) {
    frame_assembly.complete = true;

    // Reassemble frame
    std::vector<uint8_t> frame_data;
    for (const auto& packet : frame_assembly.packets) {
      frame_data.insert(frame_data.end(), packet.begin(), packet.end());
    }

    // Handle complete frame (decode and write to file)
    HandleCompleteFrame(frame_sequence, frame_data);

    // Flush any pending TWCC feedback at frame boundary to bound latency
    feedback_collector_.Flush();

    // Clean up old frame assemblies (keep only recent ones)
    if (frame_sequence > last_completed_frame_) {
      last_completed_frame_ = frame_sequence;
      // Remove frames older than 10 frames; report any still-missing packets
      // as losses before evicting (gap-based loss detection).
      auto it = frame_assemblies_.begin();
      while (it != frame_assemblies_.end()) {
        if (it->first < frame_sequence - 10) {
          if (!it->second.complete) {
            ReportFrameLoss(static_cast<uint16_t>(it->first), it->second);
          }
          it = frame_assemblies_.erase(it);
        } else {
          ++it;
        }
      }
    }
  }
}

void ReceivedFrameDataHandler::ReportFrameLoss(uint16_t frame_sequence,
                                               const FrameAssembly& assembly) {
  std::vector<bool> received_mask(assembly.total_packets, false);
  for (uint8_t i = 0; i < assembly.total_packets && i < assembly.packets.size(); i++) {
    received_mask[i] = !assembly.packets[i].empty();
  }
  auto lost = FeedbackCollector::DetectLoss(frame_sequence,
                                            assembly.total_packets, received_mask);
  if (!lost.empty()) {
    feedback_collector_.SendLossReport(lost);
    LOG(VERBOSE) << "[ReceivedFrameDataHandler] Reported " << lost.size()
                 << " lost packets for frame " << frame_sequence;
  }
}

void ReceivedFrameDataHandler::SendFeedback(uint16_t frame_sequence, uint8_t packet_index) {
  // Try to initialize feedback sender if not already initialized
  if (!feedback_sender_initialized_) {
    if (InitializeFeedbackSender()) {
      feedback_sender_initialized_ = true;
      // Wire the collector to send via the feedback sender (TWCC-style)
      feedback_collector_.SetSendCallback(
          [this](const uint8_t* data, size_t size) {
            feedback_sender_.SendRawFeedback(data, size);
          });
      LOG(INFO) << "[ReceivedFrameDataHandler] Feedback sender initialized on first feedback";
    } else {
      // Still no sender info available, skip this feedback
      return;
    }
  }

  if (!feedback_sender_.IsInitialized()) {
    LOG(WARNING) << "[ReceivedFrameDataHandler] Feedback sender not initialized";
    return;
  }

  // Feed arrival into collector — it batches and sends TWCC TransportFeedback.
  // This is what the GCC delay-based estimator consumes.
  feedback_collector_.OnPacketReceived(frame_sequence, packet_index);
}

void ReceivedFrameDataHandler::HandleCompleteFrame(uint32_t frame_sequence,
                                                    const std::vector<uint8_t>& frame_data) {
  if (!decoder_) {
    LOG(ERROR) << "[ReceivedFrameDataHandler] Decoder is null";
    return;
  }

  // Decode the complete frame
  YUVBuffer* decoded_frame = decoder_->DecodeFrame(frame_data.data(), frame_data.size());
  
  if (decoded_frame) {
    // Write decoded frame to file if output stream is set
    if (output_stream_.is_open()) {
      if (writeYUVToFile(&output_stream_, decoded_frame, false, false) != 0) {
        LOG(ERROR) << "[ReceivedFrameDataHandler] Failed to write YUV frame to file";
      }
    }

    LOG(INFO) << "[ReceivedFrameDataHandler] Successfully decoded frame " << frame_sequence;

    // Release the decoded frame
    decoder_->ReleaseFrame(decoded_frame);
  } else {
    LOG(WARNING) << "[ReceivedFrameDataHandler] Failed to decode frame " << frame_sequence;
  }
}

bool ReceivedFrameDataHandler::InitializeFeedbackSender() {
  if (!data_receiver_) {
    return false;
  }

  // Get sender information from data receiver
  std::string sender_ip;
  int sender_port;
  if (!data_receiver_->GetLastSenderInfo(sender_ip, sender_port)) {
    LOG(WARNING) << "[ReceivedFrameDataHandler] No sender info available yet";
    return false;
  }

  // Initialize feedback sender to send back to the sender on feedback port
  if (feedback_sender_.Initialize(sender_ip, feedback_port_) != 0) {
    LOG(ERROR) << "[ReceivedFrameDataHandler] Failed to initialize feedback sender to "
               << sender_ip << ":" << feedback_port_;
    return false;
  }

  LOG(INFO) << "[ReceivedFrameDataHandler] Feedback sender initialized to send to "
            << sender_ip << ":" << feedback_port_;
  return true;
}
