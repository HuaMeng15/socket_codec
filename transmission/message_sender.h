#ifndef TRANSMISSION_MESSAGE_SENDER_H
#define TRANSMISSION_MESSAGE_SENDER_H

#include <cstdint>
#include <string>

// MessageSender class for sending encoded video data over UDP
// Splits large NAL units into smaller packets for transmission
class MessageSender {
 public:
  MessageSender();
  ~MessageSender();

  // Initialize the sender with destination IP and port
  // Returns 0 on success, negative value on error
  int Initialize(const std::string& dest_ip, int dest_port, size_t max_packet_size = 1400);

  // Send encoded data (NAL unit)
  // Automatically splits large data into multiple packets
  // Returns 0 on success, negative value on error
  int SendData(const uint8_t* data, size_t data_size, uint32_t frame_sequence);

  // Send raw data without packet header (for feedback, etc.)
  // Returns 0 on success, negative value on error
  int SendRaw(const uint8_t* data, size_t data_size);

  // Close the socket
  void Close();

  // Check if sender is initialized
  bool IsInitialized() const;

 private:
  // Send a single packet
  int SendPacket(const uint8_t* packet_data, size_t packet_size);

  int socket_fd_;
  std::string dest_ip_;
  int dest_port_;
  size_t max_packet_size_;
  bool initialized_;
  uint32_t packet_sequence_;
};

#endif  // TRANSMISSION_MESSAGE_SENDER_H

