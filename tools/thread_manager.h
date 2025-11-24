#ifndef TOOLS_THREAD_MANAGER_H
#define TOOLS_THREAD_MANAGER_H

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

// RAII wrapper for std::thread with automatic cleanup
class Thread {
 public:
  // Default constructor for delayed initialization
  Thread();

  // Constructor that immediately starts a thread with a function
  explicit Thread(std::function<void()> func);

  // Destructor: automatically joins if thread is joinable
  ~Thread();

  // Delete copy constructor and assignment operator
  Thread(const Thread&) = delete;
  Thread& operator=(const Thread&) = delete;

  // Move constructor and assignment operator
  Thread(Thread&& other) noexcept;
  Thread& operator=(Thread&& other) noexcept;

  // Start a thread with a function
  void Start(std::function<void()> func);

  // Join the thread (blocks until thread completes)
  void Join();

  // Detach the thread (thread runs independently)
  void Detach();

  // Check if thread is joinable
  bool IsJoinable() const;

  // Get the underlying thread ID
  std::thread::id GetId() const;

  // Check if thread is still running
  bool IsRunning() const;

 private:
  std::unique_ptr<std::thread> thread_;
  std::atomic<bool> running_;
};

#endif  // TOOLS_THREAD_MANAGER_H

