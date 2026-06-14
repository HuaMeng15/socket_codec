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
}

int NetworkSender::Send(const uint8_t* data, size_t size) {
  if (socket_fd_ < 0) {
    return -1;
  }

  // If simulator is attached, process through it first
  if (simulator_) {
    if (!simulator_->ProcessPacket(size)) {
      // Packet was "lost" by the simulator
      return 1;
    }
  }

  // Actual send
  ssize_t bytes_sent = send(socket_fd_, data, size, 0);
  if (bytes_sent < 0) {
    LOG(ERROR) << "[NetworkSender] send() failed: " << strerror(errno);
    return -1;
  }

  return 0;
}
