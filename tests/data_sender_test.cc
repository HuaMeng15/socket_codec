#include <gtest/gtest.h>
#include <cstring>
#include <memory>
#include <vector>

#include "transmission/data_sender.h"
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
  // max_packet_size = 1460, header = 6 bytes, max_payload = 1454
  // NAL of exactly 1454 bytes → 1 packet
  auto frame = MakeFrame(1, {1454});
  sender.SendFrame(frame.get());
  EXPECT_EQ(reported_count_, 1);
}

TEST_F(DataSenderPacketCountTest, SingleNalOneByteBeyondPayload) {
  // NAL of 1455 bytes → ceil(1455/1454) = 2 packets
  auto frame = MakeFrame(2, {1455});
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
  // NAL 1: 1454 → 1 packet
  // NAL 2: 2908 → 2 packets (exactly 2x payload)
  // NAL 3: 100  → 1 packet
  // Total: 4
  auto frame = MakeFrame(4, {1454, 2908, 100});
  sender.SendFrame(frame.get());
  EXPECT_EQ(reported_count_, 4);
}

TEST_F(DataSenderPacketCountTest, LargeNalManySplits) {
  // NAL of 10000 bytes, max_payload = 1454
  // ceil(10000/1454) = 7 packets
  auto frame = MakeFrame(5, {10000});
  sender.SendFrame(frame.get());
  EXPECT_EQ(reported_count_, 7);
}
