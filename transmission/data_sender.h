#ifndef TRANSMISSION_DATA_SENDER_H
#define TRANSMISSION_DATA_SENDER_H

#include <cstdint>
#include <string>
#include "codec/encoder.h"
#include "config/config.h"

// Data type enumeration
enum class FeedbackType {
  ReceiveACK,
};

// DataSender class for sending encoded video data over UDP
// Splits large NAL units into smaller packets for transmission
class DataSender {
 public:
  DataSender();
  ~DataSender();

  int Initialize(const std::string& dest_ip, int dest_port, size_t max_packet_size = kDefaultMaxPacketSize);

  // Send frame data (encoded video frames)
  // Automatically splits large data into multiple packets with FramePacketHeader
  int SendFrame(const EncodedData* encoded_data);

  // Send feedback data
  int SendFeedback(uint16_t frame_sequence, uint8_t packet_index, FeedbackType feedback_type);

  void Close();

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

#endif  // TRANSMISSION_DATA_SENDER_H

