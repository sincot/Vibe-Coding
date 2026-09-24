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
  // 通过人数：已 AC 的不同用户数（同一用户重复 AC 只计一人）。
  long long pass_count = 0;
  // 本人对该题是否已 AC；仅当查询携带了已登录用户 ID 时有意义（游客为 false）。
  bool solved = false;
};

// 列表可见性筛选（M2.3）。调用方须先完成身份判定：非管理员只能使用
// VisibleOnly，不得因筛选参数而看到隐藏题目。
enum class ProblemVisibility {
  VisibleOnly, // 仅 visible=1
  All,         // 全部（含隐藏），仅管理员可用
  HiddenOnly,  // 仅 visible=0，仅管理员可用
};

// 列表查询条件。keyword/difficulty/tag 已由 problem::parse_list_query
// 归一化（去空白、空表示不限）；viewer_user_id 为 0 表示游客（不返回本人状态），
// 正数表示当前登录用户（来自已验证的身份上下文，不接受客户端指定）。
struct ProblemListQuery {
  ProblemVisibility visibility = ProblemVisibility::VisibleOnly;
  std::string keyword;
  std::string difficulty;
  std::string tag;
  std::int64_t viewer_user_id = 0;
  int page = 1;
  int page_size = 20;
};

// 列表查询结果：当前页数据与满足相同筛选/可见性条件的总数。
struct ProblemListResult {
  std::vector<ProblemSummary> items;
  long long total = 0;
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

  // 带搜索/难度/标签筛选与分页的列表查询（M2.3），按 id 升序稳定排序。
  // 列表与 total 使用完全相同的筛选与可见性条件，total 不会泄露隐藏题目。
  // 每页大小由 query.page_size 指定（接口固定为 20）。超出末页返回空列表且
  // total 仍为筛选后的总数。返回 false 表示数据库错误。
  bool query(const ProblemListQuery &query, ProblemListResult &out,
             std::string &error);

  // 读取题目标签集合（M4.2）：跨所有题目去重，按字节升序稳定排序。
  // include_hidden=false 时只统计 visible=1 的题目，避免向普通用户/游客泄露
  // 仅隐藏题目使用的标签；true 供通过管理员检查的调用方使用（含隐藏题目标签）。
  // 不从分页结果生成，始终基于完整可见范围的题目。返回 false 表示数据库错误。
  bool list_tags(bool include_hidden, std::vector<std::string> &out,
                 std::string &error);

  // 按 ID 查询题目元数据（不区分可见性；可见性由调用方结合当前身份判断）。
  // 返回 true 表示查询过程正常，found 指示是否存在；返回 false 表示数据库错误。
  bool find_by_id(std::int64_t id, bool &found, ProblemRecord &out,
                  std::string &error);

  // 查询某用户对某题的做题状态（M4.3 详情页本人状态的最小读取能力）。
  // user_id 必须来自已验证身份（<=0 表示游客，视为未 AC 且不查询）。
  // out_solved 表示该用户在该题是否已 AC；无状态记录视为未 AC。
  // 只读取 user_problem_status，不修改状态、不重新判题。返回 false 表示数据库错误。
  bool viewer_solved(std::int64_t user_id, std::int64_t problem_id,
                     bool &out_solved, std::string &error);

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
