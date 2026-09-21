#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace oj {

class Database;

// 题目列表条目：仅包含列表展示所需字段，不含题面与任何测试用例。
struct ProblemSummary {
  std::int64_t id = 0;
  std::string title;
  std::string difficulty;
  std::vector<std::string> tags;
  bool visible = true;
};

// 题目详情元数据：不含任何测试用例（公开样例与隐藏用例都需另行读取），
// 因此可以安全地序列化下发，不会泄露隐藏用例。
struct ProblemRecord {
  std::int64_t id = 0;
  std::string title;
  std::string description;
  std::string difficulty;
  std::vector<std::string> tags;
  int time_limit_ms = 2000;
  int memory_limit_kb = 65536;
  bool visible = true;
  std::string created_at;
  std::string updated_at;
};

// 公开样例：只有输入/输出，结构上无法承载隐藏用例或 is_sample 标记。
struct SampleCase {
  std::string input;
  std::string output;
};

// 测试用例完整记录（含隐藏用例），仅供判题与管理员用例管理使用，
// 绝不直接用于题目列表/详情接口的响应。
struct TestcaseRecord {
  std::int64_t id = 0;
  int ord = 0;
  std::string input;
  std::string output;
  bool is_sample = false;
};

// 题目数据访问封装。
//
// 所有查询均使用参数绑定，不拼接外部输入。公开查询（列表、详情、样例）与判题所需
// 的隐藏用例读取（list_testcases）在接口层面明确分离：前者返回的结构体不含隐藏
// 用例，后者仅供判题/管理调用，避免把含隐藏用例的数据库对象直接序列化。
class ProblemStore {
public:
  explicit ProblemStore(Database &db) : db_(db) {}

  // 题目列表，按 id 升序稳定排序。include_hidden=false 时只返回 visible=1 的题目；
  // true 时包含隐藏题目（供通过管理员检查的调用方使用）。
  // 返回 false 表示数据库错误，error 非空。
  bool list(bool include_hidden, std::vector<ProblemSummary> &out,
            std::string &error);

  // 按 ID 查询题目元数据（不区分可见性；可见性由调用方结合当前身份判断）。
  // 返回 true 表示查询过程正常，found 指示是否存在；返回 false 表示数据库错误。
  bool find_by_id(std::int64_t id, bool &found, ProblemRecord &out,
                  std::string &error);

  // 读取某题的公开样例（is_sample=1），按 ord 升序、id 升序稳定排序。
  bool list_samples(std::int64_t problem_id, std::vector<SampleCase> &out,
                    std::string &error);

  // 读取某题的全部测试用例（含隐藏用例），按 ord 升序、id 升序稳定排序。
  // 仅供判题与管理员用例管理使用，调用方不得将结果直接下发到公开接口。
  bool list_testcases(std::int64_t problem_id, std::vector<TestcaseRecord> &out,
                      std::string &error);

private:
  Database &db_;
};

// 将数据库中以逗号分隔的 tags 文本解析为去除首尾空白、跳过空项后的标签列表。
// 纯函数，便于单独测试；同时供列表与详情序列化复用。
std::vector<std::string> split_tags(const std::string &tags);

} // namespace oj
