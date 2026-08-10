#include <gtest/gtest.h>
#include <arpa/inet.h>
#include <chrono>
#include <cstring>
#include <thread>

#include "transmission/transport_feedback.h"
#include "transmission/feedback_collector.h"
#include "transmission/feedback_handler.h"

// Test TransportFeedback wire format serialization/deserialization
TEST(TransportFeedbackTest, SerializeDeserializeRoundtrip) {
  // Build a transport feedback message manually (as receiver would)
  const int record_count = 3;
  size_t msg_size = sizeof(FeedbackMessageHeader) +
                    record_count * sizeof(PacketArrivalRecord);
  std::vector<uint8_t> buffer(msg_size);

  auto* header = reinterpret_cast<FeedbackMessageHeader*>(buffer.data());
  header->message_type = static_cast<uint8_t>(FeedbackMessageType::TransportFeedback);
  header->reserved = 0;
  header->record_count = htons(record_count);
  header->sender_ssrc = 0;

  auto* records = reinterpret_cast<PacketArrivalRecord*>(
      buffer.data() + sizeof(FeedbackMessageHeader));
  // {frame_seq, packet_index, padding, arrival_time_us, recv_size, reserved2}
  records[0] = {htons(1), 0, 0, htonl(0),  htons(1200), 0};
  records[1] = {htons(1), 1, 0, htonl(5),  htons(800),  0};
  records[2] = {htons(2), 0, 0, htonl(10), htons(1454), 0};

  // Parse on sender side via FeedbackHandler
  FeedbackHandler handler;
  handler.Initialize();

  TransportFeedback received_fb;
  handler.SetTransportFeedbackCallback([&](const TransportFeedback& fb) {
    received_fb = fb;
  });

  int ret = handler.HandlePacketMessage(buffer.data(), buffer.size());
  EXPECT_EQ(ret, 0);
  ASSERT_EQ(received_fb.packets.size(), 3u);
  EXPECT_EQ(received_fb.packets[0].frame_sequence, 1);
  EXPECT_EQ(received_fb.packets[0].packet_index, 0);
  EXPECT_EQ(received_fb.packets[0].recv_size, 1200);
  EXPECT_EQ(received_fb.packets[1].frame_sequence, 1);
  EXPECT_EQ(received_fb.packets[1].packet_index, 1);
  EXPECT_EQ(received_fb.packets[1].recv_size, 800);
  EXPECT_EQ(received_fb.packets[2].frame_sequence, 2);
  EXPECT_EQ(received_fb.packets[2].packet_index, 0);
  EXPECT_EQ(received_fb.packets[2].recv_size, 1454);
}

TEST(TransportFeedbackTest, LossReportSerializeDeserialize) {
  const int record_count = 2;
  size_t msg_size = sizeof(FeedbackMessageHeader) +
                    record_count * sizeof(PacketLossRecord);
  std::vector<uint8_t> buffer(msg_size);

  auto* header = reinterpret_cast<FeedbackMessageHeader*>(buffer.data());
  header->message_type = static_cast<uint8_t>(FeedbackMessageType::LossReport);
  header->reserved = 0;
  header->record_count = htons(record_count);
  header->sender_ssrc = 0;

  auto* records = reinterpret_cast<PacketLossRecord*>(
      buffer.data() + sizeof(FeedbackMessageHeader));
  records[0] = {htons(5), 2, 0};
  records[1] = {htons(5), 7, 0};

  FeedbackHandler handler;
  handler.Initialize();

  LossReport received_report;
  handler.SetLossReportCallback([&](const LossReport& report) {
    received_report = report;
  });

  int ret = handler.HandlePacketMessage(buffer.data(), buffer.size());
  EXPECT_EQ(ret, 0);
  ASSERT_EQ(received_report.packets.size(), 2u);
  EXPECT_EQ(received_report.packets[0].frame_sequence, 5);
  EXPECT_EQ(received_report.packets[0].packet_index, 2);
  EXPECT_EQ(received_report.packets[1].frame_sequence, 5);
  EXPECT_EQ(received_report.packets[1].packet_index, 7);
}

TEST(TransportFeedbackTest, CountTriggerSendsAfterInterval) {
  // With the time trigger disabled, feedback is sent only when the packet
  // count reaches the interval — not before.
  FeedbackCollector collector;
  collector.SetFeedbackMaxIntervalMs(0);  // disable time trigger
  collector.SetFeedbackInterval(5);
  int sends = 0;
  collector.SetSendCallback([&](const uint8_t*, size_t) { sends++; });

  for (int i = 0; i < 4; i++) {
    collector.OnPacketReceived(1, static_cast<uint8_t>(i), 1200);
  }
  EXPECT_EQ(sends, 0);  // 4 < 5, no send yet
  collector.OnPacketReceived(1, 4, 1200);
  EXPECT_EQ(sends, 1);  // 5th packet hits the interval
}

TEST(TransportFeedbackTest, TimeTriggerSendsBeforeCountReached) {
  // At low bitrate the count interval is far from reached, but a packet aging
  // past the max interval must still flush — this is what bounds feedback
  // latency when the link is slow.
  FeedbackCollector collector;
  collector.SetFeedbackInterval(20);        // count trigger far off
  collector.SetFeedbackMaxIntervalMs(20);   // 20ms time bound
  int sends = 0;
  collector.SetSendCallback([&](const uint8_t*, size_t) { sends++; });

  collector.OnPacketReceived(1, 0, 1200);   // starts the clock
  EXPECT_EQ(sends, 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(30));  // exceed window
  collector.OnPacketReceived(1, 1, 1200);   // 2 pkts (<20) but oldest is >20ms
  EXPECT_EQ(sends, 1);  // time trigger fired despite count not reached
}

TEST(TransportFeedbackTest, LossDetection) {
  FeedbackCollector collector;

  // Frame 3 expected 5 packets, received 0,1,3,4 (missing 2)
  std::vector<bool> received_mask = {true, true, false, true, true};
  auto lost = collector.DetectLoss(3, 5, received_mask);

  ASSERT_EQ(lost.size(), 1u);
  EXPECT_EQ(lost[0].frame_sequence, 3);
  EXPECT_EQ(lost[0].packet_index, 2);
}

TEST(TransportFeedbackTest, LossDetectionMultipleMissing) {
  FeedbackCollector collector;

  // Frame 7 expected 10 packets, received only 0,1,5,6
  std::vector<bool> received_mask = {true, true, false, false, false,
                                      true, true, false, false, false};
  auto lost = collector.DetectLoss(7, 10, received_mask);

  ASSERT_EQ(lost.size(), 6u);
  EXPECT_EQ(lost[0].packet_index, 2);
  EXPECT_EQ(lost[1].packet_index, 3);
  EXPECT_EQ(lost[2].packet_index, 4);
  EXPECT_EQ(lost[3].packet_index, 7);
  EXPECT_EQ(lost[4].packet_index, 8);
  EXPECT_EQ(lost[5].packet_index, 9);
}

TEST(TransportFeedbackTest, FeedbackHandlerDistinguishesMessageTypes) {
  FeedbackHandler handler;
  handler.Initialize();

  int transport_count = 0;
  int loss_count = 0;

  handler.SetTransportFeedbackCallback([&](const TransportFeedback&) {
    transport_count++;
  });
  handler.SetLossReportCallback([&](const LossReport&) {
    loss_count++;
  });

  // Send a transport feedback
  {
    size_t msg_size = sizeof(FeedbackMessageHeader) + sizeof(PacketArrivalRecord);
    std::vector<uint8_t> buf(msg_size);
    auto* h = reinterpret_cast<FeedbackMessageHeader*>(buf.data());
    h->message_type = static_cast<uint8_t>(FeedbackMessageType::TransportFeedback);
    h->record_count = htons(1);
    auto* r = reinterpret_cast<PacketArrivalRecord*>(buf.data() + sizeof(FeedbackMessageHeader));
    r->frame_sequence = htons(1);
    r->packet_index = 0;
    r->arrival_time_us = htonl(0);
    handler.HandlePacketMessage(buf.data(), buf.size());
  }

  // Send a loss report
  {
    size_t msg_size = sizeof(FeedbackMessageHeader) + sizeof(PacketLossRecord);
    std::vector<uint8_t> buf(msg_size);
    auto* h = reinterpret_cast<FeedbackMessageHeader*>(buf.data());
    h->message_type = static_cast<uint8_t>(FeedbackMessageType::LossReport);
    h->record_count = htons(1);
    auto* r = reinterpret_cast<PacketLossRecord*>(buf.data() + sizeof(FeedbackMessageHeader));
    r->frame_sequence = htons(2);
    r->packet_index = 3;
    handler.HandlePacketMessage(buf.data(), buf.size());
  }

  EXPECT_EQ(transport_count, 1);
  EXPECT_EQ(loss_count, 1);
}
