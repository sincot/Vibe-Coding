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

const char *termination_reason_name(TerminationReason reason) {
  switch (reason) {
  case TerminationReason::Completed:
    return "completed";
  case TerminationReason::NonZeroExit:
    return "non_zero_exit";
  case TerminationReason::Signaled:
    return "signaled";
  case TerminationReason::TimedOut:
    return "timed_out";
  case TerminationReason::MemoryExceeded:
    return "memory_exceeded";
  case TerminationReason::Cancelled:
    return "cancelled";
  case TerminationReason::LaunchFailure:
    return "launch_failure";
  }
  return "launch_failure";
}

} // namespace judge
} // namespace oj
