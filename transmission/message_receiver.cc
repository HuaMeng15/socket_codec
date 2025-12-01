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
      message_handler_(nullptr) {}

MessageReceiver::~MessageReceiver() { Close(); }

int MessageReceiver::Initialize(int listen_port) {
  if (initialized_) {
    LOG(WARNING) << "[MessageReceiver] Already initialized";
    return 0;
  }

  listen_port_ = listen_port;

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

  initialized_ = true;
  stop_requested_ = false;
  message_handler_ = nullptr;

  LOG(INFO) << "[MessageReceiver] Initialized: listening on port " << listen_port_;

  return 0;
}

void MessageReceiver::SetMessageHandler(MessageHandler* handler) {
  message_handler_ = handler;
  LOG(VERBOSE) << "[MessageReceiver] Message handler set";
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
      // Pass packet directly to message handler
      if (message_handler_) {
        message_handler_->HandlePacketMessage(buffer.data(),
                                             static_cast<size_t>(bytes_received));
      }
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

  initialized_ = false;
}

bool MessageReceiver::IsInitialized() const { return initialized_; }
