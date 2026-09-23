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
  // 前端静态资源根目录。仅该目录下的文件可被 HTTP 静态托管访问，项目根目录、
  // 数据库、配置与判题临时目录均不在其中（详见 README「前端」一节）。
  std::string web_root = "web";
  // --seed：只导入内置种子题目后退出，不启动 HTTP 服务。默认为 false，
  // 因此正常启动不会无条件重写题目数据。
  bool seed = false;
};

// 校验端口字符串是否为 1..65535 之间的整数。成功写入 out 并返回 true；
// 失败返回 false 且不改写 out。
bool parse_port(const std::string &text, int &out);

// 判题等待队列容量的默认值与上限（等待执行的任务数，非正在执行数）。
inline constexpr int kDefaultJudgeQueueCapacity = 32;
inline constexpr int kMaxJudgeQueueCapacity = 256;

// 判题工作目录（tmpfs）的默认挂载点（SPEC JUDGE-04 / dependence.md 第 3.9 节）。
inline constexpr const char *kDefaultJudgeWorkspace = "/opt/oj-tmpfs";

// 读取判题工作目录配置：
//   - OJ_JUDGE_WORKSPACE：工作目录根，默认 /opt/oj-tmpfs；
//   - OJ_JUDGE_ALLOW_NON_TMPFS：取 1/true/yes 时允许工作目录不是 tmpfs（仅用于
//     无挂载权限的开发/测试环境，会显著告警；正式服务不应设置）。
// 两个变量本身非法时返回 false 并置 error。
bool read_judge_workspace(std::string &out, bool &allow_non_tmpfs,
                          std::string &error);

// 读取判题等待队列容量环境变量 OJ_JUDGE_QUEUE_CAPACITY。
// 未设置时写入默认值并返回 true；已设置但非 1..kMaxJudgeQueueCapacity 的整数时
// 返回 false 并置 error（服务启动报错退出），绝不会无限增长。
bool read_judge_queue_capacity(int &out, std::string &error);

// 编译阶段并发门限的默认值与上限。默认 2：在 3.3 GiB 级服务器上限制高内存的
// ASan 编译阶段同时数量，运行阶段仍由 worker 数（min(CPU,8)）控制。
inline constexpr int kDefaultCompileConcurrency = 2;
inline constexpr int kMaxCompileConcurrency = 64;

// 读取 OJ_JUDGE_COMPILE_CONCURRENCY（编译阶段并发门限）。
// 未设置时写入默认值并返回 true；已设置但非 1..kMaxCompileConcurrency 的整数时
// 返回 false 并置 error。
bool read_judge_compile_concurrency(int &out, std::string &error);

// 解析命令行参数。want_help=true 表示请求打印帮助（调用方以返回码 0 正常退出）。
// 返回 false 表示参数非法，error 给出明确原因（调用方以非零返回码退出）。
bool parse_args(int argc, char **argv, Config &cfg, bool &want_help,
                std::string &error);

// 读取初始管理员密码环境变量 OJ_ADMIN_PASSWORD；未设置时返回 nullopt。
// 仅当数据库中尚无 admin 时才读取并使用；已有 admin 时无需设置。
std::optional<std::string> read_admin_password();

} // namespace config
} // namespace oj
