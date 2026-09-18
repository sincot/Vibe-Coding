// 改密与权限检查单元测试（M1.3）。
//
// 使用 /tmp 下的隔离临时数据库，不触碰正式数据库。覆盖：
//   - 改密新密码校验（复用注册密码规则 + 新密码不得与旧密码相同 + 不裁剪）
//   - 首次改密检查（requires_password_change）
//   - 管理员权限检查（check_admin：登录/角色/改密组合，基于数据库当前值）
//   - 改密服务：成功（哈希更新 + reset_pwd_flag 清除 + 旧密码失效新密码可验证）、
//     错误旧密码、用户不存在、失败路径不产生部分更新、并发改密不互相覆盖
//
// 运行方式：ctest --test-dir build -R password_unit --output-on-failure
// 或直接执行 build/oj_password_test。

#include <sys/types.h>
#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "auth/authorize.h"
#include "auth/context.h"
#include "auth/password.h"
#include "auth/password_change.h"
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

oj::auth::AuthUser make_user(const std::string &role, int reset_flag,
                             std::int64_t id = 1) {
  oj::auth::AuthUser u;
  u.id = id;
  u.account = (role == "admin" ? "admin" : "1234567890");
  u.nickname = (role == "admin" ? "admin" : "alice");
  u.role = role;
  u.reset_pwd_flag = reset_flag;
  return u;
}

// 直接创建用户并返回其 id（用于改密服务测试）。
std::int64_t create_user(oj::Database &db, const std::string &account,
                         const std::string &nickname,
                         const std::string &password) {
  std::string hash;
  std::string err;
  if (!oj::auth::hash_password(password, hash, err)) {
    return -1;
  }
  oj::UserStore store(db);
  oj::UserRecord rec;
  if (store.create(account, nickname, hash, rec, err) !=
      oj::UserStore::CreateStatus::Success) {
    return -1;
  }
  return rec.id;
}

bool read_user_hash_and_flag(oj::Database &db, std::int64_t id,
                             std::string &hash, int &flag) {
  std::string err;
  oj::Statement stmt;
  if (!db.prepare(
          "SELECT password_hash, reset_pwd_flag FROM users WHERE id = ?", stmt,
          err)) {
    return false;
  }
  stmt.bind(1, static_cast<sqlite3_int64>(id));
  if (stmt.step() != SQLITE_ROW) {
    return false;
  }
  hash = stmt.column_text(0);
  flag = stmt.column_int(1);
  return true;
}

void test_validate_password_change() {
  std::cout << "改密新密码校验（复用注册规则 + 不得与旧密码相同）\n";
  std::string err;

  check(!oj::auth::validate_password_change("oldpw", "", err), "空新密码拒绝");
  check(!oj::auth::validate_password_change("oldpw", std::string(129, 'p'),
                                            err),
        "超长新密码拒绝");
  check(!oj::auth::validate_password_change("same", "same", err),
        "新密码与旧密码相同拒绝");
  check(oj::auth::validate_password_change("oldpw", " newpw ", err),
        "含空白新密码允许且不裁剪");
  check(oj::auth::validate_password_change("oldpw", "newpw", err),
        "正常新密码通过");
}

void test_requires_password_change() {
  std::cout << "首次改密检查（requires_password_change）\n";
  check(!oj::auth::requires_password_change(make_user("user", 0)),
        "普通用户无需改密");
  check(!oj::auth::requires_password_change(make_user("admin", 0)),
        "已完成改密管理员无需改密");
  check(oj::auth::requires_password_change(make_user("admin", 1)),
        "未改密管理员需先改密");
}

void test_check_admin() {
  std::cout << "管理员权限检查（角色 + 首次改密组合）\n";
  using oj::auth::AdminCheck;

  check(oj::auth::check_admin(make_user("admin", 0)) == AdminCheck::Ok,
        "已完成改密管理员 -> Ok");
  check(oj::auth::check_admin(make_user("admin", 1)) ==
            AdminCheck::PasswordChangeRequired,
        "未改密管理员 -> 必须先改密");
  check(oj::auth::check_admin(make_user("user", 0)) == AdminCheck::NotAdmin,
        "普通用户 -> NotAdmin");
  // 角色优先：即使普通用户 reset_pwd_flag 为 1（异常状态），也按非管理员拒绝。
  check(oj::auth::check_admin(make_user("user", 1)) == AdminCheck::NotAdmin,
        "普通用户（flag=1）仍为 NotAdmin");
}

void test_change_password_success() {
  std::cout << "改密服务：成功更新哈希并清除首次改密标记\n";
  TempDir dir("pw_ok");
  auto db = open_test_db(dir.db_path());
  check(db != nullptr, "打开测试库成功");

  std::int64_t id = create_user(*db, "1000000000", "alice", "OldPass123");
  check(id > 0, "创建用户成功");

  // 模拟「已设置首次改密标记」的普通场景之外，先置标记为 1 验证被清除。
  std::string err;
  db->exec("UPDATE users SET reset_pwd_flag = 1 WHERE id = " +
               std::to_string(id),
           err);

  oj::auth::ChangePasswordService svc(*db);
  auto r = svc.change_password(id, "OldPass123", "NewPass456");
  check(r.outcome == oj::auth::ChangePasswordService::Outcome::Success,
        "改密成功");

  std::string hash;
  int flag = -1;
  check(read_user_hash_and_flag(*db, id, hash, flag), "读取用户状态成功");
  check(flag == 0, "reset_pwd_flag 已清除为 0");
  check(oj::auth::verify_password(hash, "NewPass456", err), "新密码可验证");
  check(!oj::auth::verify_password(hash, "OldPass123", err), "旧密码不再有效");
}

void test_change_password_wrong_old() {
  std::cout << "改密服务：错误旧密码被拒，哈希与标记不变\n";
  TempDir dir("pw_wrong");
  auto db = open_test_db(dir.db_path());
  std::int64_t id = create_user(*db, "1000000001", "bob", "RealPass1");
  std::string err;
  db->exec("UPDATE users SET reset_pwd_flag = 1 WHERE id = " +
               std::to_string(id),
           err);

  std::string before_hash;
  int before_flag = -1;
  read_user_hash_and_flag(*db, id, before_hash, before_flag);

  oj::auth::ChangePasswordService svc(*db);
  auto r = svc.change_password(id, "WrongPass9", "NewPass456");
  check(r.outcome == oj::auth::ChangePasswordService::Outcome::InvalidOldPassword,
        "返回旧密码错误");

  std::string after_hash;
  int after_flag = -1;
  read_user_hash_and_flag(*db, id, after_hash, after_flag);
  check(after_hash == before_hash, "哈希未被修改");
  check(after_flag == before_flag && after_flag == 1, "首次改密标记未变");
}

void test_change_password_user_not_found() {
  std::cout << "改密服务：用户不存在返回 UserNotFound\n";
  TempDir dir("pw_nouser");
  auto db = open_test_db(dir.db_path());
  oj::auth::ChangePasswordService svc(*db);
  auto r = svc.change_password(999999, "OldPass", "NewPass");
  check(r.outcome == oj::auth::ChangePasswordService::Outcome::UserNotFound,
        "返回 UserNotFound");
}

void test_change_password_concurrent() {
  std::cout << "改密服务：并发改密不互相覆盖\n";
  TempDir dir("pw_race");
  auto db = open_test_db(dir.db_path());
  std::int64_t id = create_user(*db, "1000000002", "carol", "SharedOld1");

  // 多个线程并发使用同一旧密码发起改密，各自使用不同新密码。
  const int kThreads = 8;
  std::atomic<int> success{0};
  std::atomic<int> invalid_old{0};
  std::atomic<int> other{0};
  std::vector<std::thread> threads;
  for (int i = 0; i < kThreads; ++i) {
    threads.emplace_back([&, i]() {
      oj::auth::ChangePasswordService svc(*db);
      auto r = svc.change_password(id, "SharedOld1",
                                   "NewPw" + std::to_string(i));
      if (r.outcome == oj::auth::ChangePasswordService::Outcome::Success) {
        ++success;
      } else if (r.outcome ==
                 oj::auth::ChangePasswordService::Outcome::InvalidOldPassword) {
        ++invalid_old;
      } else {
        ++other;
      }
    });
  }
  for (auto &t : threads) {
    t.join();
  }

  check(success.load() == 1, "恰好一个改密成功");
  check(invalid_old.load() == kThreads - 1, "其余均因旧密码失效被拒");
  check(other.load() == 0, "无其它异常结果");

  // 最终哈希应为某一个新密码（成功者），旧密码不再有效。
  std::string hash;
  int flag = -1;
  check(read_user_hash_and_flag(*db, id, hash, flag), "读取最终状态成功");
  check(flag == 0, "首次改密标记被清除");
  std::string err;
  check(!oj::auth::verify_password(hash, "SharedOld1", err), "旧密码不再有效");
  bool any_new_valid = false;
  for (int i = 0; i < kThreads; ++i) {
    if (oj::auth::verify_password(hash, "NewPw" + std::to_string(i), err)) {
      any_new_valid = true;
    }
  }
  check(any_new_valid, "最终哈希对应某一个成功的新密码");
}

} // namespace

int main() {
  test_validate_password_change();
  test_requires_password_change();
  test_check_admin();
  test_change_password_success();
  test_change_password_wrong_old();
  test_change_password_user_not_found();
  test_change_password_concurrent();

  std::cout << "\n";
  if (g_failures == 0) {
    std::cout << "全部改密与权限单元测试通过\n";
    return 0;
  }
  std::cout << g_failures << " 项失败\n";
  return 1;
}
