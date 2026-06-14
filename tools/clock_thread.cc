#include "tools/clock_thread.h"

ClockThread::ClockThread()
    : fps_(30),
      slice_count_(1),
      frame_interval_us_(33333),
      running_(false),
      frame_index_(-1) {
}

ClockThread::~ClockThread() {
  Stop();
}

void ClockThread::SetFps(int fps) {
  fps_ = fps > 0 ? fps : 30;
  frame_interval_us_ = 1000000 / fps_;
}

void ClockThread::SetSliceCount(int slice_count) {
  slice_count_ = slice_count > 0 ? slice_count : 1;
}

void ClockThread::Start() {
  epoch_ = std::chrono::steady_clock::now();
  frame_index_.store(-1);
  running_.store(true);
}

void ClockThread::Stop() {
  running_.store(false);
  // Wake any threads blocked in WaitForNextFrameTick
  tick_cv_.notify_all();
}

int64_t ClockThread::GetCurrentTimeUs() const {
  auto now = std::chrono::steady_clock::now();
  return std::chrono::duration_cast<std::chrono::microseconds>(now - epoch_).count();
}

int ClockThread::WaitForNextFrameTick() {
  if (!running_.load()) {
    return -1;
  }

  int next_frame = frame_index_.load() + 1;
  int64_t target_us = next_frame * frame_interval_us_;

  // Sleep until the target time
  auto target_time = epoch_ + std::chrono::microseconds(target_us);

  std::unique_lock<std::mutex> lock(mutex_);
  tick_cv_.wait_until(lock, target_time, [this]() {
    return !running_.load();
  });

  if (!running_.load()) {
    return -1;
  }

  frame_index_.store(next_frame);
  return next_frame;
}

int64_t ClockThread::GetSliceDeadline(int frame_index, int slice_index) const {
  int64_t frame_start = static_cast<int64_t>(frame_index) * frame_interval_us_;
  int64_t slice_deadline = frame_start +
      (static_cast<int64_t>(slice_index) + 1) * (frame_interval_us_ / slice_count_);
  return slice_deadline;
}

int64_t ClockThread::GetFrameIntervalUs() const {
  return frame_interval_us_;
}

int ClockThread::GetCurrentFrameIndex() const {
  return frame_index_.load();
}
