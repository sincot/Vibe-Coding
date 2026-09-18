// 注册核心逻辑单元测试（M1.1）。
//
// 使用 /tmp 下的隔离临时数据库，不触碰正式数据库。覆盖：
//   - 昵称 / 密码输入规则（空值、空白处理、长度、不裁剪密码）
//   - 账号生成器输出格式（10 位纯数字、前导零允许）
//   - 注册服务：成功、昵称冲突、账号碰撞重试、重试耗尽、非法输入、持久化
//   - 密码哈希（argon2id）正确性验证
//
// 运行方式：ctest --test-dir build -R register_unit --output-on-failure
// 或直接执行 build/oj_register_test。

#include <sys/types.h>
#include <unistd.h>

#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "auth/account.h"
#include "auth/password.h"
#include "auth/register.h"
#include "auth/validation.h"
#include "db/database.h"
#include "db/schema.h"
#include "db/users.h"

namespace {

int g_failures = 0;

void check(bool cond, const std::string &msg) {
  if (cond) {
    std::cout << "  [PASS] " << msg << "\n";
  } else {
    std::cout << "  [FAIL] " << msg << "\n";
    ++g_failures;
  }
}

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

// 确定性账号生成器：按脚本序列返回账号，耗尽后重复最后一个。
class FakeAccountGenerator : public oj::auth::AccountGenerator {
public:
  explicit FakeAccountGenerator(std::vector<std::string> seq)
      : seq_(std::move(seq)) {}
  std::string generate() override {
    if (seq_.empty()) {
      return "0000000000";
    }
    std::size_t idx = idx_ < seq_.size() ? idx_ : seq_.size() - 1;
    ++idx_;
    return seq_[idx];
  }

private:
  std::vector<std::string> seq_;
  std::size_t idx_ = 0;
};

std::unique_ptr<oj::Database> open_test_db(const std::string &path) {
  std::string err;
  auto db = oj::Database::open(path, err);
  if (!db) {
    return nullptr;
  }
  if (!oj::initialize_schema(*db, std::string("AdminSecret123!"), err)) {
    return nullptr;
  }
  return db;
}

int count_users_by_nickname(oj::Database &db, const std::string &nickname) {
  std::string err;
  oj::Statement stmt;
  if (!db.prepare("SELECT COUNT(*) FROM users WHERE nickname = ?", stmt, err)) {
    return -1;
  }
  stmt.bind(1, nickname);
  if (stmt.step() != SQLITE_ROW) {
    return -1;
  }
  return stmt.column_int(0);
}

void test_validate_nickname() {
  std::cout << "昵称校验规则\n";
  std::string normalized, err;

  check(!oj::auth::validate_nickname("", normalized, err), "空昵称被拒绝");
  check(!oj::auth::validate_nickname("   \t  ", normalized, err),
        "纯空白昵称被拒绝");

  check(oj::auth::validate_nickname("alice", normalized, err),
        "正常昵称通过");
  check(normalized == "alice", "昵称不被改写");

  check(oj::auth::validate_nickname("  alice  ", normalized, err),
        "首尾空白被去除");
  check(normalized == "alice", "去除首尾空白后正确");

  check(oj::auth::validate_nickname("a b c", normalized, err),
        "内部空白保留");
  check(normalized == "a b c", "内部空白保留正确");

  check(!oj::auth::validate_nickname(std::string(31, 'a'), normalized, err),
        "31 字符昵称被拒绝");
  check(oj::auth::validate_nickname(std::string(30, 'a'), normalized, err),
        "30 字符昵称通过");
}

void test_validate_password() {
  std::cout << "密码校验规则\n";
  std::string err;

  check(!oj::auth::validate_password("", err), "空密码被拒绝");

  std::string ws_only = "   ";
  check(oj::auth::validate_password(ws_only, err), "纯空白密码允许（不裁剪）");

  check(!oj::auth::validate_password(std::string(129, 'p'), err),
        "129 字符密码被拒绝");
  check(oj::auth::validate_password(std::string(128, 'p'), err),
        "128 字符密码通过");

  std::string with_space = " ab cd ";
  check(oj::auth::validate_password(with_space, err), "含空白密码允许且不裁剪");
}

void test_random_account_format() {
  std::cout << "账号生成器输出格式\n";
  oj::auth::RandomAccountGenerator gen;
  for (int i = 0; i < 20; ++i) {
    std::string account = gen.generate();
    check(account.size() == 10, "账号长度恒为 10");
    bool all_digits = true;
    for (char c : account) {
      if (c < '0' || c > '9') {
        all_digits = false;
        break;
      }
    }
    check(all_digits, "账号为纯数字");
  }
  // 前导零允许：格式化为 "%010llu"，值不足 10 位时以前导零补齐。
}

void test_register_success() {
  std::cout << "注册成功：记录与响应一致，密码哈希可验证\n";
  TempDir dir("reg_success");
  auto db = open_test_db(dir.db_path());
  check(db != nullptr, "打开测试库成功");

  FakeAccountGenerator gen({"1234567890"});
  oj::auth::RegisterService svc(*db, gen);
  auto outcome = svc.register_user("alice", "Secret123");
  check(outcome.kind == oj::auth::RegisterOutcome::Kind::Success, "注册成功");
  check(outcome.user.account == "1234567890", "返回账号与生成器一致");
  check(outcome.user.nickname == "alice", "昵称正确");
  check(outcome.user.role == "user", "角色为普通用户");
  check(outcome.user.reset_pwd_flag == 0, "首次改密标记为 0");

  check(outcome.user.password_hash.find("$argon2id$") == 0, "哈希为 argon2id");
  std::string verr;
  check(oj::auth::verify_password(outcome.user.password_hash, "Secret123",
                                  verr),
        "正确密码验证通过");
  check(!oj::auth::verify_password(outcome.user.password_hash, "WrongPass",
                                   verr),
        "错误密码验证失败");

  oj::UserStore store(*db);
  bool found = false;
  oj::UserRecord rec;
  check(store.find_by_account("1234567890", found, rec, verr) && found,
        "账号可查询到");
  check(rec.nickname == "alice" && rec.role == "user",
        "数据库记录与响应一致");
}

void test_register_nickname_conflict() {
  std::cout << "重复昵称被拒绝，不产生多余记录\n";
  TempDir dir("reg_conflict");
  auto db = open_test_db(dir.db_path());

  FakeAccountGenerator gen({"1000000000", "1000000001"});
  oj::auth::RegisterService svc(*db, gen);
  check(svc.register_user("bob", "Pw1").kind ==
            oj::auth::RegisterOutcome::Kind::Success,
        "首次注册成功");
  auto second = svc.register_user("bob", "Pw2");
  check(second.kind == oj::auth::RegisterOutcome::Kind::NicknameTaken,
        "重复昵称返回 NicknameTaken");
  check(count_users_by_nickname(*db, "bob") == 1, "昵称 bob 仅一条记录");
}

void test_register_account_collision_retry() {
  std::cout << "账号碰撞：换号重试后成功\n";
  TempDir dir("reg_collision");
  auto db = open_test_db(dir.db_path());

  oj::UserStore store(*db);
  oj::UserRecord dummy;
  std::string err;
  check(store.create("1000000000", "existing", "x", dummy, err) ==
            oj::UserStore::CreateStatus::Success,
        "预置占用账号 1000000000");

  FakeAccountGenerator gen({"1000000000", "1000000001"});
  oj::auth::RegisterService svc(*db, gen);
  auto outcome = svc.register_user("newuser", "pw123456");
  check(outcome.kind == oj::auth::RegisterOutcome::Kind::Success,
        "碰撞后重试成功");
  check(outcome.user.account == "1000000001", "成功分配到下一账号");
  check(count_users_by_nickname(*db, "newuser") == 1, "newuser 已创建");
}

void test_register_account_exhaustion() {
  std::cout << "账号碰撞：重试次数耗尽后报内部错误，不留部分数据\n";
  TempDir dir("reg_exhaust");
  auto db = open_test_db(dir.db_path());

  oj::UserStore store(*db);
  oj::UserRecord dummy;
  std::string err;
  check(store.create("1000000000", "existing", "x", dummy, err) ==
            oj::UserStore::CreateStatus::Success,
        "预置占用账号 1000000000");

  FakeAccountGenerator gen({"1000000000"}); // 永远碰撞
  oj::auth::RegisterService svc(*db, gen, /*max_account_attempts=*/3);
  auto outcome = svc.register_user("newuser", "pw123456");
  check(outcome.kind == oj::auth::RegisterOutcome::Kind::InternalError,
        "重试耗尽返回 InternalError");
  check(count_users_by_nickname(*db, "newuser") == 0,
        "未留下 newuser 的部分数据");
}

void test_register_invalid_inputs() {
  std::cout << "非法输入被拒绝\n";
  TempDir dir("reg_invalid");
  auto db = open_test_db(dir.db_path());
  FakeAccountGenerator gen({"1000000000"});
  oj::auth::RegisterService svc(*db, gen);

  check(svc.register_user("", "pw").kind ==
            oj::auth::RegisterOutcome::Kind::InvalidNickname,
        "空昵称 -> InvalidNickname");
  check(svc.register_user("   ", "pw").kind ==
            oj::auth::RegisterOutcome::Kind::InvalidNickname,
        "纯空白昵称 -> InvalidNickname");
  check(svc.register_user(std::string(31, 'a'), "pw").kind ==
            oj::auth::RegisterOutcome::Kind::InvalidNickname,
        "超长昵称 -> InvalidNickname");

  check(svc.register_user("nick", "").kind ==
            oj::auth::RegisterOutcome::Kind::InvalidPassword,
        "空密码 -> InvalidPassword");
  check(svc.register_user("nick", std::string(129, 'p')).kind ==
            oj::auth::RegisterOutcome::Kind::InvalidPassword,
        "超长密码 -> InvalidPassword");
}

void test_register_persistence() {
  std::cout << "关闭并重新打开后用户保留，账号不复用\n";
  TempDir dir("reg_persist");
  std::string path = dir.db_path();

  {
    auto db = open_test_db(path);
    check(db != nullptr, "首次打开成功");
    FakeAccountGenerator gen({"2000000000"});
    oj::auth::RegisterService svc(*db, gen);
    check(svc.register_user("carol", "Pw1").kind ==
              oj::auth::RegisterOutcome::Kind::Success,
          "注册成功");
    db->close();
  }

  {
    auto db = open_test_db(path);
    check(db != nullptr, "重新打开成功");
    oj::UserStore store(*db);
    bool found = false;
    oj::UserRecord rec;
    std::string err;
    check(store.find_by_account("2000000000", found, rec, err) && found,
          "用户记录仍在");
    check(rec.nickname == "carol", "昵称一致");

    // 再次注册，生成器给同一账号 -> 会被视为碰撞并换号，而非复用旧账号。
    FakeAccountGenerator gen({"2000000000", "2000000001"});
    oj::auth::RegisterService svc(*db, gen);
    auto outcome = svc.register_user("dave", "Pw2");
    check(outcome.kind == oj::auth::RegisterOutcome::Kind::Success,
          "再次注册成功");
    check(outcome.user.account != "2000000000", "已有账号未被复用");
  }
}

} // namespace

int main() {
  test_validate_nickname();
  test_validate_password();
  test_random_account_format();
  test_register_success();
  test_register_nickname_conflict();
  test_register_account_collision_retry();
  test_register_account_exhaustion();
  test_register_invalid_inputs();
  test_register_persistence();

  std::cout << "\n";
  if (g_failures == 0) {
    std::cout << "全部单元测试通过\n";
    return 0;
  }
  std::cout << g_failures << " 项失败\n";
  return 1;
}
