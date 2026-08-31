#include "data_receiver.h"

#include <arpa/inet.h>
#include <chrono>
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

  // Keep enough queued datagrams to survive a short decoder/logger scheduling
  // pause without losing packets. Linux doubles this value internally.
  int receive_buffer_bytes = 4 * 1024 * 1024;
  if (setsockopt(socket_fd_, SOL_SOCKET, SO_RCVBUF, &receive_buffer_bytes,
                 sizeof(receive_buffer_bytes)) < 0) {
    LOG(WARNING) << "[DataReceiver] Failed to enlarge receive buffer: "
                 << strerror(errno);
  }

#if defined(__linux__) && defined(SO_TIMESTAMPNS)
  // Ask the kernel to timestamp each datagram when it enters the socket queue.
  // This timestamp remains correct even if userspace is briefly descheduled or
  // blocked writing logs before it calls recvmsg().
  int enable_timestamp = 1;
  if (setsockopt(socket_fd_, SOL_SOCKET, SO_TIMESTAMPNS, &enable_timestamp,
                 sizeof(enable_timestamp)) < 0) {
    LOG(WARNING) << "[DataReceiver] Kernel receive timestamps unavailable: "
                 << strerror(errno);
  }
#endif

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

    // Data available. Drain the socket queue before returning to select(); this
    // reduces queueing and overflow when several packets become ready together.
    if (FD_ISSET(socket_fd_, &read_fds)) {
      while (!stop_requested_) {
        ssize_t bytes_received = 0;
        int64_t arrival_time_us = 0;
        int ret = ReceivePacket(buffer.data(), buffer_size, bytes_received,
                                arrival_time_us);

        if (ret == 0 && bytes_received > 0) {
          if (message_handler_) {
            message_handler_->HandlePacketMessageWithTimestamp(
                buffer.data(), static_cast<size_t>(bytes_received),
                arrival_time_us);
          }
          continue;
        }
        if (ret < 0 && !stop_requested_ && errno != EAGAIN &&
            errno != EWOULDBLOCK) {
          LOG(WARNING) << "[DataReceiver] Error receiving packet: "
                       << strerror(errno);
        }
        break;
      }
    }
  }

  LOG(INFO) << "[DataReceiver] Receiver loop stopped";
}

int DataReceiver::ReceivePacket(uint8_t* buffer, size_t buffer_size,
                                ssize_t& bytes_received,
                                int64_t& arrival_time_us) {
  if (socket_fd_ < 0) {
    return -1;
  }

  struct sockaddr_in sender_addr;
  memset(&sender_addr, 0, sizeof(sender_addr));
  struct iovec iov;
  iov.iov_base = buffer;
  iov.iov_len = buffer_size;
  char control[CMSG_SPACE(sizeof(struct timespec))];
  memset(control, 0, sizeof(control));
  struct msghdr msg;
  memset(&msg, 0, sizeof(msg));
  msg.msg_name = &sender_addr;
  msg.msg_namelen = sizeof(sender_addr);
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = control;
  msg.msg_controllen = sizeof(control);

  bytes_received = recvmsg(socket_fd_, &msg, 0);

  if (bytes_received < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      // Timeout or would block - not an error
      return 0;
    }
    if (!stop_requested_) {
      LOG(ERROR) << "[DataReceiver] recvmsg() failed: " << strerror(errno);
    }
    return -1;
  }

  // Fallback uses CLOCK_REALTIME's C++ equivalent, matching SO_TIMESTAMPNS's
  // clock domain. Only relative deltas are put on the wire.
  arrival_time_us = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
#if defined(__linux__) && defined(SO_TIMESTAMPNS)
  for (struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg); cmsg != nullptr;
       cmsg = CMSG_NXTHDR(&msg, cmsg)) {
    if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_TIMESTAMPNS) {
      const auto* ts = reinterpret_cast<const struct timespec*>(CMSG_DATA(cmsg));
      arrival_time_us = static_cast<int64_t>(ts->tv_sec) * 1000000LL +
                        static_cast<int64_t>(ts->tv_nsec) / 1000LL;
      break;
    }
  }
#endif

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
