#include "db/database.h"

#include <filesystem>
#include <utility>

namespace oj {

// ---------------------------------------------------------------------------
// Statement
// ---------------------------------------------------------------------------

Statement::Statement(sqlite3_stmt *stmt) : stmt_(stmt) {}

Statement::~Statement() {
  if (stmt_ != nullptr) {
    sqlite3_finalize(stmt_);
    stmt_ = nullptr;
  }
}

Statement::Statement(Statement &&other) noexcept : stmt_(other.stmt_) {
  other.stmt_ = nullptr;
}

Statement &Statement::operator=(Statement &&other) noexcept {
  if (this != &other) {
    if (stmt_ != nullptr) {
      sqlite3_finalize(stmt_);
    }
    stmt_ = other.stmt_;
    other.stmt_ = nullptr;
  }
  return *this;
}

bool Statement::bind(int index, int value) {
  return sqlite3_bind_int(stmt_, index, value) == SQLITE_OK;
}

bool Statement::bind(int index, sqlite3_int64 value) {
  return sqlite3_bind_int64(stmt_, index, value) == SQLITE_OK;
}

bool Statement::bind(int index, const std::string &value) {
  return sqlite3_bind_text(stmt_, index, value.data(),
                           static_cast<int>(value.size()), SQLITE_TRANSIENT) ==
         SQLITE_OK;
}

bool Statement::bind(int index, const char *value) {
  return sqlite3_bind_text(stmt_, index, value, -1, SQLITE_TRANSIENT) ==
         SQLITE_OK;
}

bool Statement::bind_null(int index) {
  return sqlite3_bind_null(stmt_, index) == SQLITE_OK;
}

int Statement::step() {
  return sqlite3_step(stmt_);
}

bool Statement::reset() {
  return sqlite3_reset(stmt_) == SQLITE_OK;
}

int Statement::column_int(int column) const {
  return sqlite3_column_int(stmt_, column);
}

sqlite3_int64 Statement::column_int64(int column) const {
  return sqlite3_column_int64(stmt_, column);
}

std::string Statement::column_text(int column) const {
  const unsigned char *text = sqlite3_column_text(stmt_, column);
  if (text == nullptr) {
    return {};
  }
  return std::string(reinterpret_cast<const char *>(text));
}

bool Statement::column_is_null(int column) const {
  return sqlite3_column_type(stmt_, column) == SQLITE_NULL;
}

std::string Statement::errmsg() const {
  sqlite3 *db = sqlite3_db_handle(stmt_);
  return db != nullptr ? sqlite3_errmsg(db) : "未知语句错误";
}

// ---------------------------------------------------------------------------
// Database
// ---------------------------------------------------------------------------

Database::Database(sqlite3 *db, std::string path)
    : db_(db), path_(std::move(path)) {}

Database::~Database() {
  close();
}

std::unique_ptr<Database> Database::open(const std::string &path,
                                         std::string &error) {
  // 首次启动时自动创建数据目录。
  std::filesystem::path fs_path(path);
  std::filesystem::path parent = fs_path.parent_path();
  if (!parent.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(parent, ec);
    if (ec) {
      error = "无法创建数据目录 \"" + parent.string() + "\": " + ec.message();
      return nullptr;
    }
  }

  sqlite3 *db = nullptr;
  int rc = sqlite3_open_v2(path.c_str(), &db,
                           SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                               SQLITE_OPEN_FULLMUTEX,
                           nullptr);
  if (rc != SQLITE_OK) {
    error = "无法打开数据库 \"" + path + "\": " +
            (db != nullptr ? sqlite3_errmsg(db) : "未知错误");
    if (db != nullptr) {
      sqlite3_close_v2(db);
    }
    return nullptr;
  }

  auto database = std::unique_ptr<Database>(new Database(db, path));
  if (!database->configure(error)) {
    return nullptr; // unique_ptr 析构自动关闭连接
  }
  return database;
}

bool Database::configure(std::string &error) {
  if (!exec("PRAGMA journal_mode = WAL;", error)) {
    return false;
  }
  if (!exec("PRAGMA synchronous = NORMAL;", error)) {
    return false;
  }
  if (!exec("PRAGMA foreign_keys = ON;", error)) {
    return false;
  }
  if (!exec("PRAGMA busy_timeout = 5000;", error)) {
    return false;
  }
  return true;
}

bool Database::exec(const std::string &sql, std::string &error) {
  char *errmsg = nullptr;
  int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &errmsg);
  if (rc != SQLITE_OK) {
    error = (errmsg != nullptr) ? errmsg : sqlite3_errmsg(db_);
    sqlite3_free(errmsg);
    return false;
  }
  return true;
}

bool Database::prepare(const std::string &sql, Statement &stmt,
                       std::string &error) {
  sqlite3_stmt *raw = nullptr;
  int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &raw, nullptr);
  if (rc != SQLITE_OK) {
    error = sqlite3_errmsg(db_);
    return false;
  }
  stmt = Statement(raw);
  return true;
}

bool Database::begin(std::string &error) {
  return exec("BEGIN IMMEDIATE;", error);
}

bool Database::commit(std::string &error) {
  return exec("COMMIT;", error);
}

bool Database::rollback(std::string &error) {
  return exec("ROLLBACK;", error);
}

void Database::close() {
  if (db_ != nullptr) {
    sqlite3_close_v2(db_);
    db_ = nullptr;
  }
}

} // namespace oj
