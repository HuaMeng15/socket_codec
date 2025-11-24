#include "message_sender.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

#include "log_system/log_system.h"

MessageSender::MessageSender()
    : socket_fd_(-1),
      dest_port_(0),
      max_packet_size_(1400),
      initialized_(false),
      packet_sequence_(0) {}

MessageSender::~MessageSender() { Close(); }

int MessageSender::Initialize(const std::string& dest_ip, int dest_port,
                              size_t max_packet_size) {
  if (initialized_) {
    LOG(WARNING) << "[MessageSender] Already initialized";
    return 0;
  }

  dest_ip_ = dest_ip;
  dest_port_ = dest_port;
  max_packet_size_ = max_packet_size;
  packet_sequence_ = 0;

  // Create UDP socket
  socket_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
  if (socket_fd_ < 0) {
    LOG(ERROR) << "[MessageSender] Failed to create socket: " << strerror(errno);
    return -1;
  }

  // Set up destination address
  struct sockaddr_in dest_addr;
  memset(&dest_addr, 0, sizeof(dest_addr));
  dest_addr.sin_family = AF_INET;
  dest_addr.sin_port = htons(dest_port_);

  if (inet_pton(AF_INET, dest_ip_.c_str(), &dest_addr.sin_addr) <= 0) {
    LOG(ERROR) << "[MessageSender] Invalid IP address: " << dest_ip_;
    close(socket_fd_);
    socket_fd_ = -1;
    return -1;
  }

  // Connect socket to destination (for UDP, this sets default destination)
  if (connect(socket_fd_, (struct sockaddr*)&dest_addr, sizeof(dest_addr)) < 0) {
    LOG(ERROR) << "[MessageSender] Failed to connect socket: " << strerror(errno);
    close(socket_fd_);
    socket_fd_ = -1;
    return -1;
  }

  initialized_ = true;
  LOG(INFO) << "[MessageSender] Initialized: " << dest_ip_ << ":" << dest_port_
            << " max_packet_size=" << max_packet_size_;

  return 0;
}

int MessageSender::SendData(const uint8_t* data, size_t data_size,
                            uint32_t frame_sequence) {
  if (!initialized_ || socket_fd_ < 0) {
    LOG(ERROR) << "[MessageSender] Not initialized";
    return -1;
  }

  if (!data || data_size == 0) {
    LOG(WARNING) << "[MessageSender] Invalid data or size";
    return -1;
  }

  // Calculate payload size per packet (max_packet_size - header size)
  const size_t header_size = sizeof(PacketHeader);
  const size_t max_payload_size = max_packet_size_ - header_size;

  // Calculate number of packets needed
  uint16_t total_packets =
      static_cast<uint16_t>((data_size + max_payload_size - 1) / max_payload_size);

  LOG(INFO) << "[MessageSender] Sending frame " << frame_sequence
            << " size=" << data_size << " bytes in " << total_packets
            << " packets";

  // Allocate packet buffer (use vector to avoid VLA)
  std::vector<uint8_t> packet_buffer(max_packet_size_);

  // Send data in chunks
  size_t offset = 0;
  for (uint16_t packet_index = 0; packet_index < total_packets; packet_index++) {
    // Calculate payload size for this packet
    size_t remaining = data_size - offset;
    size_t payload_size = (remaining > max_payload_size) ? max_payload_size : remaining;

    // Prepare packet with header
    uint8_t* packet = packet_buffer.data();
    PacketHeader* header = reinterpret_cast<PacketHeader*>(packet);
    header->frame_sequence = htonl(frame_sequence);
    header->packet_index = htons(packet_index);
    header->total_packets = htons(total_packets);
    header->payload_size = htonl(static_cast<uint32_t>(payload_size));

    // Copy payload
    memcpy(packet + header_size, data + offset, payload_size);

    // Send packet
    size_t packet_size = header_size + payload_size;
    int ret = SendPacket(packet, packet_size);
    if (ret != 0) {
      LOG(ERROR) << "[MessageSender] Failed to send packet " << packet_index
                 << " of frame " << frame_sequence;
      return ret;
    }

    offset += payload_size;
  }

  LOG(INFO) << "[MessageSender] Successfully sent frame " << frame_sequence
            << " in " << total_packets << " packets";

  return 0;
}

int MessageSender::SendPacket(const uint8_t* packet_data, size_t packet_size) {
  if (socket_fd_ < 0) {
    return -1;
  }

  ssize_t bytes_sent = send(socket_fd_, packet_data, packet_size, 0);
  if (bytes_sent < 0) {
    LOG(ERROR) << "[MessageSender] send() failed: " << strerror(errno);
    return -1;
  }

  if (static_cast<size_t>(bytes_sent) != packet_size) {
    LOG(WARNING) << "[MessageSender] Partial send: " << bytes_sent << "/"
                 << packet_size;
  }

  return 0;
}

void MessageSender::Close() {
  if (socket_fd_ >= 0) {
    close(socket_fd_);
    socket_fd_ = -1;
  }
  initialized_ = false;
}

bool MessageSender::IsInitialized() const { return initialized_; }

