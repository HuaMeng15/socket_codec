#include <gtest/gtest.h>

#include "transmission/alr_detector.h"

// The detector accrues budget at 0.65x the estimate and enters ALR when unspent
// budget exceeds 80% of a 500ms window, leaving when it drops below 50%. All
// timestamps are supplied by the caller, so these tests drive an explicit clock.

TEST(AlrDetectorTest, GreedySenderNeverEntersAlr) {
  // A source that fills the estimate (1000 kbps = 125 bytes/ms) sends MORE than
  // the 0.65x accrual, so the budget never builds up -> never ALR. This is the
  // greedy-encoder case that must stay out of ALR (no periodic probing).
  AlrDetector d;
  d.SetEstimatedBitrate(1000);
  int64_t t = 0;
  d.OnBytesSent(t, 0);  // prime last_send_time
  for (int i = 0; i < 20; ++i) {
    t += 100;
    d.OnBytesSent(t, 12500);  // 125 bytes/ms == full estimate rate
    EXPECT_FALSE(d.InAlr());
  }
}

TEST(AlrDetectorTest, UnderProducingEntersAlr) {
  // A source sending well below the estimate builds up budget and enters ALR.
  AlrDetector d;
  d.SetEstimatedBitrate(1000);
  d.OnBytesSent(0, 0);       // prime
  EXPECT_FALSE(d.InAlr());
  d.OnBytesSent(500, 0);     // 500ms sending nothing -> budget fills the window
  EXPECT_TRUE(d.InAlr());
}

TEST(AlrDetectorTest, RefillingLeavesAlr) {
  AlrDetector d;
  d.SetEstimatedBitrate(1000);
  d.OnBytesSent(0, 0);
  d.OnBytesSent(500, 0);
  ASSERT_TRUE(d.InAlr());

  // A large burst drains the budget below the stop threshold -> leaves ALR.
  d.OnBytesSent(600, 100000);
  EXPECT_FALSE(d.InAlr());
}

TEST(AlrDetectorTest, NoEstimateNoAlr) {
  // Before any estimate is set, the detector must not claim ALR (no divide by
  // zero, no spurious probing at startup).
  AlrDetector d;
  d.OnBytesSent(0, 0);
  d.OnBytesSent(1000, 0);
  EXPECT_FALSE(d.InAlr());
}
