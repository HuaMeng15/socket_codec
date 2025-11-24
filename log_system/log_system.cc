#include "log_system.h"

// Initialize static mutex for thread safety
std::mutex LogStream::log_mutex;

const LogLevel kMinLogLevel = LogLevel::VERBOSE;

// Convert log level to human-readable string with color coding
std::string logLevelToString(LogLevel level) {
  switch (level) {
    case LogLevel::VERBOSE:
      return "\033[34mVERBOSE\033[0m";  // Blue
    case LogLevel::INFO:
      return "\033[32mINFO\033[0m";  // Green
    case LogLevel::WARNING:
      return "\033[33mWARNING\033[0m";  // Yellow
    case LogLevel::ERROR:
      return "\033[31mERROR\033[0m";  // Red
    case LogLevel::FATAL:
      return "\033[41mFATAL\033[0m";  // Red background
    default:
      return "UNKNOWN";
  }
}

// Get current time formatted as "YYYY-MM-DD HH:MM:SS"
std::string getCurrentTime() {
  auto now = std::chrono::system_clock::now();
  std::time_t now_time = std::chrono::system_clock::to_time_t(now);
  struct tm time_info;
  localtime_r(&now_time, &time_info);  // Thread-safe version of localtime

  char time_str[20];
  std::strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &time_info);
  return std::string(time_str);
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