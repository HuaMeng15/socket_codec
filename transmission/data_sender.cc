#include "data_sender.h"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <limits>
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
  dry_run_ = false;

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

void DataSender::InitializeForTesting(size_t max_packet_size) {
  dest_ip_.clear();
  dest_port_ = 0;
  max_packet_size_ = max_packet_size;
  packet_sequence_ = 0;
  dry_run_ = true;
  initialized_ = true;
  socket_fd_ = -1;
  network_sender_.SetSocketFd(-1);
  LOG(INFO) << "[DataSender] Initialized in dry-run mode for testing: max_packet_size="
            << max_packet_size_;
}

void DataSender::SetPacer(Pacer* pacer) {
  pacer_ = pacer;
  if (!pacer_) return;
  // The pacer thread is the sole caller of SendPacket once wired. It records
  // each real packet's send-time at actual send (not enqueue) so the delay
  // trendline uses true wire departure times; padding gets no send-time.
  pacer_->SetSendCallback([this](const uint8_t* data, size_t size) {
    SendPacket(data, size);
  });
  pacer_->SetRecordCallback([this](uint16_t frame_sequence, uint16_t packet_index) {
    if (send_time_store_) send_time_store_->Record(frame_sequence, packet_index);
  });
  pacer_->SetMaxPacketSize(max_packet_size_);
  pacer_->Start();
}

size_t DataSender::CountFramePackets(const EncodedData* encoded_data) const {
  if (!encoded_data || encoded_data->size == 0 ||
      encoded_data->data_ptrs.empty() || encoded_data->data_sizes.empty()) {
    return 0;
  }

  const size_t header_size = sizeof(FramePacketHeader);
  if (max_packet_size_ <= header_size) {
    return 0;
  }
  const size_t max_payload_size = max_packet_size_ - header_size;

  size_t total_packets_count = 0;
  for (size_t i = 0; i < encoded_data->data_sizes.size(); i++) {
    size_t nal_size = encoded_data->data_sizes[i];
    total_packets_count += (nal_size + max_payload_size - 1) / max_payload_size;
  }
  return total_packets_count;
}

int DataSender::SendFrame(const EncodedData* encoded_data) {
  size_t total_packets_count = CountFramePackets(encoded_data);
  if (total_packets_count == 0) {
    LOG(WARNING) << "[DataSender] Invalid data or size";
    return -1;
  }
  if (total_packets_count > std::numeric_limits<uint16_t>::max()) {
    LOG(ERROR) << "[DataSender] Frame needs " << total_packets_count
               << " packets, but header supports at most "
               << std::numeric_limits<uint16_t>::max();
    return -1;
  }

  uint16_t packets_sent = 0;
  return SendFrameFragment(encoded_data, 0,
                           static_cast<uint16_t>(total_packets_count),
                           &packets_sent);
}

int DataSender::SendFrameFragment(const EncodedData* encoded_data,
                                  uint16_t first_packet_index,
                                  uint16_t total_packets_for_header,
                                  uint16_t* packets_sent) {
  if (!initialized_) {
    LOG(ERROR) << "[DataSender] Not initialized";
    return -1;
  }

  size_t fragment_packet_count = CountFramePackets(encoded_data);
  if (fragment_packet_count == 0) {
    LOG(WARNING) << "[DataSender] Invalid data or size";
    return -1;
  }
  if (static_cast<size_t>(first_packet_index) + fragment_packet_count >
      std::numeric_limits<uint16_t>::max()) {
    LOG(ERROR) << "[DataSender] Fragment exceeds packet-index range: first="
               << (int)first_packet_index << " count=" << fragment_packet_count;
    return -1;
  }
  if (total_packets_for_header != 0 &&
      first_packet_index + fragment_packet_count > total_packets_for_header) {
    LOG(ERROR) << "[DataSender] Fragment extends beyond declared frame total";
    return -1;
  }

  uint16_t frame_sequence = encoded_data->sequence_number;
  const size_t header_size = sizeof(FramePacketHeader);
  const size_t max_payload_size = max_packet_size_ - header_size;

  LOG(VERBOSE) << "[DataSender] Sending frame " << frame_sequence
               << " fragment size=" << encoded_data->size
               << " bytes in " << fragment_packet_count
               << " packets, first_packet=" << (int)first_packet_index
               << " total_header=" << (int)total_packets_for_header
               << " nals=" << encoded_data->data_ptrs.size();

  // Allocate packet buffer (use vector to avoid VLA)
  std::vector<uint8_t> packet_buffer(max_packet_size_);

  // Send data in chunks
  uint32_t packet_index = first_packet_index;
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
      header->packet_index = htons(static_cast<uint16_t>(packet_index));
      header->total_packets = htons(total_packets_for_header);
      header->payload_size = htons(payload_size);

      // Copy payload
      memcpy(packet + header_size, data + offset, payload_size);

      size_t packet_size = header_size + payload_size;
      if (pacer_) {
        // Hand the packet to the pacer thread, which paces it (bounded token
        // bucket), records its send-time at actual send, and may interleave
        // probe padding. Enqueue copies the bytes, so packet_buffer is reusable.
        pacer_->Enqueue(packet, packet_size, frame_sequence,
                        static_cast<uint16_t>(packet_index));
      } else {
        if (dry_run_) {
          // Unit-test mode: exercise packetization and callbacks without a
          // network socket. This keeps the framing path deterministic under
          // sandboxed test runners.
        } else {
          // No pacer: send inline immediately (record send-time at send).
          if (send_time_store_) {
            send_time_store_->Record(frame_sequence,
                                     static_cast<uint16_t>(packet_index));
          }
          int ret = SendPacket(packet, packet_size);
          if (ret != 0) {
            LOG(ERROR) << "[DataSender] Failed to send packet " << (int)packet_index
                       << " of frame " << frame_sequence;
            return ret;
          }
        }
        LOG(VERBOSE) << "[DataSender] Sent packet " << (int)packet_index
                     << " for frame " << frame_sequence;
      }

      offset += payload_size;
      packet_index++;
    }
  }

  const int64_t enqueue_done_us =
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count();
  LOG(VERBOSE) << "[DataSender] Successfully sent frame " << frame_sequence
               << " fragment in " << (int)(packet_index - first_packet_index)
               << " packets enqueue_done_us=" << enqueue_done_us;

  uint16_t sent_count =
      static_cast<uint16_t>(packet_index - first_packet_index);
  if (packets_sent) {
    *packets_sent = sent_count;
  }
  if (packets_sent_cb_) {
    packets_sent_cb_(sent_count);
  }

  return 0;
}

int DataSender::SendFeedback(uint16_t frame_sequence, uint16_t packet_index,
                             FeedbackType feedback_type) {
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
  header->packet_index = htons(packet_index);
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
  if (dry_run_) {
    return 0;
  }
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

int DataSender::SendRawFeedback(const uint8_t* data, size_t size) {
  if (!initialized_) {
    return -1;
  }
  if (dry_run_) {
    return 0;
  }
  if (socket_fd_ < 0) {
    return -1;
  }
  return SendPacket(data, size);
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
  dry_run_ = false;
}

bool DataSender::IsInitialized() const { return initialized_; }
