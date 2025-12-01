#ifndef TRANSMISSION_MESSAGE_RECEIVER_H
#define TRANSMISSION_MESSAGE_RECEIVER_H

#include <atomic>
#include <cstdint>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "transmission/message_handler.h"

// MessageReceiver class for receiving encoded video data over UDP
// Reassembles packets into complete frames and writes to file/handler
class MessageReceiver {
 public:
  MessageReceiver();
  ~MessageReceiver();

  // Initialize the receiver with listening port
  // Returns 0 on success, negative value on error
  int Initialize(int listen_port);

  // Set message handler for processing received frames
  void SetMessageHandler(MessageHandler* handler);

  // Run the receiver loop (blocks until stopped)
  // Continuously receives packets and writes complete frames to file/decoder
  void Run();

  // Stop the receiver
  void Stop();

  // Check if receiver is stopped
  bool IsStopped() const;

  // Close the socket and file
  void Close();

  // Check if receiver is initialized
  bool IsInitialized() const;

 private:
  // Receive a single packet
  int ReceivePacket(uint8_t* buffer, size_t buffer_size, ssize_t& bytes_received);

  int socket_fd_;
  int listen_port_;
  bool initialized_;
  std::atomic<bool> stop_requested_;

  // Message handler for processing received packets
  MessageHandler* message_handler_;
};

#endif  // TRANSMISSION_MESSAGE_RECEIVER_H
