#ifndef LOG_SYSTEM_H
#define LOG_SYSTEM_H

#include <iostream>
#include <string>
#include <chrono>
#include <ctime>
#include <sstream>
#include <mutex>

// Log levels
enum LogLevel {
  INFO = 0,
  WARNING = 1,
  ERROR = 2,
  FATAL = 3
};

// Log stream class to handle the << operator chaining
class LogStream {
public:
  // Constructor: captures log level and timestamp
  LogStream(LogLevel level);

  // Destructor: outputs the complete log message when the stream goes out of scope
  ~LogStream();

  // Overload << operator to accept any data type
  template <typename T>
  LogStream& operator<<(const T& data) {
      log_ss << data;  // Append data to internal string stream
      return *this;
  }

private:
  LogLevel level_;                  // Current log level
  std::stringstream log_ss;         // Internal stream to accumulate message parts
  std::chrono::system_clock::time_point timestamp_;  // Log creation time
  static std::mutex log_mutex;      // For thread safety (optional but useful)
};

// Helper function to convert LogLevel to string
std::string logLevelToString(LogLevel level);

// Helper function to get current time as string (YYYY-MM-DD HH:MM:SS)
std::string getCurrentTime();

// Macro definitions for easy logging
#define LOG(level) LogStream(LogLevel::level)

#endif // LOG_SYSTEM_H