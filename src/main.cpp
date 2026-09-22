#include <chrono>
#include <csignal>
#include <iostream>
#include <string>
#include <thread>

#include "auth/jwt.h"
#include "config.h"
#include "db/database.h"
#include "db/schema.h"
#include "db/seed.h"
#include "http/server.h"
#include "judge/manager.h"
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
      << "，取值 1.." << oj::config::kMaxJudgeQueueCapacity << "。\n";
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

  oj::HttpServer server(cfg.host, cfg.port, *db, std::move(jwt_config),
                        /*enable_test_routes=*/false,
                        /*judge_executor=*/nullptr,
                        /*judge_options=*/{},
                        /*web_root=*/cfg.web_root,
                        /*manager_options=*/manager_options);
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
