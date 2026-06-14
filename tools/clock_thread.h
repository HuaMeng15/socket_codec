#ifndef TOOLS_CLOCK_THREAD_H
#define TOOLS_CLOCK_THREAD_H

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <condition_variable>

/**
 * ClockThread: single timing source for all threads in the pipeline.
 *
 * Provides:
 * - Monotonic microsecond timestamps (GetCurrentTimeUs)
 * - Frame-tick mechanism: blocks callers until the next frame boundary
 * - Slice-deadline calculation: given N slices per frame, returns the
 *   timestamp by which slice K should be complete
 *
 * Thread-safe: multiple threads can call any method concurrently.
 * Start() begins the internal tick loop; Stop() ends it.
 */
class ClockThread {
 public:
  ClockThread();
  ~ClockThread();

  /** Configure frame rate. Must be called before Start(). */
  void SetFps(int fps);

  /** Configure number of slices per frame (for deadline calculation). */
  void SetSliceCount(int slice_count);

  /** Start the clock. Records epoch and begins frame tick counting. */
  void Start();

  /** Stop the clock. Wakes any blocked WaitForNextFrameTick() callers. */
  void Stop();

  /** Get monotonic time in microseconds since clock Start(). */
  int64_t GetCurrentTimeUs() const;

  /**
   * Block until the next frame tick. Returns the frame index (0-based).
   * Returns -1 if the clock has been stopped.
   */
  int WaitForNextFrameTick();

  /**
   * Get the absolute timestamp (us since Start) by which the given slice
   * should be encoded and sent. Spreads slices evenly across the frame interval.
   *
   * For frame N with S slices, slice K's deadline is:
   *   frame_start + (K+1) * (frame_interval / S)
   */
  int64_t GetSliceDeadline(int frame_index, int slice_index) const;

  /** Get the frame interval in microseconds. */
  int64_t GetFrameIntervalUs() const;

  /** Get current frame index (how many ticks have elapsed). */
  int GetCurrentFrameIndex() const;

 private:
  int fps_;
  int slice_count_;
  int64_t frame_interval_us_;

  std::chrono::steady_clock::time_point epoch_;
  std::atomic<bool> running_;
  std::atomic<int> frame_index_;

  mutable std::mutex mutex_;
  std::condition_variable tick_cv_;
};

#endif  // TOOLS_CLOCK_THREAD_H
