#include "thread_manager.h"

#include <algorithm>
#include <stdexcept>

#ifdef __APPLE__
#include <pthread.h>
#elif __linux__
#include <pthread.h>
#endif

Thread::Thread() : running_(false) {}

Thread::Thread(std::function<void()> func) : running_(false) {
  thread_ = std::make_unique<std::thread>([this, func]() {
    running_ = true;
    func();
    running_ = false;
  });
}

Thread::~Thread() {
  if (thread_ && thread_->joinable()) {
    thread_->join();
  }
}

Thread::Thread(Thread&& other) noexcept
    : thread_(std::move(other.thread_)), running_(other.running_.load()) {
  other.running_ = false;
}

Thread& Thread::operator=(Thread&& other) noexcept {
  if (this != &other) {
    if (thread_ && thread_->joinable()) {
      thread_->join();
    }
    thread_ = std::move(other.thread_);
    running_ = other.running_.load();
    other.running_ = false;
  }
  return *this;
}

void Thread::Start(std::function<void()> func) {
  if (thread_ && thread_->joinable()) {
    throw std::runtime_error("Thread is already running");
  }
  running_ = false;
  thread_ = std::make_unique<std::thread>([this, func]() {
    running_ = true;
    func();
    running_ = false;
  });
}

void Thread::Join() {
  if (thread_ && thread_->joinable()) {
    thread_->join();
  }
}

void Thread::Detach() {
  if (thread_ && thread_->joinable()) {
    thread_->detach();
    running_ = false;
  }
}

bool Thread::IsJoinable() const {
  return thread_ && thread_->joinable();
}

std::thread::id Thread::GetId() const {
  if (thread_) {
    return thread_->get_id();
  }
  return std::thread::id();
}

bool Thread::IsRunning() const { return running_.load(); }

