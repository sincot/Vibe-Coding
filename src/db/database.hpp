#pragma once

#include <sqlite3.h>

#include <mutex>
#include <string>

namespace oj {

// SQLite 连接封装：自动建目录/文件、开启 WAL、外键检查、busy_timeout、幂等建表。
//
// 单连接 + 全局互斥锁：所有公共方法在锁内串行访问，供多线程 HTTP 层安全共用。
class Database {
 public:
  Database() = default;
  ~Database();

  Database(const Database&) = delete;
  Database& operator=(const Database&) = delete;

  // 锁定数据库（HTTP 处理器在多线程下访问时使用）。锁内可安全执行 sqlite3 调用。
  std::unique_lock<std::mutex> Lock() { return std::unique_lock<std::mutex>(mutex_); }

  // 打开数据库（父目录与文件不存在时自动创建），并设置：
  //   journal_mode=WAL、foreign_keys=ON、busy_timeout、synchronous=NORMAL。
  // 失败返回 false 且 err 给出原因（WAL 未生效、目录/文件不可写等）。
  bool Open(const std::string& path, int busy_timeout_ms, std::string* err);

  // 幂等建表。表已存在且结构一致时不改动；结构不兼容（缺列）时明确报错，不自动迁移、
  // 不删除重建。
  bool InitSchema(std::string* err);

  // 确保 admin 账号存在：不存在则用 argon2id 存密码哈希并置 reset_pwd_flag=1。
  // 已存在时不做任何修改（不重置密码/角色/改密标记）。
  // 返回 1=新建，0=已存在，-1=失败（err 说明原因，如未提供初始密码、账号/昵称冲突）。
  int EnsureAdmin(const std::string& account, const std::string& nickname,
                  const std::string& plain_password, std::string* err);

  void Close();
  bool IsOpen() const { return db_ != nullptr; }

  sqlite3* raw() const { return db_; }

 private:
  int Exec(const char* sql, std::string* err);
  bool VerifySchemaColumns(std::string* err);

  sqlite3* db_ = nullptr;
  std::string path_;
  mutable std::mutex mutex_;
};

}  // namespace oj