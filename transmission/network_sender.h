#ifndef TRANSMISSION_NETWORK_SENDER_H
#define TRANSMISSION_NETWORK_SENDER_H

#include <cstddef>
#include <cstdint>

class NetworkSimulator;

/**
 * NetworkSender: wraps the raw socket send() call with an optional
 * NetworkSimulator. When the simulator is attached, packets go through
 * bandwidth/delay/loss processing before hitting the wire. When not
 * attached, packets are sent directly (zero overhead).
 *
 * This class does NOT own the socket. It operates on an existing fd.
 */
class NetworkSender {
 public:
  NetworkSender();
  ~NetworkSender() = default;

  /** Set the socket fd to send on. Does not own it. */
  void SetSocketFd(int fd);

  /** Attach a simulator (nullptr to disable). Not owned. */
  void SetSimulator(NetworkSimulator* simulator);

  /**
   * Send a packet. If simulator is attached, applies its processing first.
   * Returns 0 on success, -1 on error, 1 if packet was "lost" by simulator.
   */
  int Send(const uint8_t* data, size_t size);

 private:
  int socket_fd_;
  NetworkSimulator* simulator_;
};

#endif  // TRANSMISSION_NETWORK_SENDER_H
