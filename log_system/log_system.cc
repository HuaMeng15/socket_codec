#include "log_system.h"

#include <cerrno>
#include <condition_variable>
#include <deque>
#include <utility>
#include <unistd.h>

namespace {

class AsyncLogSink {
 public:
  static AsyncLogSink& Instance() {
    static AsyncLogSink sink;
    return sink;
  }

  void Enqueue(std::string line) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      queue_.push_back(std::move(line));
    }
    cv_.notify_one();
  }

  void Flush() {
    std::unique_lock<std::mutex> lock(mutex_);
    drained_cv_.wait(lock, [this] { return queue_.empty() && !writing_; });
  }

  ~AsyncLogSink() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) {
      worker_.join();
    }
  }

 private:
  AsyncLogSink() : worker_(&AsyncLogSink::Run, this) {}

  static void WriteAll(const std::string& line) {
    size_t offset = 0;
    while (offset < line.size()) {
      ssize_t written =
          ::write(STDERR_FILENO, line.data() + offset, line.size() - offset);
      if (written > 0) {
        offset += static_cast<size_t>(written);
      } else if (written < 0 && errno == EINTR) {
        continue;
      } else {
        break;
      }
    }
  }

  void Run() {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
      cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
      if (queue_.empty() && stopping_) {
        break;
      }

      std::string line = std::move(queue_.front());
      queue_.pop_front();
      writing_ = true;
      lock.unlock();
      WriteAll(line);
      lock.lock();
      writing_ = false;
      if (queue_.empty()) {
        drained_cv_.notify_all();
      }
    }
    drained_cv_.notify_all();
  }

  std::mutex mutex_;
  std::condition_variable cv_;
  std::condition_variable drained_cv_;
  std::deque<std::string> queue_;
  std::thread worker_;
  bool stopping_ = false;
  bool writing_ = false;
};

}  // namespace

// Initialize static mutex for thread safety
std::mutex LogStream::log_mutex;

const LogLevel kMinLogLevel = LogLevel::VERBOSE;

// Convert log level to human-readable string; use colors only when stderr is a TTY
std::string logLevelToString(LogLevel level) {
  static const bool use_color = isatty(STDERR_FILENO);

  switch (level) {
    case LogLevel::VERBOSE:
      return use_color ? "\033[34mVERBOSE\033[0m" : "VERBOSE";
    case LogLevel::INFO:
      return use_color ? "\033[32mINFO\033[0m" : "INFO";
    case LogLevel::WARNING:
      return use_color ? "\033[33mWARNING\033[0m" : "WARNING";
    case LogLevel::ERROR:
      return use_color ? "\033[31mERROR\033[0m" : "ERROR";
    case LogLevel::FATAL:
      return use_color ? "\033[41mFATAL\033[0m" : "FATAL";
    default:
      return "UNKNOWN";
  }
}

// Get current time formatted as "YYYY-MM-DD HH:MM:SS.mmm"
std::string getCurrentTime() {
  auto now = std::chrono::system_clock::now();
  std::time_t now_time = std::chrono::system_clock::to_time_t(now);
  struct tm time_info;
  localtime_r(&now_time, &time_info);  // Thread-safe version of localtime

  // Get milliseconds
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      now.time_since_epoch()) % 1000;

  char time_str[24];
  std::strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &time_info);

  // Append milliseconds with proper padding (always 3 digits)
  std::string result(time_str);
  result += ".";
  int ms_value = static_cast<int>(ms.count());
  if (ms_value < 10) {
    result += "00";
  } else if (ms_value < 100) {
    result += "0";
  }
  result += std::to_string(ms_value);

  return result;
}

// Get thread ID as a formatted string
std::string getThreadId() {
  std::thread::id id = std::this_thread::get_id();
  std::stringstream ss;
  ss << id;
  return ss.str();
}

// LogStream constructor: capture timestamp and level
LogStream::LogStream(LogLevel level)
    : level_(level), timestamp_(std::chrono::system_clock::now()) {}

// LogStream destructor: output the complete log message
LogStream::~LogStream() {
  // std::lock_guard<std::mutex> lock(log_mutex);  // Ensure thread-safe output

  if (level_ < kMinLogLevel) {
    return;  // Skip logs below the minimum level
  }

  // Format the final log line with thread ID
  std::string log_line = "[" + getCurrentTime() + "] " + "[" +
                         logLevelToString(level_) + "] " + "[TID:" +
                         getThreadId() + "] " + log_ss.str() + "\n";

  // Logging must never block capture, feedback, pacing, or receiver processing.
  // A dedicated sink thread owns the potentially blocking file descriptor.
  AsyncLogSink& sink = AsyncLogSink::Instance();
  sink.Enqueue(std::move(log_line));

  // For FATAL level, exit the program after logging
  if (level_ == LogLevel::FATAL) {
    sink.Flush();
    std::exit(EXIT_FAILURE);
  }
}
