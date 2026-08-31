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
 * - Frame-tick mechanism: blocks caller until the next frame boundary.
 *   Boundaries remain anchored to the clock epoch, so file-read and encoding
 *   overhead do not silently lower the configured frame rate.
 * - Slice-deadline calculation: given N slices per frame, returns the
 *   timestamp by which slice K should be complete
 *
 * Threading model:
 * - GetCurrentTimeUs(), GetSliceDeadline(), GetFrameIntervalUs(),
 *   GetCurrentFrameIndex(): safe to call from any thread concurrently.
 * - WaitForNextFrameTick(): single-consumer only (the capture/encode loop).
 *   Multiple waiters would produce duplicate frame indexes.
 * - MarkFrameReadComplete(): called by that same loop after a successful read.
 * - Start()/Stop(): call from owner thread.
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
   * Record that the current frame's raw input read has completed. Kept as a
   * pipeline progress marker; frame and slice scheduling remain epoch-based.
   */
  void MarkFrameReadComplete();

  /**
   * Get the absolute timestamp (us since Start) by which the given slice
   * should be encoded and sent. Spreads slices evenly across the frame interval.
   *
   * For frame N with S slices, slice K's deadline is:
   *   N * frame_interval + (K+1) * (frame_interval / S)
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
  std::chrono::steady_clock::time_point last_tick_time_;
  std::chrono::steady_clock::time_point last_frame_read_done_time_;
  std::atomic<bool> running_;
  std::atomic<int> frame_index_;
  bool has_frame_read_done_;
  int read_done_frame_index_;
  int64_t read_done_us_;

  mutable std::mutex mutex_;
  std::condition_variable tick_cv_;
};

#endif  // TOOLS_CLOCK_THREAD_H
