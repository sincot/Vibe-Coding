#include "db/instance_lock.h"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <utility>

namespace oj {

InstanceLock::InstanceLock(int fd, std::string path)
    : fd_(fd), path_(std::move(path)) {}

InstanceLock::~InstanceLock() {
  if (fd_ >= 0) {
    // 显式释放锁并关闭 fd；进程退出时操作系统同样会释放。
    ::flock(fd_, LOCK_UN);
    ::close(fd_);
    fd_ = -1;
  }
}

std::unique_ptr<InstanceLock> InstanceLock::acquire(const std::string &db_path,
                                                     std::string &error) {
  const std::string lock_path = db_path + ".lock";
  int fd = ::open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  if (fd < 0) {
    error = "无法打开实例锁文件 \"" + lock_path + "\": " + std::strerror(errno);
    return nullptr;
  }
  if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
    const int err = errno;
    ::close(fd);
    if (err == EWOULDBLOCK) {
      error = "已有服务实例在使用数据库 \"" + db_path +
              "\"（实例锁被占用），拒绝启动以避免并发恢复同一批在途任务";
    } else {
      error = "获取实例锁失败 \"" + lock_path + "\": " + std::strerror(err);
    }
    return nullptr;
  }
  return std::unique_ptr<InstanceLock>(new InstanceLock(fd, lock_path));
}

} // namespace oj
