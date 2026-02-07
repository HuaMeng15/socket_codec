#include "data_receiver.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>

#include "log_system/log_system.h"

DataReceiver::DataReceiver()
    : socket_fd_(-1),
      listen_port_(0),
      initialized_(false),
      stop_requested_(false),
      message_handler_(nullptr),
      last_sender_port_(0),
      has_sender_info_(false) {}

DataReceiver::~DataReceiver() { Close(); }

int DataReceiver::Initialize(int listen_port) {
  if (initialized_) {
    LOG(WARNING) << "[DataReceiver] Already initialized";
    return 0;
  }

  listen_port_ = listen_port;

  // Create UDP socket
  socket_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
  if (socket_fd_ < 0) {
    LOG(ERROR) << "[DataReceiver] Failed to create socket: " << strerror(errno);
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
    LOG(ERROR) << "[DataReceiver] Failed to bind socket: " << strerror(errno);
    close(socket_fd_);
    socket_fd_ = -1;
    return -1;
  }

  // Set socket to non-blocking mode for responsive shutdown
  int flags = fcntl(socket_fd_, F_GETFL, 0);
  if (flags < 0) {
    LOG(ERROR) << "[DataReceiver] Failed to get socket flags: " << strerror(errno);
    close(socket_fd_);
    socket_fd_ = -1;
    return -1;
  }
  if (fcntl(socket_fd_, F_SETFL, flags | O_NONBLOCK) < 0) {
    LOG(ERROR) << "[DataReceiver] Failed to set socket to non-blocking: " << strerror(errno);
    close(socket_fd_);
    socket_fd_ = -1;
    return -1;
  }

  initialized_ = true;
  stop_requested_ = false;
  message_handler_ = nullptr;

  LOG(INFO) << "[DataReceiver] Initialized: listening on port " << listen_port_;

  return 0;
}

void DataReceiver::SetMessageHandler(MessageHandler* handler) {
  message_handler_ = handler;
  LOG(VERBOSE) << "[DataReceiver] Message handler set";
}

void DataReceiver::Run() {
  if (!initialized_ || socket_fd_ < 0) {
    LOG(ERROR) << "[DataReceiver] Not initialized";
    return;
  }

  LOG(INFO) << "[DataReceiver] Starting receiver loop...";

  const size_t buffer_size = 1500;  // Standard Ethernet MTU
  std::vector<uint8_t> buffer(buffer_size);

  while (!stop_requested_) {
    // Use select() with timeout to periodically check stop_requested_
    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(socket_fd_, &read_fds);

    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 100000;  // 100ms timeout

    int select_ret = select(socket_fd_ + 1, &read_fds, nullptr, nullptr, &timeout);
    
    if (select_ret < 0) {
      if (errno == EINTR) {
        // Interrupted by signal, continue
        continue;
      }
      if (!stop_requested_) {
        LOG(ERROR) << "[DataReceiver] select() failed: " << strerror(errno);
      }
      break;
    }

    if (stop_requested_) {
      break;
    }

    if (select_ret == 0) {
      // Timeout - check stop_requested_ and continue
      continue;
    }

    // Data available, receive packet
    if (FD_ISSET(socket_fd_, &read_fds)) {
      ssize_t bytes_received = 0;
      int ret = ReceivePacket(buffer.data(), buffer_size, bytes_received);

      if (ret == 0 && bytes_received > 0) {
        // Pass packet directly to message handler
        if (message_handler_) {
          message_handler_->HandlePacketMessage(buffer.data(),
                                               static_cast<size_t>(bytes_received));
        }
      } else if (ret < 0 && !stop_requested_) {
        // Error receiving, but continue if not stopped
        // EAGAIN/EWOULDBLOCK is expected with non-blocking socket
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
          LOG(WARNING) << "[DataReceiver] Error receiving packet: " << strerror(errno);
        }
      }
    }
  }

  LOG(INFO) << "[DataReceiver] Receiver loop stopped";
}

int DataReceiver::ReceivePacket(uint8_t* buffer, size_t buffer_size,
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
      LOG(ERROR) << "[DataReceiver] recvfrom() failed: " << strerror(errno);
    }
    return -1;
  }

  // Store sender information for feedback
  char ip_str[INET_ADDRSTRLEN];
  if (inet_ntop(AF_INET, &sender_addr.sin_addr, ip_str, INET_ADDRSTRLEN) != nullptr) {
    last_sender_ip_ = std::string(ip_str);
    last_sender_port_ = ntohs(sender_addr.sin_port);
    has_sender_info_ = true;
  }

  return 0;
}


void DataReceiver::Stop() {
  stop_requested_ = true;
  // With non-blocking socket and select(), the loop will exit quickly
  // No need to shutdown/close here - Close() will handle cleanup
  LOG(INFO) << "[DataReceiver] Stop requested";
}

bool DataReceiver::IsStopped() const { return stop_requested_.load(); }

void DataReceiver::Close() {
  Stop();

  if (socket_fd_ >= 0) {
    close(socket_fd_);
    socket_fd_ = -1;
  }

  initialized_ = false;
}

bool DataReceiver::IsInitialized() const { return initialized_; }

bool DataReceiver::GetLastSenderInfo(std::string& sender_ip,
                                        int& sender_port) const {
  if (!has_sender_info_) {
    return false;
  }
  sender_ip = last_sender_ip_;
  sender_port = last_sender_port_;
  return true;
}

