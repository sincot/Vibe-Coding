#include <chrono>
#include <csignal>
#include <iostream>
#include <string>
#include <thread>

#include "http/server.h"
#include "log.h"

namespace {

// 基础配置：仅包含监听地址与端口，均提供默认值。
struct Config {
  std::string host = "0.0.0.0";
  int port = 8080;
};

// 仅用于在信号处理函数中写入的标志位：volatile sig_atomic_t 保证异步信号安全，
// 信号处理函数不调用任何日志或复杂清理逻辑。
volatile std::sig_atomic_t g_signal_received = 0;

extern "C" void handle_signal(int sig) {
  g_signal_received = sig;
}

void print_usage(std::ostream &os, const char *prog) {
  os << "用法: " << prog << " [--host <地址>] [--port <端口>] [--help]\n"
     << "  --host  监听地址，默认 0.0.0.0\n"
     << "  --port  监听端口，默认 8080（范围 1-65535）\n"
     << "  --help  显示本帮助\n";
}

bool parse_port(const std::string &text, int &out) {
  if (text.empty()) {
    return false;
  }
  for (char c : text) {
    if (c < '0' || c > '9') {
      return false;
    }
  }
  long value = 0;
  try {
    value = std::stol(text);
  } catch (...) {
    return false;
  }
  if (value < 1 || value > 65535) {
    return false;
  }
  out = static_cast<int>(value);
  return true;
}

// 解析命令行参数。want_help=true 表示打印帮助后正常退出（返回码 0）。
// 返回 false 表示参数非法，调用方以非零返回码退出。
bool parse_args(int argc, char **argv, Config &cfg, bool &want_help) {
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];

    if (arg == "--help" || arg == "-h") {
      want_help = true;
      return true;
    }

    if (arg == "--host") {
      if (i + 1 >= argc) {
        std::cerr << "错误: --host 需要一个参数\n";
        return false;
      }
      cfg.host = argv[++i];
      if (cfg.host.empty()) {
        std::cerr << "错误: --host 参数不能为空\n";
        return false;
      }
      continue;
    }

    if (arg == "--port") {
      if (i + 1 >= argc) {
        std::cerr << "错误: --port 需要一个参数\n";
        return false;
      }
      if (!parse_port(argv[++i], cfg.port)) {
        std::cerr << "错误: 非法端口 \"" << argv[i]
                  << "\"（端口须为 1-65535 之间的整数）\n";
        return false;
      }
      continue;
    }

    std::cerr << "错误: 未知参数 \"" << arg << "\"\n";
    return false;
  }
  return true;
}

} // namespace

int main(int argc, char **argv) {
  Config cfg;
  bool want_help = false;
  if (!parse_args(argc, argv, cfg, want_help)) {
    print_usage(std::cerr, argv[0]);
    return 2;
  }
  if (want_help) {
    print_usage(std::cout, argv[0]);
    return 0;
  }

  // 注册信号处理：仅置位标志，真正的清理在主循环之后执行。
  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);

  oj::log(oj::LogLevel::Info, "oj_server 正在启动...");

  oj::HttpServer server(cfg.host, cfg.port);
  std::string error;
  if (!server.start(error)) {
    oj::log(oj::LogLevel::Error, "启动失败: " + error);
    return 1;
  }

  oj::log(oj::LogLevel::Info,
          "HTTP 服务已启动，监听 " + cfg.host + ":" + std::to_string(cfg.port));
  oj::log(oj::LogLevel::Info,
          "健康检查接口: http://" + cfg.host + ":" + std::to_string(cfg.port) +
              "/api/health");

  // 等待停止信号。
  while (g_signal_received == 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  oj::log(oj::LogLevel::Info,
          "收到信号 " + std::to_string(static_cast<int>(g_signal_received)) +
              "，正在优雅停止...");
  server.stop();
  oj::log(oj::LogLevel::Info, "HTTP 服务已停止");

  return 0;
}
