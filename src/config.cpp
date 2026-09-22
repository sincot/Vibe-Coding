#include "config.h"

#include <cstdlib>

namespace oj {
namespace config {

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

bool parse_args(int argc, char **argv, Config &cfg, bool &want_help,
                std::string &error) {
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];

    if (arg == "--help" || arg == "-h") {
      want_help = true;
      return true;
    }

    if (arg == "--host") {
      if (i + 1 >= argc) {
        error = "错误: --host 需要一个参数";
        return false;
      }
      cfg.host = argv[++i];
      if (cfg.host.empty()) {
        error = "错误: --host 参数不能为空";
        return false;
      }
      continue;
    }

    if (arg == "--port") {
      if (i + 1 >= argc) {
        error = "错误: --port 需要一个参数";
        return false;
      }
      if (!parse_port(argv[++i], cfg.port)) {
        error = "错误: 非法端口 \"" + std::string(argv[i]) +
                "\"（端口须为 1-65535 之间的整数）";
        return false;
      }
      continue;
    }

    if (arg == "--db") {
      if (i + 1 >= argc) {
        error = "错误: --db 需要一个参数";
        return false;
      }
      cfg.db_path = argv[++i];
      if (cfg.db_path.empty()) {
        error = "错误: --db 参数不能为空";
        return false;
      }
      continue;
    }

    if (arg == "--web") {
      if (i + 1 >= argc) {
        error = "错误: --web 需要一个参数";
        return false;
      }
      cfg.web_root = argv[++i];
      if (cfg.web_root.empty()) {
        error = "错误: --web 参数不能为空";
        return false;
      }
      continue;
    }

    if (arg == "--seed") {
      cfg.seed = true;
      continue;
    }

    error = "错误: 未知参数 \"" + arg + "\"";
    return false;
  }
  return true;
}

std::optional<std::string> read_admin_password() {
  const char *value = std::getenv("OJ_ADMIN_PASSWORD");
  if (value == nullptr) {
    return std::nullopt;
  }
  return std::string(value);
}

} // namespace config
} // namespace oj
