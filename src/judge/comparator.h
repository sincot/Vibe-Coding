#pragma once

#include <string>

namespace oj {
namespace judge {

// 输出归一化（SPEC JUDGE-06）：
//   1. 按 '\n' 切分文本为若干行；
//   2. 去除每一行**行尾**的空白字符（空格、制表符、回车等），行首与行内空白保留；
//   3. 去除文末的空行（连续多行同样处理）；
//   4. 以 '\n' 逐字符重新连接比较。
//
// 不采用分词/折叠空白比较：行首空白、行内空白与中间空行都具有意义。
// 空串、纯空白、仅换行等都会归一化为空串。
std::string normalize_output(const std::string &text);

// 按上述规则比较期望输出与实际输出，完全一致返回 true。
bool outputs_match(const std::string &expected, const std::string &actual);

} // namespace judge
} // namespace oj
