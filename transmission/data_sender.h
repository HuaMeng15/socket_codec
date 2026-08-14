#ifndef TRANSMISSION_DATA_SENDER_H
#define TRANSMISSION_DATA_SENDER_H

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include "codec/encoder.h"
#include "config/config.h"
#include "network_sender.h"

class NetworkSimulator;
class PacketSendTimeStore;
class Pacer;

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

  // Test-only: initialize packetization and callbacks without opening a socket.
  // SendFrame/SendFrameFragment will exercise the same framing logic but skip
  // the actual transport send.
  void InitializeForTesting(size_t max_packet_size = kDefaultMaxPacketSize);

  // Send frame data (encoded video frames)
  // Automatically splits large data into multiple packets with FramePacketHeader
  int SendFrame(const EncodedData* encoded_data);

  // Count packets SendFrame/SendFrameFragment would emit for this encoded data.
  size_t CountFramePackets(const EncodedData* encoded_data) const;

  // Send part of a frame while preserving packet indices across fragments.
  // total_packets_for_header == 0 means "frame still open"; the final fragment
  // must pass the actual total packet count for the whole frame.
  int SendFrameFragment(const EncodedData* encoded_data,
                        uint8_t first_packet_index,
                        uint8_t total_packets_for_header,
                        uint8_t* packets_sent);

  // Send feedback data (legacy per-packet ACK)
  int SendFeedback(uint16_t frame_sequence, uint8_t packet_index, FeedbackType feedback_type);

  // Send raw feedback bytes (for TWCC-style transport feedback / loss reports)
  int SendRawFeedback(const uint8_t* data, size_t size);

  void Close();

  bool IsInitialized() const;

  /** Optional: set to record send time for each (frame, packet) for latency stats. */
  void SetSendTimeStore(PacketSendTimeStore* store) { send_time_store_ = store; }
  /** Optional: set to pace packet sends (spread over time by bitrate). Wires
   *  the pacer's send/record callbacks to this sender and starts its thread. */
  void SetPacer(Pacer* pacer);
  /** Optional: attach a network simulator for bandwidth/delay/loss. */
  void SetSimulator(NetworkSimulator* simulator);
  /** Optional: callback invoked with packet count after each SendFrame. */
  using PacketsSentCallback = std::function<void(int packet_count)>;
  void SetPacketsSentCallback(PacketsSentCallback cb) { packets_sent_cb_ = std::move(cb); }

 private:
  // Send a single packet
  int SendPacket(const uint8_t* packet_data, size_t packet_size);

  int socket_fd_;
  std::string dest_ip_;
  int dest_port_;
  size_t max_packet_size_;
  bool initialized_;
  uint32_t packet_sequence_;
  NetworkSender network_sender_;
  PacketSendTimeStore* send_time_store_ = nullptr;
  Pacer* pacer_ = nullptr;
  bool dry_run_ = false;
  PacketsSentCallback packets_sent_cb_;
};

#endif  // TRANSMISSION_DATA_SENDER_H
