// 项目结构单元测试（M0.1，基于 gtest）。
//
// 作为工程骨架的回归保护：验证约定目录结构、关键文件与忽略规则是否齐备。
// 纯文件系统检查，不依赖任何运行时组件（不触碰数据库与网络）。
//
// 源码根目录通过编译期宏 OJ_SOURCE_DIR 注入（见 CMakeLists.txt）。
//
// 运行方式：ctest --test-dir build -R project_structure_unit --output-on-failure
// 或直接执行 build/oj_project_structure_test。

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace fs = std::filesystem;

namespace {

#ifndef OJ_SOURCE_DIR
#error "OJ_SOURCE_DIR 未定义：请在 CMake 中为测试目标注入源码根目录"
#endif

const std::string kRoot = OJ_SOURCE_DIR;

bool is_dir(const std::string &rel) {
  return fs::is_directory(fs::path(kRoot) / rel);
}

bool is_file(const std::string &rel) {
  return fs::is_regular_file(fs::path(kRoot) / rel);
}

std::string read_file(const std::string &rel) {
  std::ifstream in(fs::path(kRoot) / rel);
  return std::string(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
}

} // namespace

TEST(ProjectStructure, RequiredDirectoriesExist) {
  const char *dirs[] = {
      "src",          "src/auth",     "src/db",        "src/http",
      "src/judge",    "web",          "web/css",       "web/js",
      "web/js/pages", "web/assets",   "scripts",       "data",
      "tests",        "tests/unit",
  };
  for (const char *d : dirs) {
    EXPECT_TRUE(is_dir(d)) << "缺少约定目录: " << d;
  }
}

TEST(ProjectStructure, RequiredFilesExist) {
  const char *files[] = {"SPEC.md",    "dependence.md", ".gitignore",
                         "README.md",  "CMakeLists.txt", "src/main.cpp"};
  for (const char *f : files) {
    EXPECT_TRUE(is_file(f)) << "缺少关键文件: " << f;
  }
}

TEST(ProjectStructure, GitkeepPlaceholdersPresent) {
  // 尚未加入实现源码的空目录应保留 .gitkeep 占位，保证空目录可被 Git 跟踪。
  // 注意：后续里程碑在这些目录加入文件后，应同步移除对应 .gitkeep 并更新此用例。
  const char *dirs[] = {"scripts", "data", "src/judge", "web/css",
                        "web/js",  "web/js/pages", "web/assets"};
  for (const char *d : dirs) {
    EXPECT_TRUE(is_file(std::string(d) + "/.gitkeep")) << "缺少占位文件: " << d;
  }
}

TEST(ProjectStructure, GitignoreCoversArtifacts) {
  const std::string gi = read_file(".gitignore");
  const char *patterns[] = {"/build/", "data/oj.db", "*.db", "*.db-wal",
                            "*.db-shm", "*.log", "backup/", "*.o",
                            ".env"};
  for (const char *p : patterns) {
    EXPECT_NE(gi.find(p), std::string::npos) << "缺少忽略规则: " << p;
  }
}
