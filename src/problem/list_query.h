#pragma once

#include <string>

namespace oj {
namespace problem {

// 题目列表查询（M2.3）的纯参数解析与条件构造逻辑。
//
// 本文件只做与数据库/HTTP 无关的字符串处理与校验，便于独立单元测试；
// 实际 SQL 构造、筛选与分页由 ProblemStore::query 完成，接口层只负责把
// 查询参数归一化为 ListFilter。
//
// 规则（与 README / SPEC M2.3 一致）：
//   - q：按题目标题做子串匹配；ASCII 字母大小写不敏感；空（去空白后）表示不限；
//        LIKE 中的 %/_/\ 视为普通字符（转义后配合 ESCAPE '\' 使用）。
//   - difficulty：仅 easy/medium/hard；空表示不限；其它取值属参数错误。
//   - tag：按完整标签匹配（不适用子串/前缀），空表示不限。
//   - page：默认 1，正整数；拒绝非法格式及超出上限的数值，避免偏移量溢出。
//   - visible：管理员可见性筛选，all/1/true/0/false；其它取值属参数错误。

// 每页固定大小（SPEC：每页 20 条）。
constexpr int kProblemPageSize = 20;

// page 允许的最大值。超过则明确拒绝，保证 (page-1)*page_size 不溢出且分页
// 偏移量在合理范围内；晚于末页但不超过该上限的页码返回正常空列表。
constexpr long long kMaxProblemPage = 1000000;

// 删除首尾 ASCII 空白（空格、制表、换行、回车、换页、垂直制表）。
std::string trim_ascii(const std::string &text);

// difficulty 是否落在沿用的问题表取值集合内。
bool is_valid_difficulty(const std::string &difficulty);

// 转义 LIKE 模式中的特殊字符，使其按普通字符匹配。配合 "ESCAPE '\'" 使用。
// 依次处理反斜杠、% 与 _。
std::string escape_like(const std::string &text);

// 标题子串匹配模式："%" + escape_like(keyword) + "%"。
// 调用方须保证 keyword 已 trim 且非空。
std::string title_like_pattern(const std::string &keyword);

// 标签完整匹配的 needle："," + tag + ","。配合
// instr(',' || tags || ',', ?) > 0 使用，实现「逗号分隔的完整标签」匹配，
// 避免把 "图" 当作 "图论" 的子串命中。
std::string tag_match_needle(const std::string &tag);

// 管理员可见性筛选取值。
enum class VisibleFilter {
  All,         // 全部（含隐藏）
  OnlyVisible, // 仅 visible=1
  OnlyHidden,  // 仅 visible=0
};

// 归一化后的列表查询参数。
struct ListFilter {
  std::string keyword;    // 已 trim，空 = 不限
  std::string difficulty; // 已 trim，空 = 不限
  std::string tag;        // 已 trim，空 = 不限
  int page = 1;
  int page_size = kProblemPageSize;
  VisibleFilter visible = VisibleFilter::All;
};

// 原始查询串参数（缺失与空串均以 "" 表示）。
struct RawListParams {
  std::string q;
  std::string difficulty;
  std::string tag;
  std::string page;
  std::string visible;
};

// 解析并校验列表查询参数。返回 false 时 out 不可用，error 为面向客户端的
// 错误文案。visible 的取值始终被校验（与角色无关）；调用方负责在非管理员
// 场景下忽略该筛选，确保不扩大访问范围。
bool parse_list_query(const RawListParams &raw, ListFilter &out,
                      std::string &error);

} // namespace problem
} // namespace oj
