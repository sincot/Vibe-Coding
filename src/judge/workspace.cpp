#include "judge/workspace.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <vector>

namespace oj {
namespace judge {

Workspace::Workspace(std::string path) : path_(std::move(path)) {}

Workspace::~Workspace() {
  if (!path_.empty()) {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }
}

Workspace::Workspace(Workspace &&other) noexcept
    : path_(std::move(other.path_)) {
  other.path_.clear();
}

Workspace &Workspace::operator=(Workspace &&other) noexcept {
  if (this != &other) {
    if (!path_.empty()) {
      std::error_code ec;
      std::filesystem::remove_all(path_, ec);
    }
    path_ = std::move(other.path_);
    other.path_.clear();
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
  return std::unique_ptr<Workspace>(new Workspace(created));
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
