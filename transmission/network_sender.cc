#include "network_sender.h"

#include <cerrno>
#include <cstring>
#include <sys/socket.h>

#include "log_system/log_system.h"
#include "network_simulator.h"

NetworkSender::NetworkSender()
    : socket_fd_(-1),
      simulator_(nullptr) {
}

void NetworkSender::SetSocketFd(int fd) {
  socket_fd_ = fd;
}

void NetworkSender::SetSimulator(NetworkSimulator* simulator) {
  simulator_ = simulator;
  if (simulator_) {
    // The simulator delivers (delayed) packets back to the real socket.
    int fd = socket_fd_;
    simulator_->SetDeliverCallback([fd](const uint8_t* data, size_t size) {
      if (fd < 0) return;
      ssize_t n = send(fd, data, size, 0);
      if (n < 0) {
        LOG(ERROR) << "[NetworkSender] delayed send() failed: " << strerror(errno);
      }
    });
    simulator_->Start();
  }
}

int NetworkSender::Send(const uint8_t* data, size_t size) {
  if (socket_fd_ < 0) {
    return -1;
  }

  // With a simulator attached, enqueue for async (delayed) delivery. This does
  // NOT block the sender, so send timestamps reflect the sender's paced rate,
  // and queuing delay at the bottleneck becomes observable to GCC.
  if (simulator_) {
    if (!simulator_->Enqueue(data, size)) {
      return 1;  // dropped (random loss or queue overflow)
    }
    return 0;
  }

  // No simulator: send directly.
  ssize_t bytes_sent = send(socket_fd_, data, size, 0);
  if (bytes_sent < 0) {
    LOG(ERROR) << "[NetworkSender] send() failed: " << strerror(errno);
    return -1;
  }

  return 0;
}
