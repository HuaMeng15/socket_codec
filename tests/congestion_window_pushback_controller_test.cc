#include <gtest/gtest.h>

#include <memory>

#include "transmission/congestion_window_pushback_controller.h"

// Ported from WebRTC's congestion_window_pushback_controller_unittest.cc.
// Default min pushback target = 30000 bps (WebRTC field-trial default).

namespace {
constexpr uint32_t kMinPushbackBps = 30000;

std::unique_ptr<CongestionWindowPushbackController> MakeController() {
  return std::make_unique<CongestionWindowPushbackController>(kMinPushbackBps);
}
}  // namespace

TEST(CwndPushbackTest, FullCongestionWindow) {
  auto c = MakeController();
  c->SetDataWindow(50000);
  c->UpdateOutstandingData(0);
  EXPECT_EQ(80000u, c->UpdateTargetBitrate(80000, 0));

  c->UpdateOutstandingData(100000);
  c->SetDataWindow(50000);  // fill_ratio = 2.0 (> 1.5) -> x0.9

  uint32_t bitrate_bps = c->UpdateTargetBitrate(80000, 100);
  EXPECT_EQ(72000u, bitrate_bps);

  // More callbacks at the same time do not apply more reductions.
  bitrate_bps = c->UpdateTargetBitrate(80000, 100);
  EXPECT_EQ(72000u, bitrate_bps);

  bitrate_bps = c->UpdateTargetBitrate(80000, 200);
  EXPECT_EQ(64800u, bitrate_bps);
}

TEST(CwndPushbackTest, NormalCongestionWindow) {
  auto c = MakeController();
  c->UpdateOutstandingData(199999);
  c->SetDataWindow(200000);  // fill_ratio ~0.9999 (<1.0, >0.1) -> x1.05 cap 1.0

  uint32_t bitrate_bps = 80000;
  bitrate_bps = c->UpdateTargetBitrate(bitrate_bps, 0);
  EXPECT_EQ(80000u, bitrate_bps);
}

TEST(CwndPushbackTest, LowBitrate) {
  auto c = MakeController();
  c->SetDataWindow(50000);
  c->UpdateOutstandingData(0);
  EXPECT_EQ(35000u, c->UpdateTargetBitrate(35000, 0));

  c->UpdateOutstandingData(100000);
  c->SetDataWindow(50000);  // fill_ratio = 2.0 -> x0.9

  uint32_t bitrate_bps = 35000;
  bitrate_bps = c->UpdateTargetBitrate(bitrate_bps, 100);
  EXPECT_EQ(static_cast<uint32_t>(35000 * 0.9), bitrate_bps);

  c->SetDataWindow(20000);  // fill_ratio = 5.0 -> ratio 0.81, 28350 < floor
  bitrate_bps = c->UpdateTargetBitrate(35000, 200);
  EXPECT_EQ(30000u, bitrate_bps);  // min(35000, 30000 floor)
}

TEST(CwndPushbackTest, NoPushbackOnDataWindowUnset) {
  auto c = MakeController();
  c->UpdateOutstandingData(100000000);  // huge in-flight
  // No SetDataWindow -> window unset -> pass-through unchanged.
  uint32_t bitrate_bps = 80000;
  bitrate_bps = c->UpdateTargetBitrate(bitrate_bps, 0);
  EXPECT_EQ(80000u, bitrate_bps);
}

TEST(CwndPushbackTest, DrainReleasesRatioToFull) {
  auto c = MakeController();
  c->SetDataWindow(50000);
  c->UpdateOutstandingData(0);
  uint32_t b = c->UpdateTargetBitrate(800000, 0);

  // Decay the ratio over real elapsed time while the window is overfull.
  c->UpdateOutstandingData(100000);  // fill 2.0
  for (int i = 1; i <= 5; ++i) {
    b = c->UpdateTargetBitrate(800000, i * 100);
  }
  EXPECT_LT(c->encoding_rate_ratio(), 0.7);

  // Backlog drains below 10% of window -> ratio snaps back to 1.0.
  c->UpdateOutstandingData(1000);  // fill 0.02 (< 0.1)
  b = c->UpdateTargetBitrate(500000, 501);
  EXPECT_DOUBLE_EQ(1.0, c->encoding_rate_ratio());
  EXPECT_EQ(500000u, b);
}

TEST(CwndPushbackTest, DecayIsIndependentOfFeedbackFrequency) {
  auto frequent = MakeController();
  auto sparse = MakeController();
  for (auto* c : {frequent.get(), sparse.get()}) {
    c->SetDataWindow(50000);
    c->UpdateOutstandingData(0);
    EXPECT_EQ(800000u, c->UpdateTargetBitrate(800000, 0));
    c->UpdateOutstandingData(100000);
  }

  uint32_t frequent_rate = 800000;
  for (int64_t now_ms = 10; now_ms <= 300; now_ms += 10) {
    frequent_rate = frequent->UpdateTargetBitrate(800000, now_ms);
  }
  uint32_t sparse_rate = sparse->UpdateTargetBitrate(800000, 300);

  EXPECT_NEAR(frequent->encoding_rate_ratio(),
              sparse->encoding_rate_ratio(), 1e-9);
  EXPECT_NEAR(frequent_rate, sparse_rate, 1u);
  EXPECT_EQ(static_cast<uint32_t>(800000 * 0.9 * 0.9 * 0.9),
            sparse_rate);
}
