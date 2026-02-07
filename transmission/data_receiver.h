#ifndef TRANSMISSION_DATA_RECEIVER_H
#define TRANSMISSION_DATA_RECEIVER_H

#include <atomic>
#include <cstdint>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "transmission/message_handler.h"

// DataReceiver class for receiving encoded video data over UDP
// Reassembles packets into complete frames and writes to file/handler
class DataReceiver {
 public:
  DataReceiver();
  ~DataReceiver();

  int Initialize(int listen_port);

  void SetMessageHandler(MessageHandler* handler);

  // Run the receiver loop (blocks until stopped)
  // Continuously receives packets and writes complete frames to file/decoder
  void Run();

  void Stop();

  bool IsStopped() const;

  void Close();

  bool IsInitialized() const;

  bool GetLastSenderInfo(std::string& sender_ip, int& sender_port) const;

 private:
  // Receive a single packet
  int ReceivePacket(uint8_t* buffer, size_t buffer_size, ssize_t& bytes_received);

  int socket_fd_;
  int listen_port_;
  bool initialized_;
  std::atomic<bool> stop_requested_;

  MessageHandler* message_handler_;

  // Last sender information (for feedback)
  mutable std::string last_sender_ip_;
  mutable int last_sender_port_;
  mutable bool has_sender_info_;
};

#endif  // TRANSMISSION_DATA_RECEIVER_H

