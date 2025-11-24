#include "message_receiver.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>

#include "log_system/log_system.h"

MessageReceiver::MessageReceiver()
    : socket_fd_(-1),
      listen_port_(0),
      initialized_(false),
      stop_requested_(false),
      last_completed_frame_(0) {}

MessageReceiver::~MessageReceiver() { Close(); }

int MessageReceiver::Initialize(int listen_port, const std::string& output_file) {
  if (initialized_) {
    LOG(WARNING) << "[MessageReceiver] Already initialized";
    return 0;
  }

  listen_port_ = listen_port;
  output_file_ = output_file;

  // Create UDP socket
  socket_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
  if (socket_fd_ < 0) {
    LOG(ERROR) << "[MessageReceiver] Failed to create socket: " << strerror(errno);
    return -1;
  }

  // Set up local address for binding
  struct sockaddr_in local_addr;
  memset(&local_addr, 0, sizeof(local_addr));
  local_addr.sin_family = AF_INET;
  local_addr.sin_port = htons(listen_port_);
  local_addr.sin_addr.s_addr = INADDR_ANY;  // Listen on all interfaces

  // Bind socket to local address
  if (bind(socket_fd_, (struct sockaddr*)&local_addr, sizeof(local_addr)) < 0) {
    LOG(ERROR) << "[MessageReceiver] Failed to bind socket: " << strerror(errno);
    close(socket_fd_);
    socket_fd_ = -1;
    return -1;
  }

  // Open output file
  output_stream_.open(output_file_, std::ios::out | std::ios::binary | std::ios::trunc);
  if (!output_stream_.is_open()) {
    LOG(ERROR) << "[MessageReceiver] Failed to open output file: " << output_file_;
    close(socket_fd_);
    socket_fd_ = -1;
    return -1;
  }

  initialized_ = true;
  stop_requested_ = false;
  last_completed_frame_ = 0;
  frame_assemblies_.clear();

  LOG(INFO) << "[MessageReceiver] Initialized: listening on port " << listen_port_
            << " output file: " << output_file_;

  return 0;
}

void MessageReceiver::Run() {
  if (!initialized_ || socket_fd_ < 0) {
    LOG(ERROR) << "[MessageReceiver] Not initialized";
    return;
  }

  LOG(INFO) << "[MessageReceiver] Starting receiver loop...";

  const size_t buffer_size = 1500;  // Standard Ethernet MTU
  std::vector<uint8_t> buffer(buffer_size);

  while (!stop_requested_) {
    ssize_t bytes_received = 0;
    int ret = ReceivePacket(buffer.data(), buffer_size, bytes_received);

    if (ret == 0 && bytes_received > 0) {
      ProcessPacket(buffer.data(), static_cast<size_t>(bytes_received));
    } else if (ret < 0 && !stop_requested_) {
      // Error receiving, but continue if not stopped
      LOG(WARNING) << "[MessageReceiver] Error receiving packet, continuing...";
    }
  }

  LOG(INFO) << "[MessageReceiver] Receiver loop stopped";
}

int MessageReceiver::ReceivePacket(uint8_t* buffer, size_t buffer_size,
                                    ssize_t& bytes_received) {
  if (socket_fd_ < 0) {
    return -1;
  }

  struct sockaddr_in sender_addr;
  socklen_t sender_addr_len = sizeof(sender_addr);

  bytes_received = recvfrom(socket_fd_, buffer, buffer_size, 0,
                            (struct sockaddr*)&sender_addr, &sender_addr_len);

  if (bytes_received < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      // Timeout or would block - not an error
      return 0;
    }
    if (!stop_requested_) {
      LOG(ERROR) << "[MessageReceiver] recvfrom() failed: " << strerror(errno);
    }
    return -1;
  }

  return 0;
}

void MessageReceiver::ProcessPacket(const uint8_t* packet_data, size_t packet_size) {
  if (packet_size < sizeof(PacketHeader)) {
    LOG(WARNING) << "[MessageReceiver] Packet too small, ignoring";
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
    LOG(WARNING) << "[MessageReceiver] Packet size mismatch for frame "
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
    LOG(INFO) << "[MessageReceiver] Starting frame " << frame_sequence
              << " expecting " << total_packets << " packets";
  }

  // Validate packet index
  if (packet_index >= total_packets) {
    LOG(WARNING) << "[MessageReceiver] Invalid packet index " << packet_index
                 << " for frame " << frame_sequence;
    return;
  }

  // Store packet payload
  if (frame_assembly.packets[packet_index].empty()) {
    // New packet for this frame
    const uint8_t* payload = packet_data + sizeof(PacketHeader);
    frame_assembly.packets[packet_index].assign(payload, payload + payload_size);
    frame_assembly.received_packets++;

    LOG(INFO) << "[MessageReceiver] Received packet " << packet_index << "/"
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

    // Write frame to file
    WriteFrame(frame_sequence, frame_data);

    // Clean up old frame assemblies (keep only recent ones)
    if (frame_sequence > last_completed_frame_) {
      last_completed_frame_ = frame_sequence;
      // Remove frames older than 10 frames
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

void MessageReceiver::WriteFrame(uint32_t frame_sequence,
                                  const std::vector<uint8_t>& frame_data) {
  if (!output_stream_.is_open()) {
    LOG(ERROR) << "[MessageReceiver] Output file not open";
    return;
  }

  output_stream_.write(reinterpret_cast<const char*>(frame_data.data()),
                       frame_data.size());
  output_stream_.flush();

  if (output_stream_.fail()) {
    LOG(ERROR) << "[MessageReceiver] Failed to write frame " << frame_sequence
               << " to file";
  } else {
    LOG(INFO) << "[MessageReceiver] Wrote frame " << frame_sequence
              << " size=" << frame_data.size() << " bytes to file";
  }
}

void MessageReceiver::Stop() {
  stop_requested_ = true;
  // Wake up recvfrom by closing socket (will be reopened if needed)
  if (socket_fd_ >= 0) {
    shutdown(socket_fd_, SHUT_RD);
  }
}

bool MessageReceiver::IsStopped() const { return stop_requested_.load(); }

void MessageReceiver::Close() {
  Stop();

  if (socket_fd_ >= 0) {
    close(socket_fd_);
    socket_fd_ = -1;
  }

  if (output_stream_.is_open()) {
    output_stream_.close();
  }

  frame_assemblies_.clear();
  initialized_ = false;
}

bool MessageReceiver::IsInitialized() const { return initialized_; }
