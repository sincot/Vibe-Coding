// 配置管理单元测试（基于 gtest）。
//
// 覆盖：
//   - Config 默认值（监听地址 / 端口 / 数据库路径）
//   - parse_port 端口校验（边界 1/65535、非法字符、越界、溢出、空白、前导零）
//   - parse_args 命令行参数解析（默认值、--host/--port/--db、--help/-h 及短路、
//     缺少/空参数、未知参数、组合参数、后值覆盖、错误信息）
//   - read_admin_password 环境变量读取（未设置 / 已设置 / 空串）
//   - load_jwt_config JWT 配置读取与校验（缺失/空/过短密钥、合法密钥、
//     默认/自定义有效期、非法有效期、密钥与有效期边界）
//
// 运行方式：ctest --test-dir build -R config_unit --output-on-failure
// 或直接执行 build/oj_config_test。

#include <gtest/gtest.h>

#include <cstdlib>
#include <string>
#include <vector>

#include "auth/jwt.h"
#include "config.h"

namespace {

using oj::config::Config;
using oj::config::parse_args;
using oj::config::parse_port;
using oj::config::read_admin_password;

// 从字符串列表构造 argv 风格的参数数组（argv[0] 占位为程序名）。
class Argv {
public:
  explicit Argv(std::initializer_list<std::string> args) {
    storage_.push_back("oj_server");
    for (const auto &a : args) {
      storage_.push_back(a);
    }
    for (auto &s : storage_) {
      ptrs_.push_back(s.data());
    }
  }

  int argc() const { return static_cast<int>(ptrs_.size()); }
  char **argv() { return ptrs_.data(); }

private:
  std::vector<std::string> storage_;
  std::vector<char *> ptrs_;
};

// 设置/取消环境变量并在析构时恢复原状态的 RAII 辅助。
//
// 关键点：getenv 返回的指针可能被后续 setenv/unsetenv 覆盖或失效，因此这里
// 将旧值「拷贝」到 std::string 中保存，避免在析构恢复时使用悬垂指针。
// value == nullptr 表示取消该变量；否则设置为指定值（可为空串 ""）。
class EnvGuard {
public:
  EnvGuard(const std::string &name, const char *value) : name_(name) {
    const char *old = std::getenv(name.c_str());
    if (old != nullptr) {
      has_old_ = true;
      old_value_ = std::string(old); // 拷贝，避免悬垂
    }

    if (value != nullptr) {
      setenv(name.c_str(), value, 1);
    } else {
      unsetenv(name.c_str());
    }
  }

  ~EnvGuard() {
    if (has_old_) {
      setenv(name_.c_str(), old_value_.c_str(), 1);
    } else {
      unsetenv(name_.c_str());
    }
  }

private:
  std::string name_;
  bool has_old_ = false;
  std::string old_value_;
};

} // namespace

// ---------------------------------------------------------------------------
// Config 默认值
// ---------------------------------------------------------------------------

TEST(ConfigDefaults, DefaultValues) {
  Config cfg;
  EXPECT_EQ(cfg.host, "0.0.0.0");
  EXPECT_EQ(cfg.port, 8080);
  EXPECT_EQ(cfg.db_path, "data/oj.db");
  EXPECT_FALSE(cfg.seed) << "默认不导入种子（正常启动不重写题目数据）";
}

// ---------------------------------------------------------------------------
// parse_port
// ---------------------------------------------------------------------------

TEST(ParsePort, AcceptsBoundaryValues) {
  int out = -1;
  EXPECT_TRUE(parse_port("1", out)) << "最小合法端口 1";
  EXPECT_EQ(out, 1);

  EXPECT_TRUE(parse_port("65535", out)) << "最大合法端口 65535";
  EXPECT_EQ(out, 65535);

  EXPECT_TRUE(parse_port("8080", out)) << "常规端口 8080";
  EXPECT_EQ(out, 8080);
}

TEST(ParsePort, AcceptsLeadingZeros) {
  int out = -1;
  EXPECT_TRUE(parse_port("08080", out));
  EXPECT_EQ(out, 8080);
}

TEST(ParsePort, RejectsInvalidValues) {
  const char *bad[] = {"",       "0",       "-1",      "65536",
                       "abc",    "12a",     "1.5",     "+8080",
                       " 8080",  "8080 ",   "08080x",  "99999999999999999999"};
  for (const char *text : bad) {
    int out = 12345;
    EXPECT_FALSE(parse_port(text, out)) << "应拒绝: \"" << text << "\"";
    EXPECT_EQ(out, 12345) << "失败时不应改写输出: \"" << text << "\"";
  }
}

// ---------------------------------------------------------------------------
// parse_args
// ---------------------------------------------------------------------------

TEST(ParseArgs, NoArgsUsesDefaults) {
  Config cfg;
  bool want_help = false;
  std::string err;
  Argv args({});

  EXPECT_TRUE(parse_args(args.argc(), args.argv(), cfg, want_help, err));
  EXPECT_FALSE(want_help);
  EXPECT_EQ(cfg.host, "0.0.0.0");
  EXPECT_EQ(cfg.port, 8080);
  EXPECT_EQ(cfg.db_path, "data/oj.db");
}

TEST(ParseArgs, HelpFlag) {
  for (const char *flag : {"--help", "-h"}) {
    Config cfg;
    bool want_help = false;
    std::string err;
    Argv args({flag});

    EXPECT_TRUE(parse_args(args.argc(), args.argv(), cfg, want_help, err))
        << "应识别: " << flag;
    EXPECT_TRUE(want_help) << "应请求帮助: " << flag;
  }
}

// --help 应短路返回：即使后续存在未知/非法参数，也立即返回并请求帮助。
TEST(ParseArgs, HelpShortCircuitsRemainingArgs) {
  Config cfg;
  bool want_help = false;
  std::string err;
  Argv args({"--help", "--unknown", "--port", "abc"});

  EXPECT_TRUE(parse_args(args.argc(), args.argv(), cfg, want_help, err));
  EXPECT_TRUE(want_help);
  EXPECT_EQ(cfg.port, 8080) << "短路后不应继续解析后续参数";
}

TEST(ParseArgs, HostOption) {
  Config cfg;
  bool want_help = false;
  std::string err;
  Argv args({"--host", "127.0.0.1"});

  EXPECT_TRUE(parse_args(args.argc(), args.argv(), cfg, want_help, err));
  EXPECT_EQ(cfg.host, "127.0.0.1");
  EXPECT_EQ(cfg.port, 8080);
  EXPECT_EQ(cfg.db_path, "data/oj.db");
}

TEST(ParseArgs, PortOption) {
  Config cfg;
  bool want_help = false;
  std::string err;
  Argv args({"--port", "9000"});

  EXPECT_TRUE(parse_args(args.argc(), args.argv(), cfg, want_help, err));
  EXPECT_EQ(cfg.port, 9000);
  EXPECT_EQ(cfg.host, "0.0.0.0");
}

TEST(ParseArgs, DbOption) {
  Config cfg;
  bool want_help = false;
  std::string err;
  Argv args({"--db", "/tmp/oj-test.db"});

  EXPECT_TRUE(parse_args(args.argc(), args.argv(), cfg, want_help, err));
  EXPECT_EQ(cfg.db_path, "/tmp/oj-test.db");
}

TEST(ParseArgs, SeedFlag) {
  Config cfg;
  bool want_help = false;
  std::string err;
  Argv args({"--seed"});

  EXPECT_TRUE(parse_args(args.argc(), args.argv(), cfg, want_help, err));
  EXPECT_TRUE(cfg.seed);
}

TEST(ParseArgs, SeedWithOtherOptions) {
  Config cfg;
  bool want_help = false;
  std::string err;
  Argv args({"--seed", "--db", "/tmp/seed.db", "--port", "9000"});

  EXPECT_TRUE(parse_args(args.argc(), args.argv(), cfg, want_help, err));
  EXPECT_TRUE(cfg.seed);
  EXPECT_EQ(cfg.db_path, "/tmp/seed.db");
  EXPECT_EQ(cfg.port, 9000);
}

TEST(ParseArgs, CombinedOptions) {
  Config cfg;
  bool want_help = false;
  std::string err;
  Argv args({"--host", "127.0.0.1", "--port", "9000", "--db", "/tmp/x.db"});

  EXPECT_TRUE(parse_args(args.argc(), args.argv(), cfg, want_help, err));
  EXPECT_EQ(cfg.host, "127.0.0.1");
  EXPECT_EQ(cfg.port, 9000);
  EXPECT_EQ(cfg.db_path, "/tmp/x.db");
}

TEST(ParseArgs, LastValueWins) {
  Config cfg;
  bool want_help = false;
  std::string err;
  Argv args({"--port", "9000", "--port", "8000"});

  EXPECT_TRUE(parse_args(args.argc(), args.argv(), cfg, want_help, err));
  EXPECT_EQ(cfg.port, 8000);
}

TEST(ParseArgs, MissingValueRejected) {
  for (const char *opt : {"--host", "--port", "--db"}) {
    Config cfg;
    bool want_help = false;
    std::string err;
    Argv args({opt});

    EXPECT_FALSE(parse_args(args.argc(), args.argv(), cfg, want_help, err))
        << "缺少参数应被拒绝: " << opt;
    EXPECT_NE(err.find("需要一个参数"), std::string::npos)
        << "错误信息应说明缺少参数: " << opt;
  }
}

TEST(ParseArgs, EmptyValueRejected) {
  for (const char *opt : {"--host", "--db"}) {
    Config cfg;
    bool want_help = false;
    std::string err;
    Argv args({opt, ""});

    EXPECT_FALSE(parse_args(args.argc(), args.argv(), cfg, want_help, err))
        << "空值应被拒绝: " << opt;
    EXPECT_NE(err.find("不能为空"), std::string::npos)
        << "错误信息应说明参数为空: " << opt;
  }
}

TEST(ParseArgs, InvalidPortValueRejected) {
  for (const char *bad : {"0", "-1", "65536", "abc", "1.5"}) {
    Config cfg;
    bool want_help = false;
    std::string err;
    Argv args({"--port", bad});

    EXPECT_FALSE(parse_args(args.argc(), args.argv(), cfg, want_help, err))
        << "非法端口应被拒绝: " << bad;
    EXPECT_NE(err.find("非法端口"), std::string::npos)
        << "错误信息应说明端口非法: " << bad;
  }
}

TEST(ParseArgs, UnknownOptionRejected) {
  Config cfg;
  bool want_help = false;
  std::string err;
  Argv args({"--unknown"});

  EXPECT_FALSE(parse_args(args.argc(), args.argv(), cfg, want_help, err));
  EXPECT_NE(err.find("未知参数"), std::string::npos);
}

TEST(ParseArgs, UnknownOptionStopsBeforeLaterOptions) {
  Config cfg;
  bool want_help = false;
  std::string err;
  Argv args({"--host", "127.0.0.1", "--unknown", "--port", "9000"});

  EXPECT_FALSE(parse_args(args.argc(), args.argv(), cfg, want_help, err));
  EXPECT_NE(err.find("未知参数"), std::string::npos);
}

// ---------------------------------------------------------------------------
// read_admin_password
// ---------------------------------------------------------------------------

TEST(ReadAdminPassword, UnsetReturnsNullopt) {
  EnvGuard guard("OJ_ADMIN_PASSWORD", nullptr);
  EXPECT_FALSE(read_admin_password().has_value());
}

TEST(ReadAdminPassword, SetReturnsValue) {
  EnvGuard guard("OJ_ADMIN_PASSWORD", "AdminSecret123!");
  auto value = read_admin_password();
  ASSERT_TRUE(value.has_value());
  EXPECT_EQ(*value, "AdminSecret123!");
}

// 空串是「已设置但为空」，与「未设置」不同：应返回 has_value()==true 的空串。
// 下游 initialize_schema 对空串按「未提供密码」处理，此处固化该语义。
TEST(ReadAdminPassword, EmptyStringReturnsEmptyValue) {
  EnvGuard guard("OJ_ADMIN_PASSWORD", "");
  auto value = read_admin_password();
  ASSERT_TRUE(value.has_value());
  EXPECT_EQ(*value, "");
}

// ---------------------------------------------------------------------------
// load_jwt_config
// ---------------------------------------------------------------------------

TEST(LoadJwtConfig, MissingSecretRejected) {
  EnvGuard secret("OJ_JWT_SECRET", nullptr);
  EnvGuard expires("OJ_JWT_EXPIRES_SECONDS", nullptr);

  oj::auth::JwtConfig cfg;
  std::string err;
  EXPECT_FALSE(oj::auth::load_jwt_config(cfg, err));
  EXPECT_NE(err.find("缺少"), std::string::npos);
}

TEST(LoadJwtConfig, EmptySecretRejected) {
  EnvGuard secret("OJ_JWT_SECRET", "");

  oj::auth::JwtConfig cfg;
  std::string err;
  EXPECT_FALSE(oj::auth::load_jwt_config(cfg, err));
}

TEST(LoadJwtConfig, ShortSecretRejected) {
  EnvGuard secret("OJ_JWT_SECRET", "short"); // 长度 5 < kMinJwtSecretLen(16)

  oj::auth::JwtConfig cfg;
  std::string err;
  EXPECT_FALSE(oj::auth::load_jwt_config(cfg, err));
  EXPECT_NE(err.find("长度不足"), std::string::npos);
}

TEST(LoadJwtConfig, SecretLengthBoundary) {
  EnvGuard expires("OJ_JWT_EXPIRES_SECONDS", nullptr);

  {
    EnvGuard secret("OJ_JWT_SECRET", "0123456789abcdef"); // 恰好 16 字节
    oj::auth::JwtConfig cfg;
    std::string err;
    EXPECT_TRUE(oj::auth::load_jwt_config(cfg, err)) << err;
  }

  {
    EnvGuard secret("OJ_JWT_SECRET", "0123456789abcde"); // 15 字节
    oj::auth::JwtConfig cfg;
    std::string err;
    EXPECT_FALSE(oj::auth::load_jwt_config(cfg, err));
  }
}

TEST(LoadJwtConfig, ValidSecretUsesDefaultExpiry) {
  EnvGuard secret("OJ_JWT_SECRET", "a-valid-secret-0123456789");
  EnvGuard expires("OJ_JWT_EXPIRES_SECONDS", nullptr);

  oj::auth::JwtConfig cfg;
  std::string err;
  EXPECT_TRUE(oj::auth::load_jwt_config(cfg, err));
  EXPECT_EQ(cfg.secret, "a-valid-secret-0123456789");
  EXPECT_EQ(cfg.expires_seconds, 3600);
}

TEST(LoadJwtConfig, CustomExpiryAccepted) {
  EnvGuard secret("OJ_JWT_SECRET", "a-valid-secret-0123456789");
  EnvGuard expires("OJ_JWT_EXPIRES_SECONDS", "7200");

  oj::auth::JwtConfig cfg;
  std::string err;
  EXPECT_TRUE(oj::auth::load_jwt_config(cfg, err));
  EXPECT_EQ(cfg.expires_seconds, 7200);
}

TEST(LoadJwtConfig, ExpiryBoundaryAccepted) {
  EnvGuard secret("OJ_JWT_SECRET", "a-valid-secret-0123456789");

  {
    EnvGuard expires("OJ_JWT_EXPIRES_SECONDS", "1");
    oj::auth::JwtConfig cfg;
    std::string err;
    EXPECT_TRUE(oj::auth::load_jwt_config(cfg, err));
    EXPECT_EQ(cfg.expires_seconds, 1);
  }

  {
    EnvGuard expires("OJ_JWT_EXPIRES_SECONDS", "31536000");
    oj::auth::JwtConfig cfg;
    std::string err;
    EXPECT_TRUE(oj::auth::load_jwt_config(cfg, err));
    EXPECT_EQ(cfg.expires_seconds, 31536000);
  }
}

TEST(LoadJwtConfig, InvalidExpiryRejected) {
  const char *bad[] = {"",      "abc",  "0",    "-5",
                       "1.5",   "31536001", "99999999999999999999"};
  for (const char *value : bad) {
    EnvGuard secret("OJ_JWT_SECRET", "a-valid-secret-0123456789");
    EnvGuard expires("OJ_JWT_EXPIRES_SECONDS", value);

    oj::auth::JwtConfig cfg;
    std::string err;
    EXPECT_FALSE(oj::auth::load_jwt_config(cfg, err))
        << "应拒绝有效期: \"" << value << "\"";
  }
}
