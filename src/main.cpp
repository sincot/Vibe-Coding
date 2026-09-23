#include <chrono>
#include <csignal>
#include <cstddef>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include "auth/jwt.h"
#include "config.h"
#include "db/database.h"
#include "db/instance_lock.h"
#include "db/schema.h"
#include "db/seed.h"
#include "http/server.h"
#include "judge/compile_gate.h"
#include "judge/local_executor.h"
#include "judge/manager.h"
#include "judge/sandbox.h"
#include "log.h"

namespace {

// 仅用于在信号处理函数中写入的标志位：volatile sig_atomic_t 保证异步信号安全，
// 信号处理函数不调用任何日志或复杂清理逻辑。
volatile std::sig_atomic_t g_signal_received = 0;

extern "C" void handle_signal(int sig) {
  g_signal_received = sig;
}

void print_usage(std::ostream &os, const char *prog) {
  os << "用法: " << prog
     << " [--host <地址>] [--port <端口>] [--db <路径>] [--web <目录>] [--seed] [--help]\n"
     << "  --host  监听地址，默认 0.0.0.0\n"
     << "  --port  监听端口，默认 8080（范围 1-65535）\n"
     << "  --db    SQLite 数据库路径，默认 data/oj.db\n"
     << "  --web   前端静态资源目录，默认 web（仅该目录对外可读）\n"
     << "  --seed  导入内置种子题目后退出（幂等，不覆盖已有题目，不启动服务）\n"
     << "  --help  显示本帮助\n"
     << "\n"
      << "环境变量:\n"
      << "  OJ_ADMIN_PASSWORD  首次初始化（尚无 admin）时预置的管理员初始密码；\n"
      << "                     已有 admin 时无需设置。\n"
      << "  OJ_JWT_SECRET      JWT 签名密钥（必需，长度不少于 "
      << oj::auth::kMinJwtSecretLen << " 字节）。\n"
      << "  OJ_JWT_EXPIRES_SECONDS  JWT 有效期（秒），默认 3600。\n"
      << "  OJ_JUDGE_QUEUE_CAPACITY  判题等待队列容量（等待执行的任务数），\n"
      << "                           默认 " << oj::config::kDefaultJudgeQueueCapacity
      << "，取值 1.." << oj::config::kMaxJudgeQueueCapacity << "。\n"
      << "  OJ_JUDGE_WORKSPACE       判题 tmpfs 工作目录根，默认 "
      << oj::config::kDefaultJudgeWorkspace << "。\n"
      << "  OJ_JUDGE_ALLOW_NON_TMPFS 取 1 时允许工作目录非 tmpfs（仅开发/测试，不推荐）。\n"
      << "  OJ_JUDGE_COMPILE_CONCURRENCY  编译阶段并发门限，默认 "
      << oj::config::kDefaultCompileConcurrency << "，取值 1.."
      << oj::config::kMaxCompileConcurrency << "。\n";
}

} // namespace

int main(int argc, char **argv) {
  oj::config::Config cfg;
  bool want_help = false;
  std::string cfg_error;
  if (!oj::config::parse_args(argc, argv, cfg, want_help, cfg_error)) {
    std::cerr << cfg_error << "\n";
    print_usage(std::cerr, argv[0]);
    return 2;
  }
  if (want_help) {
    print_usage(std::cout, argv[0]);
    return 0;
  }

  // --seed：只导入种子题目后退出。仅创建/迁移表结构，不涉及 admin，也不启动服务；
  // 因此无需 OJ_JWT_SECRET 与 OJ_ADMIN_PASSWORD。重复执行幂等。
  if (cfg.seed) {
    std::string error;
    auto db = oj::Database::open(cfg.db_path, error);
    if (!db) {
      std::cerr << "数据库初始化失败: " << error << "\n";
      return 1;
    }
    if (!oj::ensure_schema(*db, error)) {
      std::cerr << "数据库结构初始化失败: " << error << "\n";
      return 1;
    }
    int created = 0;
    if (!oj::import_seed_problems(*db, created, error)) {
      std::cerr << "种子数据导入失败: " << error << "\n";
      return 1;
    }
    std::cout << "种子数据导入完成：新建题目 " << created
              << " 道（已存在的题目已跳过）\n";
    db->close();
    return 0;
  }

  // 注册信号处理：仅置位标志，真正的清理在主循环之后执行。
  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);

  oj::log(oj::LogLevel::Info, "oj_server 正在启动...");

  // 数据库初始化在 HTTP 服务开始监听前完成。数据库对象先于 server 声明，
  // 从而保证退出时按「HTTP 先停止、数据库后释放」的顺序析构。
  std::string error;
  auto db = oj::Database::open(cfg.db_path, error);
  if (!db) {
    oj::log(oj::LogLevel::Error, "数据库初始化失败: " + error);
    return 1;
  }
  oj::log(oj::LogLevel::Info, "数据库已打开: " + cfg.db_path);

  if (!oj::initialize_schema(*db, oj::config::read_admin_password(), error)) {
    oj::log(oj::LogLevel::Error, "数据库初始化失败: " + error);
    return 1;
  }
  oj::log(oj::LogLevel::Info, "数据库结构初始化完成");

  // 单实例互斥（M3.7）：对数据库锁文件持有 flock，防止两个服务实例同时操作/
  // 恢复同一批在途任务。进程崩溃时由操作系统自动释放，无需人工清理。
  auto instance_lock = oj::InstanceLock::acquire(cfg.db_path, error);
  if (!instance_lock) {
    oj::log(oj::LogLevel::Error, "实例互斥检查失败: " + error);
    return 1;
  }

  // 加载 JWT 配置：密钥缺失或无效时立即报错退出，绝不以公开默认密钥启动。
  oj::auth::JwtConfig jwt_config;
  std::string jwt_error;
  if (!oj::auth::load_jwt_config(jwt_config, jwt_error)) {
    oj::log(oj::LogLevel::Error, "JWT 配置错误: " + jwt_error);
    return 1;
  }
  oj::log(oj::LogLevel::Info,
          "JWT 配置已加载（有效期 " +
              std::to_string(jwt_config.expires_seconds) + " 秒）");

  // 判题调度配置：等待队列容量来自环境变量（有默认值、有上限、非法即报错退出）。
  oj::judge::JudgeManager::Options manager_options;
  int queue_capacity = oj::config::kDefaultJudgeQueueCapacity;
  std::string queue_error;
  if (!oj::config::read_judge_queue_capacity(queue_capacity, queue_error)) {
    oj::log(oj::LogLevel::Error, "判题队列配置错误: " + queue_error);
    return 1;
  }
  manager_options.queue_capacity = static_cast<std::size_t>(queue_capacity);

  // 判题工作目录与沙箱：在初始化阶段检查依赖与权限，任何一项不可用即拒绝启动，
  // 绝不在缺少隔离设施时降级为无沙箱运行。
  oj::judge::JudgeOptions judge_options;
  std::string workspace_error;
  bool allow_non_tmpfs = false;
  if (!oj::config::read_judge_workspace(judge_options.workspace_root,
                                        allow_non_tmpfs, workspace_error)) {
    oj::log(oj::LogLevel::Error, "判题工作目录配置错误: " + workspace_error);
    return 1;
  }

  std::string sandbox_error;
  if (!oj::judge::sandbox_supported(sandbox_error)) {
    oj::log(oj::LogLevel::Error,
            "判题沙箱不可用，拒绝启动（不会降级为无保护执行）: " +
                sandbox_error);
    return 1;
  }

  bool workspace_is_tmpfs = false;
  if (!allow_non_tmpfs) {
    std::string tmpfs_error;
    if (!oj::judge::path_is_tmpfs(judge_options.workspace_root, tmpfs_error)) {
      oj::log(oj::LogLevel::Error, "判题工作目录检查失败: " + tmpfs_error);
      return 1;
    }
    workspace_is_tmpfs = true;
  } else {
    oj::log(oj::LogLevel::Warn,
            "已允许非 tmpfs 工作目录（OJ_JUDGE_ALLOW_NON_TMPFS=1），"
            "仅限开发/测试，正式部署请挂载 tmpfs");
  }

  std::string self_test_error;
  if (!oj::judge::LocalExecutor::sandbox_self_test(judge_options.workspace_root,
                                                   self_test_error)) {
    oj::log(oj::LogLevel::Error, "判题沙箱自检失败，拒绝启动: " +
                                     self_test_error);
    return 1;
  }
  judge_options.sandbox_enabled = true;

  // 编译阶段并发门限：单独约束高内存的编译阶段（同时最多 N 个编译），运行阶段仍
  // 由 worker 数控制。等待编译许可的时间计入该任务的全局判题预算并可被取消。
  int compile_concurrency = oj::config::kDefaultCompileConcurrency;
  std::string compile_concurrency_error;
  if (!oj::config::read_judge_compile_concurrency(compile_concurrency,
                                                  compile_concurrency_error)) {
    oj::log(oj::LogLevel::Error,
            "编译并发配置错误: " + compile_concurrency_error);
    return 1;
  }
  judge_options.compile_gate =
      std::make_shared<oj::judge::CompileGate>(compile_concurrency);

  const long long capacity_bytes =
      oj::judge::mount_capacity_bytes(judge_options.workspace_root);
  std::string workspace_desc = judge_options.workspace_root + "（" +
                               (workspace_is_tmpfs ? "tmpfs" : "非 tmpfs");
  if (capacity_bytes > 0) {
    workspace_desc += "，容量上限 " +
                      std::to_string(capacity_bytes / (1024 * 1024)) + " MiB";
  }
  workspace_desc += "）";
  oj::log(oj::LogLevel::Info, "判题工作目录已就绪: " + workspace_desc);

  oj::HttpServer server(cfg.host, cfg.port, *db, std::move(jwt_config),
                        /*enable_test_routes=*/false,
                        /*judge_executor=*/nullptr,
                        /*judge_options=*/std::move(judge_options),
                        /*web_root=*/cfg.web_root,
                        /*manager_options=*/manager_options);

  // 启动恢复（M3.7）：数据库迁移与判题环境检查已完成，在开始接收新提交前扫描并
  // 重新入队崩溃前未结算的在途任务。恢复与新提交共用同一有界队列与 worker。
  const std::size_t recovered = server.recover_pending_tasks();
  if (recovered > 0) {
    oj::log(oj::LogLevel::Info, "启动恢复：已重新入队 " +
                                    std::to_string(recovered) +
                                    " 个未结算在途任务");
  }

  if (!server.start(error)) {
    oj::log(oj::LogLevel::Error, "启动失败: " + error);
    return 1;
  }

  oj::log(oj::LogLevel::Info,
          "判题调度已就绪（worker " +
              std::to_string(server.judge_manager()->worker_count()) +
              " 个，等待队列容量 " +
              std::to_string(server.judge_manager()->queue_capacity()) + "）");

  oj::log(oj::LogLevel::Info,
          "HTTP 服务已启动，监听 " + cfg.host + ":" + std::to_string(cfg.port));
  oj::log(oj::LogLevel::Info,
          "健康检查接口: http://" + cfg.host + ":" + std::to_string(cfg.port) +
              "/api/health");
  oj::log(oj::LogLevel::Info,
          "前端页面: http://" + cfg.host + ":" + std::to_string(cfg.port) +
              "/（静态资源目录：" + cfg.web_root + "）");

  // 等待停止信号。
  while (g_signal_received == 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  oj::log(oj::LogLevel::Info,
          "收到信号 " + std::to_string(static_cast<int>(g_signal_received)) +
              "，正在优雅停止...");
  server.stop();
  oj::log(oj::LogLevel::Info, "HTTP 服务已停止");
  db->close();
  oj::log(oj::LogLevel::Info, "数据库资源已释放");

  return 0;
}
