#include "tools/clock_thread.h"

ClockThread::ClockThread()
    : fps_(30),
      slice_count_(1),
      frame_interval_us_(33333),
      running_(false),
      frame_index_(-1),
      has_frame_read_done_(false),
      read_done_frame_index_(-1),
      read_done_us_(0) {
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
  auto now = std::chrono::steady_clock::now();
  std::lock_guard<std::mutex> lock(mutex_);
  epoch_ = now;
  last_tick_time_ = now;
  last_frame_read_done_time_ = now;
  frame_index_.store(-1);
  has_frame_read_done_ = false;
  read_done_frame_index_ = -1;
  read_done_us_ = 0;
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

  std::unique_lock<std::mutex> lock(mutex_);
  int next_frame = frame_index_.load() + 1;
  auto target_time = epoch_;
  if (next_frame > 0) {
    const auto base_time =
        has_frame_read_done_ ? last_frame_read_done_time_ : last_tick_time_;
    target_time = base_time + std::chrono::microseconds(frame_interval_us_);
  }

  tick_cv_.wait_until(lock, target_time, [this]() {
    return !running_.load();
  });

  if (!running_.load()) {
    return -1;
  }

  last_tick_time_ = std::chrono::steady_clock::now();
  frame_index_.store(next_frame);
  has_frame_read_done_ = false;
  return next_frame;
}

void ClockThread::MarkFrameReadComplete() {
  auto now = std::chrono::steady_clock::now();
  std::lock_guard<std::mutex> lock(mutex_);
  last_frame_read_done_time_ = now;
  read_done_us_ =
      std::chrono::duration_cast<std::chrono::microseconds>(now - epoch_).count();
  read_done_frame_index_ = frame_index_.load();
  has_frame_read_done_ = true;
}

int64_t ClockThread::GetSliceDeadline(int frame_index, int slice_index) const {
  std::lock_guard<std::mutex> lock(mutex_);
  int64_t frame_start = static_cast<int64_t>(frame_index) * frame_interval_us_;
  if (read_done_frame_index_ == frame_index) {
    frame_start = read_done_us_;
  }
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
