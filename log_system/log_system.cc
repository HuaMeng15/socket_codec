#include "log_system.h"

#include <unistd.h>

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

  // Output to standard error (common for logs, but can change to std::cout)
  std::cerr << log_line;

  // For FATAL level, exit the program after logging
  if (level_ == LogLevel::FATAL) {
    std::exit(EXIT_FAILURE);
  }
}