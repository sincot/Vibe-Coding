#pragma once

#include <string>

namespace oj {
namespace auth {

// 10 位纯数字账号生成器。抽象为接口，便于测试时注入确定性序列以模拟账号碰撞。
class AccountGenerator {
public:
  virtual ~AccountGenerator() = default;
  virtual std::string generate() = 0;
};

// 默认实现：使用密码学安全的随机源生成 [0000000000, 9999999999] 之间的
// 10 位纯数字账号（前导零允许，账号以字符串存储/返回）。
//
// 注意：随机只用于降低碰撞概率；账号的「唯一、不复用」最终由数据库
// account 唯一性约束保证（碰撞时由注册服务有限次重试）。
class RandomAccountGenerator : public AccountGenerator {
public:
  std::string generate() override;
};

} // namespace auth
} // namespace oj
