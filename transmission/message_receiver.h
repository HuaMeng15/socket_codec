#ifndef TRANSMISSION_MESSAGE_RECEIVER_H
#define TRANSMISSION_MESSAGE_RECEIVER_H

#include <atomic>
#include <cstdint>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

// MessageReceiver class for receiving encoded video data over UDP
// Reassembles packets into complete frames and writes to file
class MessageReceiver {
 public:
  MessageReceiver();
  ~MessageReceiver();

  // Initialize the receiver with listening port and output file
  // Returns 0 on success, negative value on error
  int Initialize(int listen_port, const std::string& output_file);

  // Run the receiver loop (blocks until stopped)
  // Continuously receives packets and writes complete frames to file
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

  // Process a received packet
  void ProcessPacket(const uint8_t* packet_data, size_t packet_size);

  // Write complete frame to file
  void WriteFrame(uint32_t frame_sequence, const std::vector<uint8_t>& frame_data);

  // Packet header structure (matches MessageSender)
  struct PacketHeader {
    uint32_t frame_sequence;  // Frame sequence number
    uint16_t packet_index;    // Packet index within frame (0-based)
    uint16_t total_packets;   // Total number of packets for this frame
    uint32_t payload_size;    // Size of payload in this packet
  };

  // Frame assembly state
  struct FrameAssembly {
    std::vector<std::vector<uint8_t>> packets;  // Packets for this frame
    uint16_t total_packets;                     // Expected total packets
    uint32_t received_packets;                  // Number of packets received
    bool complete;                              // Frame is complete
  };

  int socket_fd_;
  int listen_port_;
  std::string output_file_;
  std::ofstream output_stream_;
  bool initialized_;
  std::atomic<bool> stop_requested_;

  // Frame assembly map: frame_sequence -> FrameAssembly
  std::map<uint32_t, FrameAssembly> frame_assemblies_;
  uint32_t last_completed_frame_;
};

#endif  // TRANSMISSION_MESSAGE_RECEIVER_H
