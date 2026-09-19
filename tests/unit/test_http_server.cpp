// HTTP 服务单元测试（M0.2，基于 gtest）。
//
// 以组件级方式验证 HttpServer 的核心行为：健康检查接口、启动/停止生命周期、
// 端口占用检测与幂等停止、停止后同端口重启。使用 /tmp 隔离临时 SQLite 库 +
// 环回地址随机空闲端口 + httplib::Client，不触碰正式数据库 data/oj.db 与外部网络。
//
// 运行方式：ctest --test-dir build -R http_server_unit --output-on-failure
// 或直接执行 build/oj_http_server_test。

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "auth/jwt.h"
#include "db/database.h"
#include "db/schema.h"
#include "http/server.h"

namespace {

using nlohmann::json;

// 唯一临时目录，析构时自动删除。
class TempDir {
public:
  explicit TempDir(const std::string &label) {
    std::filesystem::path base = std::filesystem::temp_directory_path();
    for (int i = 0; i < 1000; ++i) {
      auto candidate =
          base / (label + "_" + std::to_string(::getpid()) + "_" +
                  std::to_string(i));
      std::error_code ec;
      std::filesystem::create_directories(candidate, ec);
      if (!ec) {
        path_ = candidate;
        return;
      }
    }
    path_.clear();
  }

  ~TempDir() {
    if (!path_.empty()) {
      std::error_code ec;
      std::filesystem::remove_all(path_, ec);
    }
  }

  std::string db_path() const { return (path_ / "oj.db").string(); }

private:
  std::filesystem::path path_;
};

// 通过绑定端口 0 由内核分配一个空闲端口，随后释放供服务使用。
int find_free_port() {
  int sock = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = inet_addr("127.0.0.1");
  addr.sin_port = 0;
  ::bind(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
  socklen_t len = sizeof(addr);
  ::getsockname(sock, reinterpret_cast<sockaddr *>(&addr), &len);
  int port = ntohs(addr.sin_port);
  ::close(sock);
  return port;
}

oj::auth::JwtConfig make_jwt_config() {
  oj::auth::JwtConfig cfg;
  cfg.secret = "test-secret-0123456789abcdef";
  cfg.expires_seconds = 3600;
  return cfg;
}

class ServerFixture : public ::testing::Test {
protected:
  void SetUp() override {
    dir_ = std::make_unique<TempDir>("http_unit");
    std::string err;
    db_ = oj::Database::open(dir_->db_path(), err);
    ASSERT_TRUE(db_ != nullptr) << "打开测试库失败: " << err;
    ASSERT_TRUE(oj::initialize_schema(*db_, "AdminSecret123!", err))
        << "初始化结构失败: " << err;
  }

  void TearDown() override {
    if (server_) {
      server_->stop();
    }
    if (db_) {
      db_->close();
    }
  }

  // 在随机空闲端口上启动服务，失败时记录并返回空指针。
  std::unique_ptr<oj::HttpServer> start_server(int &port) {
    port = find_free_port();
    auto server = std::make_unique<oj::HttpServer>("127.0.0.1", port, *db_,
                                                   make_jwt_config());
    std::string err;
    if (!server->start(err)) {
      ADD_FAILURE() << "服务启动失败: " << err;
      return nullptr;
    }
    // 等待监听线程进入 accept 循环，避免首个请求竞态。
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    return server;
  }

  std::unique_ptr<TempDir> dir_;
  std::unique_ptr<oj::Database> db_;
  std::unique_ptr<oj::HttpServer> server_;
};

TEST_F(ServerFixture, HealthEndpointReturnsOk) {
  int port = 0;
  server_ = start_server(port);
  ASSERT_NE(server_, nullptr);
  EXPECT_TRUE(server_->is_running());

  httplib::Client cli("127.0.0.1", port);
  auto res = cli.Get("/api/health");
  ASSERT_TRUE(res) << "健康检查请求失败";
  EXPECT_EQ(res->status, 200);
  EXPECT_EQ(res->get_header_value("Content-Type"), "application/json");
  json parsed = json::parse(res->body);
  ASSERT_TRUE(parsed.is_object());
  EXPECT_EQ(parsed.value("status", ""), "ok");
}

TEST_F(ServerFixture, PortOccupiedFailsToStart) {
  int port = 0;
  server_ = start_server(port);
  ASSERT_NE(server_, nullptr);

  // 同一端口再次启动应失败，且错误信息明确指出端口被占用。
  oj::HttpServer second("127.0.0.1", port, *db_, make_jwt_config());
  std::string err;
  EXPECT_FALSE(second.start(err));
  EXPECT_NE(err.find("占用"), std::string::npos) << "错误信息应提示端口被占用";
}

TEST_F(ServerFixture, StopIsIdempotent) {
  int port = 0;
  server_ = start_server(port);
  ASSERT_NE(server_, nullptr);
  EXPECT_TRUE(server_->is_running());

  server_->stop();
  EXPECT_FALSE(server_->is_running());

  // 重复停止不应崩溃或产生副作用。
  server_->stop();
  EXPECT_FALSE(server_->is_running());
}

TEST_F(ServerFixture, RestartOnSamePortAfterStop) {
  int port = 0;
  server_ = start_server(port);
  ASSERT_NE(server_, nullptr);

  {
    httplib::Client cli("127.0.0.1", port);
    auto res = cli.Get("/api/health");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
  }

  server_->stop();
  EXPECT_FALSE(server_->is_running());

  // 停止后在同一端口重新启动，接口应再次可访问。
  auto second =
      std::make_unique<oj::HttpServer>("127.0.0.1", port, *db_, make_jwt_config());
  std::string err;
  ASSERT_TRUE(second->start(err)) << "停止后同端口重启失败: " << err;
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  {
    httplib::Client cli("127.0.0.1", port);
    auto res = cli.Get("/api/health");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 200);
  }
  second->stop();
}

} // namespace
