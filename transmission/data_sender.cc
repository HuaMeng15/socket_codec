#include "data_sender.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

#include "log_system/log_system.h"
#include "network_simulator.h"
#include "packet_header.h"
#include "packet_send_time_store.h"
#include "pacer.h"
#include "codec/encoder.h"

DataSender::DataSender()
    : socket_fd_(-1),
      dest_port_(0),
      max_packet_size_(kDefaultMaxPacketSize),
      initialized_(false),
      packet_sequence_(0) {}

DataSender::~DataSender() { Close(); }

int DataSender::Initialize(const std::string& dest_ip, int dest_port,
                              size_t max_packet_size) {
  if (initialized_) {
    LOG(WARNING) << "[DataSender] Already initialized";
    return 0;
  }

  dest_ip_ = dest_ip;
  dest_port_ = dest_port;
  max_packet_size_ = max_packet_size;
  packet_sequence_ = 0;

  // Create UDP socket
  socket_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
  if (socket_fd_ < 0) {
    LOG(ERROR) << "[DataSender] Failed to create socket: " << strerror(errno);
    return -1;
  }

  // Set up destination address
  struct sockaddr_in dest_addr;
  memset(&dest_addr, 0, sizeof(dest_addr));
  dest_addr.sin_family = AF_INET;
  dest_addr.sin_port = htons(dest_port_);

  if (inet_pton(AF_INET, dest_ip_.c_str(), &dest_addr.sin_addr) <= 0) {
    LOG(ERROR) << "[DataSender] Invalid IP address: " << dest_ip_;
    close(socket_fd_);
    socket_fd_ = -1;
    return -1;
  }

  // Connect socket to destination (for UDP, this sets default destination)
  if (connect(socket_fd_, (struct sockaddr*)&dest_addr, sizeof(dest_addr)) < 0) {
    LOG(ERROR) << "[DataSender] Failed to connect socket: " << strerror(errno);
    close(socket_fd_);
    socket_fd_ = -1;
    return -1;
  }

  network_sender_.SetSocketFd(socket_fd_);

  initialized_ = true;
  LOG(INFO) << "[DataSender] Initialized: " << dest_ip_ << ":" << dest_port_
            << " max_packet_size=" << max_packet_size_;

  return 0;
}

int DataSender::SendFrame(const EncodedData* encoded_data) {
  if (!initialized_ || socket_fd_ < 0) {
    LOG(ERROR) << "[DataSender] Not initialized";
    return -1;
  }

  if (!encoded_data || encoded_data->size == 0 ||
      encoded_data->data_ptrs.empty() || encoded_data->data_sizes.empty()) {
    LOG(WARNING) << "[DataSender] Invalid data or size";
    return -1;
  }

  // Calculate total packets considering the max payload size
  const size_t header_size = sizeof(FramePacketHeader);
  const size_t max_payload_size = max_packet_size_ - header_size;
  uint8_t total_packets = static_cast<uint8_t>(encoded_data->data_ptrs.size()); /* nals */
  for (size_t i = 0; i < encoded_data->data_sizes.size(); i++) {
    total_packets += static_cast<uint8_t>(encoded_data->data_sizes[i] / max_payload_size);
  }

  uint16_t frame_sequence = encoded_data->sequence_number;

  LOG(VERBOSE) << "[DataSender] Sending frame " << frame_sequence
            << " size=" << encoded_data->size << " bytes in " << (int)total_packets
            << " packets, nals=" << encoded_data->data_ptrs.size();

  // Allocate packet buffer (use vector to avoid VLA)
  std::vector<uint8_t> packet_buffer(max_packet_size_);

  // Send data in chunks
  uint8_t packet_index = 0;
  for (size_t i = 0; i < encoded_data->data_ptrs.size(); i++) {
    uint8_t* data = encoded_data->data_ptrs[i];
    size_t data_size = encoded_data->data_sizes[i];
    size_t offset = 0;
    while (offset < data_size) {
      size_t remaining = data_size - offset;
      uint16_t payload_size = static_cast<uint16_t>((remaining > max_payload_size) ? max_payload_size : remaining);
      // Prepare packet with header
      uint8_t* packet = packet_buffer.data();
      FramePacketHeader* header = reinterpret_cast<FramePacketHeader*>(packet);
      header->frame_sequence = htons(frame_sequence);
      header->packet_index = packet_index;
      header->total_packets = total_packets;
      header->payload_size = htons(payload_size);

      // Copy payload
      memcpy(packet + header_size, data + offset, payload_size);

      size_t packet_size = header_size + payload_size;
      if (pacer_) {
        pacer_->Pace(packet_size);
      }
      if (send_time_store_) {
        send_time_store_->Record(frame_sequence, packet_index);
      }
      // Send packet
      int ret = SendPacket(packet, packet_size);
      if (ret != 0) {
        LOG(ERROR) << "[DataSender] Failed to send packet " << (int)packet_index
                   << " of frame " << frame_sequence;
        return ret;
      }
      LOG(VERBOSE) << "[DataSender] Sent packet " << (int)packet_index
                   << " for frame " << frame_sequence;

      offset += payload_size;
      packet_index++;
    }
  }

  LOG(VERBOSE) << "[DataSender] Successfully sent frame " << frame_sequence
            << " in " << (int)total_packets << " packets";

  return 0;
}

int DataSender::SendFeedback(uint16_t frame_sequence, uint8_t packet_index, FeedbackType feedback_type) {
  if (!initialized_ || socket_fd_ < 0) {
    LOG(ERROR) << "[DataSender] Not initialized";
    return -1;
  }

  // Calculate packet size (header + payload)
  const size_t header_size = sizeof(FeedbackPacketHeader);

  // Prepare packet with feedback header
  std::vector<uint8_t> packet_buffer(header_size);
  FeedbackPacketHeader* header = reinterpret_cast<FeedbackPacketHeader*>(packet_buffer.data());
  header->frame_sequence = htons(frame_sequence);
  header->packet_index = packet_index;
  header->feedback_type = static_cast<uint8_t>(feedback_type);

  int ret = SendPacket(packet_buffer.data(), header_size);
  if (ret != 0) {
    LOG(ERROR) << "[DataSender] Failed to send feedback for frame "
               << frame_sequence << " packet " << (int)packet_index;
    return ret;
  }

  return 0;
}

int DataSender::SendPacket(const uint8_t* packet_data, size_t packet_size) {
  if (socket_fd_ < 0) {
    return -1;
  }

  int ret = network_sender_.Send(packet_data, packet_size);
  if (ret < 0) {
    LOG(ERROR) << "[DataSender] NetworkSender::Send() failed";
    return -1;
  }
  // ret == 1 means simulator dropped it (simulated loss); treat as success
  return 0;
}

void DataSender::SetSimulator(NetworkSimulator* simulator) {
  network_sender_.SetSimulator(simulator);
}

void DataSender::Close() {
  if (socket_fd_ >= 0) {
    close(socket_fd_);
    socket_fd_ = -1;
  }
  initialized_ = false;
}

bool DataSender::IsInitialized() const { return initialized_; }

