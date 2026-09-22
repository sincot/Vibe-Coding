#pragma once

#include <cstddef>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace oj {
namespace problem {

// 隐藏测试用例的可写字段与取值范围（M2.2）。
//
// 公开样例（is_sample=1）由 M2.1 的题目接口通过 samples 字段整体维护；本组校验函数
// 只处理隐藏用例（is_sample=0）的 input/output/ord，绝不读取 is_sample，从而不会
// 因新增、修改或排序把隐藏用例变成公开样例。
//
// ord 规则（沿用既有约定）：
//   - 起始值为 0；取值 [0, kMaxTestcaseOrd]；
//   - 允许重复；同一题内用例顺序由 (ord ASC, id ASC) 唯一确定（id 为稳定第二排序键），
//     数据库读取、管理列表与判题执行使用同一排序，不依赖默认行顺序；
//   - 新增时缺省 ord = 该题全部用例（含公开样例）当前最大 ord + 1，无用例时为 0；
//   - 删除不重排、不回收空号，保留原始 ord。
inline constexpr int kMinTestcaseOrd = 0;
inline constexpr int kMaxTestcaseOrd = 1000000;

// 单条用例的输入/期望输出文本上限。与题目 description/样例字段一致，均为 64 KiB；
// 整体请求体上限（1 MiB）由 HTTP 层统一限制。空字符串是合法内容，与字段缺失不同。
inline constexpr std::size_t kMaxTestcaseTextBytes = 64 * 1024;

// 新增隐藏用例所需字段（已通过校验）。
struct TestcaseData {
  std::string input;
  std::string output;
  std::optional<int> ord; // 未提供时由数据层按「末尾追加」规则计算
};

// 修改隐藏用例的部分更新字段：仅包含请求体中实际出现的字段。
// 未出现的字段保持数据库中的原值，不会被清空；需要清空时显式传入空值（如 ""）。
struct TestcasePatch {
  std::optional<std::string> input;
  std::optional<std::string> output;
  std::optional<int> ord;

  bool empty() const;
};

// 解析并校验「新增测试用例」请求体：
//   - 必填：input、output（均须为字符串，允许空串；区分「空串」与「缺失」）；
//   - 可选：ord（整数，范围 [0, kMaxTestcaseOrd]）；
//   - 只读取以上字段；id/problem_id/is_sample 等由服务端管理，客户端传入一律忽略。
// 成功返回 true 并填充 out；失败返回 false 并写入用户可读的 error。
bool parse_create_testcase(const nlohmann::json &body, TestcaseData &out,
                           std::string &error);

// 解析并校验「修改测试用例」请求体（部分更新）。仅读取 input/output/ord。
// 请求体不含任何可更新字段时返回 false（error 说明没有可更新字段）。
bool parse_update_testcase(const nlohmann::json &body, TestcasePatch &out,
                           std::string &error);

} // namespace problem
} // namespace oj
