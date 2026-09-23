// M3.5 停止清理单元测试（gtest）：Workspace 清理失败的可重复性与可定位性。
//
// 只测试清理边界，不启动判题进程、不触碰数据库：
//   - 真正清理失败（只读子目录导致无法删除内容）时：记录含具体资源路径的日志、
//     保留现场，且不误删其它任务的目录；
//   - 资源已不存在时：重复收尾不崩溃（可重复执行）。
//
// 运行方式：ctest --test-dir build -R m35_cleanup_unit --output-on-failure
// 或直接执行 build/oj_m35_cleanup_unit。

#include <gtest/gtest.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "judge/workspace.h"

namespace fs = std::filesystem;
using oj::judge::Workspace;

namespace {

std::string make_temp_base(const std::string &label) {
  fs::path base = fs::temp_directory_path();
  for (int i = 0; i < 1000; ++i) {
    fs::path candidate = base / (label + "_" + std::to_string(::getpid()) +
                                 "_" + std::to_string(i));
    std::error_code ec;
    fs::create_directories(candidate, ec);
    if (!ec) {
      return candidate.string();
    }
  }
  return {};
}

// 临时把进程 stderr 重定向到管道，用于断言清理失败日志确实包含资源路径。
class StderrCapture {
public:
  StderrCapture() {
    if (::pipe(pipe_) != 0) {
      return;
    }
    saved_ = ::dup(STDERR_FILENO);
    if (saved_ < 0) {
      return;
    }
    if (::dup2(pipe_[1], STDERR_FILENO) < 0) {
      ::close(saved_);
      saved_ = -1;
      return;
    }
    ::close(pipe_[1]);
    pipe_[1] = -1;
    valid_ = true;
  }

  ~StderrCapture() { finish(); }

  std::string finish() {
    if (!valid_) {
      return {};
    }
    std::fflush(stderr);
    ::dup2(saved_, STDERR_FILENO);
    ::close(saved_);
    saved_ = -1;
    std::string out;
    char buffer[4096];
    for (;;) {
      const ssize_t n = ::read(pipe_[0], buffer, sizeof(buffer));
      if (n > 0) {
        out.append(buffer, static_cast<std::size_t>(n));
      } else if (n < 0 && errno == EINTR) {
        continue;
      } else {
        break;
      }
    }
    ::close(pipe_[0]);
    pipe_[0] = -1;
    valid_ = false;
    return out;
  }

private:
  int pipe_[2] = {-1, -1};
  int saved_ = -1;
  bool valid_ = false;
};

} // namespace

// 清理失败：日志能定位具体资源，且不误删其它任务目录。
TEST(M35Cleanup, LogsFailureLocationAndKeepsOthers) {
  if (::geteuid() == 0) {
    GTEST_SKIP() << "root 会绕过目录权限，无法模拟清理失败";
  }
  const std::string base = make_temp_base("m35_cleanup");
  ASSERT_FALSE(base.empty());
  std::string error;
  auto workspace = Workspace::create(base, error);
  ASSERT_NE(workspace, nullptr) << error;
  const std::string ws = workspace->path();

  // 另一个任务的目录：清理失败时绝不能被误删。
  const std::string other = base + "_other";
  fs::create_directories(other);
  {
    std::ofstream out(other + "/keep.txt");
    out << "keep";
  }

  // 制造无法删除的内容：只读子目录中的文件无法被 unlink。
  const std::string sub = ws + "/sub";
  fs::create_directories(sub);
  {
    std::ofstream out(sub + "/file");
    out << "x";
  }
  ASSERT_EQ(::chmod(sub.c_str(), 0555), 0);

  std::string captured;
  {
    StderrCapture capture;
    workspace.reset(); // 析构触发清理；remove_all 应失败并记录日志
    captured = capture.finish();
  }

  EXPECT_NE(captured.find(ws), std::string::npos)
      << "清理失败日志应包含具体资源路径，实际日志: " << captured;
  EXPECT_NE(captured.find("清理失败"), std::string::npos);
  EXPECT_TRUE(fs::exists(other + "/keep.txt")) << "不得误删其它任务目录";

  // 恢复权限后清理现场。
  ::chmod(sub.c_str(), 0755);
  std::error_code ec;
  fs::remove_all(ws, ec);
  fs::remove_all(other, ec);
  fs::remove_all(base, ec);
}

// 资源已不存在：重复收尾不崩溃、不报错。
TEST(M35Cleanup, RepeatableWhenResourceAlreadyGone) {
  const std::string base = make_temp_base("m35_cleanup_gone");
  ASSERT_FALSE(base.empty());
  std::string error;
  auto workspace = Workspace::create(base, error);
  ASSERT_NE(workspace, nullptr) << error;
  const std::string ws = workspace->path();

  std::error_code ec;
  fs::remove_all(ws, ec); // 先删除，模拟部分资源已不存在
  ASSERT_FALSE(fs::exists(ws));

  std::string captured;
  {
    StderrCapture capture;
    workspace.reset(); // 再析构：应静默正常收尾
    captured = capture.finish();
  }
  EXPECT_EQ(captured.find("清理失败"), std::string::npos);

  fs::remove_all(base, ec);
}
