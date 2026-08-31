#include <gtest/gtest.h>
#include <chrono>
#include <thread>
#include <vector>

#include "tools/clock_thread.h"

class ClockThreadTest : public ::testing::Test {
 protected:
  ClockThread clock;

  void SetUp() override {
    clock.SetFps(30);
    clock.SetSliceCount(10);
  }
};

TEST_F(ClockThreadTest, GetCurrentTimeUsIncreases) {
  clock.Start();
  int64_t t1 = clock.GetCurrentTimeUs();
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  int64_t t2 = clock.GetCurrentTimeUs();
  clock.Stop();

  EXPECT_GT(t2, t1);
  // At least 4ms should have elapsed (allowing some scheduling slack)
  EXPECT_GE(t2 - t1, 4000);
}

TEST_F(ClockThreadTest, FrameIntervalMatchesFps) {
  clock.SetFps(60);
  // 60fps => 16666us per frame
  EXPECT_EQ(clock.GetFrameIntervalUs(), 16666);

  clock.SetFps(30);
  // 30fps => 33333us per frame
  EXPECT_EQ(clock.GetFrameIntervalUs(), 33333);
}

TEST_F(ClockThreadTest, WaitForNextFrameTickReturnsSequentialIndices) {
  clock.SetFps(120);  // short interval for fast test
  clock.Start();

  int idx0 = clock.WaitForNextFrameTick();
  int idx1 = clock.WaitForNextFrameTick();
  int idx2 = clock.WaitForNextFrameTick();
  clock.Stop();

  EXPECT_EQ(idx0, 0);
  EXPECT_EQ(idx1, 1);
  EXPECT_EQ(idx2, 2);
}

TEST_F(ClockThreadTest, WaitForNextFrameTickTimingAccuracy) {
  clock.SetFps(60);  // 16.6ms intervals
  clock.Start();

  auto start = std::chrono::steady_clock::now();
  clock.WaitForNextFrameTick();  // frame 0
  clock.WaitForNextFrameTick();  // frame 1
  clock.WaitForNextFrameTick();  // frame 2
  auto end = std::chrono::steady_clock::now();

  clock.Stop();

  auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
  // 3 frames at 60fps = ~50ms. Allow 30-70ms tolerance for scheduling.
  EXPECT_GE(elapsed_ms, 30);
  EXPECT_LE(elapsed_ms, 70);
}

TEST_F(ClockThreadTest, ReadCompletionMaintainsIntervalAfterStall) {
  clock.SetFps(100);  // 10ms intervals
  clock.Start();

  EXPECT_EQ(clock.WaitForNextFrameTick(), 0);

  // Simulate a late first frame. The next read may start immediately because
  // the prior tick is already late, but its reported completion must remain a
  // full frame interval after the previous completion.
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  clock.MarkFrameReadComplete();

  EXPECT_EQ(clock.WaitForNextFrameTick(), 1);
  auto start = std::chrono::steady_clock::now();
  clock.MarkFrameReadComplete();
  auto end = std::chrono::steady_clock::now();
  clock.Stop();

  auto waited_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
  EXPECT_GE(waited_ms, 8);
}

TEST_F(ClockThreadTest, StopUnblocksWait) {
  clock.SetFps(1);  // 1 second per frame — frame 1 would block ~1s without stop
  clock.Start();

  // Frame 0 fires immediately (t=0)
  int idx0 = clock.WaitForNextFrameTick();
  EXPECT_EQ(idx0, 0);

  // Now wait for frame 1 (t=1s) but stop after 50ms
  std::thread stopper([this]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    clock.Stop();
  });

  int idx1 = clock.WaitForNextFrameTick();
  // Should return -1 (stopped) well before 1s
  EXPECT_EQ(idx1, -1);
  stopper.join();
}

TEST_F(ClockThreadTest, SliceDeadlinesSpreadEvenly) {
  clock.SetFps(30);        // 33333us per frame
  clock.SetSliceCount(10); // 10 slices

  // Frame 0, slice 0 deadline: (0+1) * 33333/10 = 3333
  EXPECT_EQ(clock.GetSliceDeadline(0, 0), 3333);
  // Frame 0, slice 9 deadline: (9+1) * 33333/10 = 33330 (integer division)
  EXPECT_EQ(clock.GetSliceDeadline(0, 9), 33330);
  // Frame 1, slice 0: frame_start(33333) + 3333 = 36666
  EXPECT_EQ(clock.GetSliceDeadline(1, 0), 36666);
}

TEST_F(ClockThreadTest, MultipleThreadsCanReadTime) {
  clock.Start();
  std::vector<int64_t> times(4);

  std::vector<std::thread> threads;
  for (int i = 0; i < 4; i++) {
    threads.emplace_back([this, &times, i]() {
      std::this_thread::sleep_for(std::chrono::milliseconds(i * 2));
      times[i] = clock.GetCurrentTimeUs();
    });
  }
  for (auto& t : threads) t.join();
  clock.Stop();

  // Times should be monotonically increasing (with thread ordering)
  for (int i = 1; i < 4; i++) {
    EXPECT_GE(times[i], times[i - 1]);
  }
}
