#pragma once

#include <cstdio>
#include <ctime>
#include <string>

namespace oj {

enum class LogLevel {
  Info,
  Warn,
  Error,
};

// 基础运行日志：带时间戳与级别，写入 stdout（错误写入 stderr）。
// 注意：该函数不保证异步信号安全，严禁在信号处理函数中调用。
inline void log(LogLevel level, const std::string &msg) {
  const char *tag = "INFO";
  if (level == LogLevel::Warn) {
    tag = "WARN";
  } else if (level == LogLevel::Error) {
    tag = "ERROR";
  }

  std::time_t now = std::time(nullptr);
  char ts[32] = {0};
  std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", std::localtime(&now));

  std::FILE *out = (level == LogLevel::Error) ? stderr : stdout;
  std::fprintf(out, "[%s] [%s] %s\n", ts, tag, msg.c_str());
  std::fflush(out);
}

} // namespace oj
