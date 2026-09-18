#pragma once

#include <memory>
#include <string>

#include <sqlite3.h>

namespace oj {

// RAII 封装 sqlite3_stmt，析构时自动 finalize，避免语句对象泄漏。
//
// step() 返回 sqlite3_step 的原始结果码，调用方据此区分：
//   SQLITE_ROW  —— 存在一行结果
//   SQLITE_DONE —— 语句执行完成（无更多行）
//   其它        —— 出错，可用 errmsg() 获取错误信息
class Statement {
public:
  Statement() = default;
  ~Statement();

  Statement(Statement &&other) noexcept;
  Statement &operator=(Statement &&other) noexcept;
  Statement(const Statement &) = delete;
  Statement &operator=(const Statement &) = delete;

  sqlite3_stmt *get() const { return stmt_; }

  // 参数绑定（索引从 1 开始，符合 SQLite 约定）。失败返回 false。
  bool bind(int index, int value);
  bool bind(int index, sqlite3_int64 value);
  bool bind(int index, const std::string &value);
  bool bind(int index, const char *value);
  bool bind_null(int index);

  // 执行一步，返回 sqlite3_step 的原始结果码。
  int step();

  // 重置语句到初始状态（清除绑定），以便复用。失败返回 false。
  bool reset();

  // 结果列读取（列索引从 0 开始）。
  int column_int(int column) const;
  sqlite3_int64 column_int64(int column) const;
  std::string column_text(int column) const;
  bool column_is_null(int column) const;

  std::string errmsg() const;

private:
  explicit Statement(sqlite3_stmt *stmt);
  friend class Database;
  sqlite3_stmt *stmt_ = nullptr;
};

// RAII 封装 sqlite3 连接。
//
// 连接以 SQLITE_OPEN_FULLMUTEX（serialized 串行化）模式打开，因此单个连接
// 可以被多个线程安全共享（SQLite 内部完成串行化）。服务进程在生命周期内持有
// 单个长连接即可，无需在每次 HTTP 请求时新建/共享连接。教学班规模的写入压力
// 下，串行化带来的开销可忽略；如后续出现明显争用，可再扩展为连接池。
class Database {
public:
  Database(const Database &) = delete;
  Database &operator=(const Database &) = delete;

  ~Database();

  // 打开（或创建）数据库并完成连接级配置：
  //   - 自动创建父目录（首次启动）
  //   - PRAGMA journal_mode=WAL
  //   - PRAGMA synchronous=NORMAL（WAL 下的推荐配置）
  //   - PRAGMA foreign_keys=ON（外键检查）
  //   - PRAGMA busy_timeout=5000（锁等待 5s，避免并发写时立即报错）
  // 失败返回 nullptr，并通过 error 给出原因。
  static std::unique_ptr<Database> open(const std::string &path,
                                        std::string &error);

  sqlite3 *handle() const { return db_; }
  const std::string &path() const { return path_; }

  // 执行不含参数的 SQL（可包含多条语句）。成功返回 true。
  bool exec(const std::string &sql, std::string &error);

  // 准备一条语句（供参数绑定）。成功返回 true 并填充 stmt。
  bool prepare(const std::string &sql, Statement &stmt, std::string &error);

  // 事务控制。
  bool begin(std::string &error);
  bool commit(std::string &error);
  bool rollback(std::string &error);

  // 主动关闭连接（幂等）；析构时也会自动关闭。
  void close();

private:
  Database(sqlite3 *db, std::string path);

  // 连接级配置，open() 内部调用。
  bool configure(std::string &error);

  sqlite3 *db_ = nullptr;
  std::string path_;
};

} // namespace oj
