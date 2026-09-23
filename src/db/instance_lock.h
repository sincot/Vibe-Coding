#pragma once

#include <memory>
#include <string>

namespace oj {

// 单机单实例互斥锁（M3.7 启动恢复的多实例保护）。
//
// 对数据库旁路锁文件 <db_path>.lock 持有 flock(LOCK_EX|LOCK_NB)，进程存活期间
// 一直持有；进程崩溃（含 SIGKILL/断电）时由操作系统自动释放，无需额外清理，
// 因此「失效占用」天然解除。用于防止两个服务实例同时扫描并恢复同一数据库中的
// 在途任务，也不引入分布式平台。
//
// 注意：仅约束正式服务入口（main）。测试直接构造 HttpServer 不经过该锁，
// 以隔离临时库为主；任务级认领与结算唯一性约束仍提供跨进程的最后防线。
class InstanceLock {
public:
  ~InstanceLock();

  InstanceLock(const InstanceLock &) = delete;
  InstanceLock &operator=(const InstanceLock &) = delete;

  // 获取锁：成功返回对象，失败（已有实例持有 / 无法创建锁文件）返回 nullptr
  // 并通过 error 给出原因。
  static std::unique_ptr<InstanceLock> acquire(const std::string &db_path,
                                               std::string &error);

private:
  InstanceLock(int fd, std::string path);

  int fd_ = -1;
  std::string path_;
};

} // namespace oj
