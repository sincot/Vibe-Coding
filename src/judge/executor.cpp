#include "judge/executor.h"

#include <cctype>
#include <string>

namespace oj {
namespace judge {

bool parse_language(const std::string &text, Language &out) {
  std::string lowered;
  lowered.reserve(text.size());
  for (char c : text) {
    lowered.push_back(
        static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  if (lowered == "c++17" || lowered == "cpp17" || lowered == "c++" ||
      lowered == "cpp") {
    out = Language::Cpp17;
    return true;
  }
  if (lowered == "c11" || lowered == "c") {
    out = Language::C11;
    return true;
  }
  return false;
}

const char *language_name(Language language) {
  switch (language) {
  case Language::Cpp17:
    return "cpp17";
  case Language::C11:
    return "c11";
  }
  return "unknown";
}

} // namespace judge
} // namespace oj
