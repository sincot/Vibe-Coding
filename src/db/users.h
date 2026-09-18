#pragma once

#include <cstdint>
#include <string>

namespace oj {

class Database;

// 用户记录的完整字段（含密码哈希）。password_hash 仅在服务端内部使用，
// 绝不通过 HTTP 响应暴露给外部。
struct UserRecord {
  std::int64_t id = 0;
  std::string account;
  std::string nickname;
  std::string password_hash;
  std::string role;
  int reset_pwd_flag = 0;
  std::string created_at;
};

// 用户数据访问封装：按账号 / 昵称查询，以及创建用户。
//
// 所有查询/写入均使用参数绑定，不拼接外部输入；数据库错误通过返回值与
// error 输出参数区分，调用方决定如何向外部呈现（不暴露 SQL 细节）。
class UserStore {
public:
  explicit UserStore(Database &db) : db_(db) {}

  // 按账号查询。返回 true 表示查询过程正常（无论是否命中），found 指示是否存在；
  // 命中时填充 out（含 password_hash，供登录密码验证使用）。
  // 返回 false 表示数据库错误，error 非空。
  bool find_by_account(const std::string &account, bool &found, UserRecord &out,
                       std::string &error);

  // 按用户 ID（主键）查询，语义同上。供 token 身份校验后加载当前用户信息。
  bool find_by_id(std::int64_t id, bool &found, UserRecord &out,
                  std::string &error);

  // 按昵称查询，语义同上。
  bool find_by_nickname(const std::string &nickname, bool &found,
                        UserRecord &out, std::string &error);

  enum class CreateStatus {
    Success,       // 创建成功，out 填充完整记录
    NicknameTaken, // 昵称唯一性冲突
    AccountTaken,  // 账号唯一性冲突（随机账号碰撞）
    Error,         // 其它数据库错误，error 非空
  };

  // 创建普通用户。账号、角色（固定 'user'）与首次改密标记（固定 0）由后端控制，
  // 调用方仅提供账号、昵称与已哈希的密码。账号/昵称唯一性最终由数据库约束保证；
  // 冲突类型通过返回值精确区分，供调用方决定重试（账号碰撞）还是直接返回
  // （昵称冲突）。
  CreateStatus create(const std::string &account, const std::string &nickname,
                      const std::string &password_hash, UserRecord &out,
                      std::string &error);

  enum class UpdatePasswordStatus {
    Success,  // 更新成功（密码哈希已替换，首次改密标记已清除）
    NotFound, // 目标用户不存在
    Error,    // 其它数据库错误，error 非空
  };

  // 原子更新指定用户的密码哈希并清除首次改密标记（单条 UPDATE，两字段同时生效）。
  // 仅更新密码哈希与标记，不读取也不校验旧密码——旧密码校验由调用方在事务内完成。
  UpdatePasswordStatus update_password(std::int64_t id,
                                       const std::string &new_hash,
                                       std::string &error);

private:
  Database &db_;
};

} // namespace oj
