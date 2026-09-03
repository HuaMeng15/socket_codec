#include <gtest/gtest.h>
#include <cstring>
#include <memory>
#include <vector>

#include "transmission/data_sender.h"
#include "transmission/packet_header.h"
#include "codec/encoder.h"

class DataSenderPacketCountTest : public ::testing::Test {
 protected:
  DataSender sender;
  int reported_count_ = 0;

  void SetUp() override {
    sender.InitializeForTesting(1460);
    sender.SetPacketsSentCallback([this](int count) {
      reported_count_ = count;
    });
  }

  void TearDown() override {
    sender.Close();
  }

  // Helper: build EncodedData with specified NAL sizes
  std::unique_ptr<EncodedData> MakeFrame(uint16_t seq,
                                          const std::vector<size_t>& nal_sizes) {
    auto data = std::make_unique<EncodedData>();
    data->sequence_number = seq;
    for (size_t s : nal_sizes) {
      auto* buf = new uint8_t[s];
      memset(buf, 0xAB, s);
      data->AddData(buf, s);
    }
    return data;
  }
};

TEST_F(DataSenderPacketCountTest, SingleNalExactlyOnePayload) {
  // max_packet_size = 1460, header = 8 bytes, max_payload = 1452
  // NAL of exactly 1452 bytes → 1 packet
  auto frame = MakeFrame(1, {1452});
  sender.SendFrame(frame.get());
  EXPECT_EQ(reported_count_, 1);
}

TEST_F(DataSenderPacketCountTest, SingleNalOneByteBeyondPayload) {
  // NAL of 1453 bytes → ceil(1453/1452) = 2 packets
  auto frame = MakeFrame(2, {1453});
  sender.SendFrame(frame.get());
  EXPECT_EQ(reported_count_, 2);
}

TEST_F(DataSenderPacketCountTest, SingleNalSmallerThanPayload) {
  // NAL of 500 bytes → 1 packet
  auto frame = MakeFrame(3, {500});
  sender.SendFrame(frame.get());
  EXPECT_EQ(reported_count_, 1);
}

TEST_F(DataSenderPacketCountTest, MultipleNals) {
  // NAL 1: 1452 → 1 packet
  // NAL 2: 2904 → 2 packets (exactly 2x payload)
  // NAL 3: 100  → 1 packet
  // Total: 4
  auto frame = MakeFrame(4, {1452, 2904, 100});
  sender.SendFrame(frame.get());
  EXPECT_EQ(reported_count_, 4);
}

TEST_F(DataSenderPacketCountTest, LargeNalManySplits) {
  // NAL of 10000 bytes, max_payload = 1452
  // ceil(10000/1452) = 7 packets
  auto frame = MakeFrame(5, {10000});
  sender.SendFrame(frame.get());
  EXPECT_EQ(reported_count_, 7);
}

TEST_F(DataSenderPacketCountTest, SupportsMoreThan255PacketsPerFrame) {
  constexpr size_t kPayloadBytes = 1460 - sizeof(FramePacketHeader);
  auto frame = MakeFrame(6, {kPayloadBytes * 277});
  EXPECT_EQ(sender.SendFrame(frame.get()), 0);
  EXPECT_EQ(reported_count_, 277);
}
