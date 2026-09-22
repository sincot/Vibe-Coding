// M1.7 静态资源托管单元测试（基于 gtest）。
//
// 以组件级方式验证 HttpServer 的前端静态资源托管：
//   - 仅挂载给定的 web_root，`/` 映射到 index.html，HTML/CSS/JS 的 MIME 正确；
//   - 工程根目录/数据库/配置/判题临时目录等不在托管范围，目录穿越（`..` 与 URL
//     编码变体）被拒绝，被拒绝路径不回显内容；
//   - 空或缺失的 web_root 只是跳过静态托管，不影响服务启动与 `/api` 路由；
//   - 静态托管不影响既有 API（健康检查、题目列表、注册 POST）。
//
// 使用 /tmp 隔离临时 SQLite 库 + 临时 web 目录 + 环回随机空闲端口 + httplib::Client，
// 不触碰正式数据库与正式 web 目录。
//
// 运行方式：ctest --test-dir build -R static_files_unit --output-on-failure
// 或直接执行 build/oj_static_files_test。

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <fstream>
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
namespace fs = std::filesystem;

// 唯一临时目录，析构时递归删除。
class TempDir {
public:
  explicit TempDir(const std::string &label) {
    fs::path base = fs::temp_directory_path();
    for (int i = 0; i < 1000; ++i) {
      auto candidate = base / (label + "_" + std::to_string(::getpid()) + "_" +
                               std::to_string(i));
      std::error_code ec;
      fs::create_directories(candidate, ec);
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
      fs::remove_all(path_, ec);
    }
  }

  const fs::path &path() const { return path_; }

private:
  fs::path path_;
};

// 通过内核分配空闲端口后释放，供被测服务使用。
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

void write_file(const fs::path &path, const std::string &content) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  out << content;
}

constexpr char kSecretMarker[] = "TOP-SECRET-DO-NOT-LEAK";

class StaticFileFixture : public ::testing::Test {
protected:
  void SetUp() override {
    base_ = std::make_unique<TempDir>("static_unit");
    web_ = base_->path() / "web";

    // 待托管的前端资源。
    write_file(web_ / "index.html",
               "<!doctype html><title>OJ</title><h1>OJ-INDEX-MARKER</h1>");
    write_file(web_ / "css" / "site.css", "body{color:#123456}");
    write_file(web_ / "js" / "app.js", "export const marker='OJ-APP-MARKER';");

    // 托管目录之外的敏感文件：绝不应通过任何路径被读取。
    write_file(base_->path() / "secret.txt", std::string(kSecretMarker) + "\n");

    std::string err;
    db_ = oj::Database::open((base_->path() / "oj.db").string(), err);
    ASSERT_TRUE(db_ != nullptr) << "打开测试库失败: " << err;
    ASSERT_TRUE(oj::initialize_schema(*db_, "AdminSecret123!", err))
        << "初始化结构失败: " << err;
  }

  void TearDown() override {
    if (server_) server_->stop();
    if (db_) db_->close();
  }

  // 以给定 web_root 启动服务；web_root 为空串表示不启用静态托管。
  std::unique_ptr<oj::HttpServer> start(const std::string &web_root, int &port) {
    port = find_free_port();
    auto server = std::make_unique<oj::HttpServer>(
        "127.0.0.1", port, *db_, make_jwt_config(),
        /*enable_test_routes=*/false, /*judge_executor=*/nullptr,
        oj::judge::JudgeOptions{}, web_root);
    std::string err;
    if (!server->start(err)) {
      ADD_FAILURE() << "服务启动失败: " << err;
      return nullptr;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    return server;
  }

  std::unique_ptr<TempDir> base_;
  fs::path web_;
  std::unique_ptr<oj::Database> db_;
  std::unique_ptr<oj::HttpServer> server_;
};

// 正常路径：仅 web_root 被托管，`/` 与显式文件均可访问且 MIME 正确。
TEST_F(StaticFileFixture, ServesIndexAndAssetsWithCorrectMime) {
  int port = 0;
  server_ = start(web_.string(), port);
  ASSERT_NE(server_, nullptr);

  httplib::Client cli("127.0.0.1", port);

  auto root = cli.Get("/");
  ASSERT_TRUE(root) << "GET / 失败";
  EXPECT_EQ(root->status, 200);
  EXPECT_EQ(root->get_header_value("Content-Type"), "text/html");
  EXPECT_NE(root->body.find("OJ-INDEX-MARKER"), std::string::npos);

  auto index = cli.Get("/index.html");
  ASSERT_TRUE(index);
  EXPECT_EQ(index->status, 200);
  EXPECT_EQ(index->get_header_value("Content-Type"), "text/html");

  auto css = cli.Get("/css/site.css");
  ASSERT_TRUE(css);
  EXPECT_EQ(css->status, 200);
  EXPECT_EQ(css->get_header_value("Content-Type"), "text/css");
  EXPECT_NE(css->body.find("#123456"), std::string::npos);

  auto js = cli.Get("/js/app.js");
  ASSERT_TRUE(js);
  EXPECT_EQ(js->status, 200);
  EXPECT_EQ(js->get_header_value("Content-Type"), "application/javascript");
  EXPECT_NE(js->body.find("OJ-APP-MARKER"), std::string::npos);
}

// 边界：托管目录内不存在的路径返回 404，不构成目录遍历助手。
TEST_F(StaticFileFixture, UnknownStaticPathReturns404) {
  int port = 0;
  server_ = start(web_.string(), port);
  ASSERT_NE(server_, nullptr);

  httplib::Client cli("127.0.0.1", port);
  auto res = cli.Get("/no-such-file.js");
  ASSERT_TRUE(res);
  EXPECT_EQ(res->status, 404);
}

// 错误路径：托管目录之外的文件（含同级的 secret.txt）不可通过直接路径访问。
TEST_F(StaticFileFixture, SiblingFileNotServed) {
  int port = 0;
  server_ = start(web_.string(), port);
  ASSERT_NE(server_, nullptr);

  httplib::Client cli("127.0.0.1", port);
  auto res = cli.Get("/secret.txt");
  if (res) {
    EXPECT_NE(res->status, 200);
    EXPECT_EQ(res->body.find(kSecretMarker), std::string::npos);
  }
}

// 错误路径：目录穿越（原样与 URL 编码变体）必须被拒绝且不回显内容。
TEST_F(StaticFileFixture, BlocksTraversalAndSensitivePaths) {
  int port = 0;
  server_ = start(web_.string(), port);
  ASSERT_NE(server_, nullptr);

  httplib::Client cli("127.0.0.1", port);
  cli.set_url_encode(false); // 发送原样路径，避免客户端再次编码导致测不到穿越

  const char *paths[] = {
      "/../secret.txt",
      "/css/../../secret.txt",
      "/%2e%2e/secret.txt",
      "/..%2fsecret.txt",
      "/.git/config",
      "/../../../../etc/passwd",
      "/../oj.db",
      "/data/oj.db",
  };
  for (const char *path : paths) {
    auto res = cli.Get(path);
    // 这些请求必须真实到达服务并得到拒绝响应（而非被客户端静默丢弃），
    // 否则测试可能因「未发出请求」而假通过。
    ASSERT_TRUE(res) << "请求未得到响应: " << path;
    EXPECT_NE(res->status, 200) << "不应成功访问: " << path;
    EXPECT_EQ(res->body.find(kSecretMarker), std::string::npos)
        << "穿越路径回显了敏感内容: " << path;
    EXPECT_EQ(res->body.find("root:"), std::string::npos)
        << "越界读取到系统文件: " << path;
  }
}

// 回归：静态托管不影响既有 API（健康检查、空库题目列表、注册 POST）。
TEST_F(StaticFileFixture, ApiRoutesRemainAvailable) {
  int port = 0;
  server_ = start(web_.string(), port);
  ASSERT_NE(server_, nullptr);

  httplib::Client cli("127.0.0.1", port);

  auto health = cli.Get("/api/health");
  ASSERT_TRUE(health);
  EXPECT_EQ(health->status, 200);
  EXPECT_EQ(health->get_header_value("Content-Type"), "application/json");
  json health_body = json::parse(health->body);
  EXPECT_EQ(health_body.value("status", ""), "ok");

  auto problems = cli.Get("/api/problems");
  ASSERT_TRUE(problems);
  EXPECT_EQ(problems->status, 200);
  json list = json::parse(problems->body);
  ASSERT_TRUE(list.contains("problems"));
  EXPECT_TRUE(list["problems"].is_array());
  EXPECT_EQ(list.value("total", -1), 0) << "隔离空库应无题目";

  json reg = {{"nickname", "static-file-user"}, {"password", "Secret123"}};
  auto registered = cli.Post("/api/register", reg.dump(), "application/json");
  ASSERT_TRUE(registered);
  EXPECT_EQ(registered->status, 201);
  json reg_body = json::parse(registered->body);
  EXPECT_EQ(reg_body.value("nickname", ""), "static-file-user");
  EXPECT_EQ(reg_body.value("account", "").size(), 10u);
}

// 边界：web_root 为空串时不启用静态托管，服务仍可正常提供 API。
TEST_F(StaticFileFixture, EmptyWebRootDisablesStaticButKeepsApi) {
  int port = 0;
  server_ = start("", port);
  ASSERT_NE(server_, nullptr);

  httplib::Client cli("127.0.0.1", port);

  auto root = cli.Get("/");
  ASSERT_TRUE(root);
  EXPECT_NE(root->status, 200) << "未启用静态托管时 / 不应返回前端内容";

  auto health = cli.Get("/api/health");
  ASSERT_TRUE(health);
  EXPECT_EQ(health->status, 200);
}

// 边界：web_root 指向不存在的目录时只跳过静态托管，不导致启动失败或 API 不可用。
TEST_F(StaticFileFixture, MissingWebRootIsSkipped) {
  int port = 0;
  server_ = start((base_->path() / "no-such-web").string(), port);
  ASSERT_NE(server_, nullptr);

  httplib::Client cli("127.0.0.1", port);

  auto root = cli.Get("/");
  ASSERT_TRUE(root);
  EXPECT_NE(root->status, 200);

  auto health = cli.Get("/api/health");
  ASSERT_TRUE(health);
  EXPECT_EQ(health->status, 200);
}

} // namespace
