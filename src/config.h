#pragma once

#include <optional>
#include <string>

namespace oj {
namespace config {

// 基础服务配置：监听地址、端口与数据库路径，均提供默认值。
struct Config {
  std::string host = "0.0.0.0";
  int port = 8080;
  std::string db_path = "data/oj.db";
};

// 校验端口字符串是否为 1..65535 之间的整数。成功写入 out 并返回 true；
// 失败返回 false 且不改写 out。
bool parse_port(const std::string &text, int &out);

// 解析命令行参数。want_help=true 表示请求打印帮助（调用方以返回码 0 正常退出）。
// 返回 false 表示参数非法，error 给出明确原因（调用方以非零返回码退出）。
bool parse_args(int argc, char **argv, Config &cfg, bool &want_help,
                std::string &error);

// 读取初始管理员密码环境变量 OJ_ADMIN_PASSWORD；未设置时返回 nullopt。
// 仅当数据库中尚无 admin 时才读取并使用；已有 admin 时无需设置。
std::optional<std::string> read_admin_password();

} // namespace config
} // namespace oj
