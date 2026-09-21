#pragma once

#include <memory>
#include <string>

namespace oj {
namespace judge {

// 一次判题的独立临时工作目录（SPEC JUDGE-04 的基础形态）。
//
// 通过 mkdtemp 在指定基目录下原子创建唯一目录（权限 0700），用于保存源码与
// 编译产物。析构时只删除本对象创建的目录，绝不影响其他文件。
//
// M1.5 默认基目录为系统临时目录，可配置；M3 将改为 tmpfs 下的随机目录。
class Workspace {
public:
  ~Workspace();

  Workspace(Workspace &&other) noexcept;
  Workspace &operator=(Workspace &&other) noexcept;
  Workspace(const Workspace &) = delete;
  Workspace &operator=(const Workspace &) = delete;

  // 在 base_directory 下创建唯一目录；base_directory 为空时使用系统临时目录。
  // 失败返回 nullptr，error 非空。
  static std::unique_ptr<Workspace> create(const std::string &base_directory,
                                           std::string &error);

  const std::string &path() const { return path_; }

  // 在目录内写入文件（覆盖）。返回 false 表示失败，error 非空。
  bool write_file(const std::string &name, const std::string &content,
                  std::string &error) const;

private:
  explicit Workspace(std::string path);

  std::string path_;
};

} // namespace judge
} // namespace oj
