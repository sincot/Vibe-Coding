#include "judge/workspace.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <vector>

#include "log.h"

namespace oj {
namespace judge {

Workspace::Workspace(std::string path, std::string base)
    : path_(std::move(path)), base_(std::move(base)) {}

Workspace::~Workspace() { cleanup(); }

void Workspace::cleanup() noexcept {
  if (path_.empty()) {
    return;
  }
  namespace fs = std::filesystem;
  std::error_code ec;

  // 防御性检查：路径必须仍位于创建时的基目录之下，防止清理越界。
  if (!base_.empty()) {
    std::error_code parent_ec;
    std::error_code base_ec;
    const fs::path parent =
        fs::weakly_canonical(fs::path(path_).parent_path(), parent_ec);
    const fs::path expected = fs::weakly_canonical(fs::path(base_), base_ec);
    if (parent_ec || base_ec || parent != expected) {
      // 路径越界或被替换：绝不清理，留下明确证据供排查（不误删其它任务资源）。
      log(LogLevel::Warn,
          "跳过判题工作目录清理（路径越界或无法确认归属）: " + path_);
      return;
    }
  }

  const fs::file_status status = fs::symlink_status(path_, ec);
  if (ec) {
    // 目录已不存在属正常可重复收尾场景；其它读取失败才记录，避免吞掉真实问题。
    std::error_code exists_ec;
    if (!fs::exists(path_, exists_ec)) {
      return;
    }
    log(LogLevel::Warn,
        "判题工作目录清理无法读取状态: " + path_ + "（" + ec.message() + "）");
    return;
  }
  if (fs::is_symlink(status)) {
    // 路径被替换为符号链接：只删除链接本身，绝不跟随删除其目标。
    fs::remove(path_, ec);
    if (ec) {
      log(LogLevel::Error, "删除判题目录符号链接失败: " + path_ + "（" +
                               ec.message() + "）");
    }
    return;
  }
  if (fs::is_directory(status)) {
    ec.clear();
    fs::remove_all(path_, ec);
    if (ec) {
      // 真实清理失败：保留现场并记录原因，不吞掉异常后报告成功。
      log(LogLevel::Error, "判题工作目录清理失败（资源保留待排查）: " + path_ +
                               "（" + ec.message() + "）");
    }
  }
}

Workspace::Workspace(Workspace &&other) noexcept
    : path_(std::move(other.path_)), base_(std::move(other.base_)) {
  other.path_.clear();
  other.base_.clear();
}

Workspace &Workspace::operator=(Workspace &&other) noexcept {
  if (this != &other) {
    cleanup();
    path_ = std::move(other.path_);
    base_ = std::move(other.base_);
    other.path_.clear();
    other.base_.clear();
  }
  return *this;
}

std::unique_ptr<Workspace>
Workspace::create(const std::string &base_directory, std::string &error) {
  namespace fs = std::filesystem;

  fs::path base =
      base_directory.empty() ? fs::temp_directory_path() : fs::path(base_directory);

  std::error_code ec;
  fs::create_directories(base, ec);
  if (ec) {
    error = "无法创建判题基目录 " + base.string() + ": " + ec.message();
    return nullptr;
  }

  std::string pattern = (base / "oj_judge_XXXXXX").string();
  std::vector<char> buffer(pattern.begin(), pattern.end());
  buffer.push_back('\0');

  char *created = ::mkdtemp(buffer.data());
  if (created == nullptr) {
    error = std::string("mkdtemp 失败: ") + std::strerror(errno);
    return nullptr;
  }
  return std::unique_ptr<Workspace>(new Workspace(created, base.string()));
}

bool Workspace::write_file(const std::string &name, const std::string &content,
                           std::string &error) const {
  std::filesystem::path target = std::filesystem::path(path_) / name;
  std::ofstream out(target.string(), std::ios::binary | std::ios::trunc);
  if (!out) {
    error = "无法写入文件: " + target.string();
    return false;
  }
  out.write(content.data(), static_cast<std::streamsize>(content.size()));
  if (!out) {
    error = "写入文件失败: " + target.string();
    return false;
  }
  return true;
}

} // namespace judge
} // namespace oj
