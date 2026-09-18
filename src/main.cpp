#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

#include "httplib.h"
#include "nlohmann/json.hpp"

#include "auth/jwt.hpp"
#include "auth/rate_limit.hpp"
#include "db/database.hpp"
#include "http/routes.hpp"

namespace {

constexpr const char* kVersion = "0.1.0";
constexpr const char* kDefaultHost = "0.0.0.0";
constexpr int kDefaultPort = 8080;
constexpr const char* kDefaultDbPath = "data/oj.db";
constexpr int kDbBusyTimeoutMs = 5000;

volatile std::sig_atomic_t g_stop_requested = 0;

void handle_signal(int) {
  g_stop_requested = 1;
}

void print_usage(const char* argv0) {
  std::cerr << "Usage: " << argv0 << " [options]\n"
            << "Options:\n"
            << "  --host HOST           listen address, default " << kDefaultHost << "\n"
            << "  --port PORT           listen port,   default " << kDefaultPort << "\n"
            << "  --web DIR             static files root (front end), optional\n"
            << "  --db PATH             SQLite database path, default " << kDefaultDbPath << "\n"
            << "  --admin-password PWD  initial admin password (first init only);\n"
            << "                        prefer env OJ_ADMIN_PASSWORD (won't leak via ps)\n";
}

}  // namespace

int main(int argc, char** argv) {
  std::cout << std::unitbuf;
  std::cerr << std::unitbuf;

  std::string host = kDefaultHost;
  int port = kDefaultPort;
  std::string web_dir;
  std::string db_path = kDefaultDbPath;
  std::string admin_password;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--host" && i + 1 < argc) {
      host = argv[++i];
    } else if (arg == "--port" && i + 1 < argc) {
      port = std::atoi(argv[++i]);
    } else if (arg == "--web" && i + 1 < argc) {
      web_dir = argv[++i];
    } else if (arg == "--db" && i + 1 < argc) {
      db_path = argv[++i];
    } else if (arg == "--admin-password" && i + 1 < argc) {
      admin_password = argv[++i];
    } else if (arg == "--help" || arg == "-h") {
      print_usage(argv[0]);
      return 0;
    } else {
      std::cerr << "Unknown or incomplete option: " << arg << "\n";
      print_usage(argv[0]);
      return 2;
    }
  }

  if (admin_password.empty()) {
    if (const char* env_pwd = std::getenv("OJ_ADMIN_PASSWORD"); env_pwd != nullptr) {
      admin_password = env_pwd;
    }
  }

  httplib::Server svr;
  svr.set_error_handler([](const httplib::Request&, httplib::Response& res) {
    if (res.body.empty()) {
      nlohmann::json j = {{"error", "request failed"}};
      res.set_content(j.dump(), "application/json");
    }
  });

  svr.Get("/", [](const httplib::Request&, httplib::Response& res) {
    res.set_content("Oj System Server is running", "text/plain; charset=utf-8");
  });

  svr.Get("/api/health", [](const httplib::Request&, httplib::Response& res) {
    nlohmann::json j = {{"status", "ok"},
                        {"service", "oj-system"},
                        {"version", kVersion}};
    res.set_content(j.dump(), "application/json");
  });

  if (!web_dir.empty()) {
    if (svr.set_mount_point("/", web_dir)) {
      std::cout << "[oj] static files mounted from " << web_dir << "\n";
    } else {
      std::cerr << "[oj] cannot mount static files from " << web_dir << "\n";
      return 1;
    }
  }

  oj::Database db;
  std::string db_err;
  if (!db.Open(db_path, kDbBusyTimeoutMs, &db_err)) {
    std::cerr << "[oj] fatal: database init failed: " << db_err << "\n";
    return 1;
  }
  if (!db.InitSchema(&db_err)) {
    std::cerr << "[oj] fatal: schema init failed: " << db_err << "\n";
    db.Close();
    return 1;
  }
  const int admin_rc = db.EnsureAdmin("admin", "admin", admin_password, &db_err);
  if (admin_rc < 0) {
    std::cerr << "[oj] fatal: " << db_err << "\n";
    db.Close();
    return 1;
  }
  admin_password.clear();

  // JWT 签名密钥：每次进程启动生成随机密钥。重启会使已签发 token 失效，
  // 需要重新登录（对教学规模无影响；M5 若需跨重启持续登录可改为持久化密钥）。
  const std::string jwt_secret = oj::auth::GenerateJwtSecret();
  if (jwt_secret.empty()) {
    std::cerr << "[oj] fatal: cannot generate JWT secret" << "\n";
    db.Close();
    return 1;
  }

  // 登录限速：默认每账户+IP 窗口 5 分钟内最多 8 次失败。
  oj::auth::LoginRateLimiter login_limiter;

  oj::http::RegisterAuthRoutes(svr, db, jwt_secret, login_limiter);

  if (admin_rc == 1) {
    std::cout << "[oj] database ready at " << db_path
              << " (schema created, admin account seeded)\n";
  } else {
    std::cout << "[oj] database ready at " << db_path
              << " (existing data kept, admin account present)\n";
  }

  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);

  bool listen_ok = false;
  std::thread server_thread([&] { listen_ok = svr.listen(host.c_str(), port); });

  std::cout << "[oj] starting server on " << host << ":" << port << " (version "
            << kVersion << ")\n";
  std::cout << "[oj] press Ctrl+C to stop\n";

  while (!g_stop_requested) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  std::cout << "\n[oj] graceful shutdown requested, stopping server...\n";
  svr.stop();
  if (server_thread.joinable()) {
    server_thread.join();
  }
  db.Close();
  std::cout << "[oj] database closed\n";

  if (listen_ok) {
    std::cout << "[oj] server stopped cleanly\n";
    return 0;
  }
  std::cerr << "[oj] server did not start cleanly (address already in use?)\n";
  return 1;
}