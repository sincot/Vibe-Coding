#include "judge/deadline.h"

#include <chrono>
#include <limits>

namespace oj {
namespace judge {

int Deadline::remaining_ms() const {
  const auto now = std::chrono::steady_clock::now();
  if (now >= deadline_) {
    return 0;
  }
  const auto remaining =
      std::chrono::ceil<std::chrono::milliseconds>(deadline_ - now);
  const long long count = remaining.count();
  if (count > std::numeric_limits<int>::max()) {
    return std::numeric_limits<int>::max();
  }
  return static_cast<int>(count);
}

int effective_time_limit_ms(int problem_limit_ms, int remaining_global_ms) {
  if (problem_limit_ms <= 0 || remaining_global_ms <= 0) {
    return 0;
  }
  return problem_limit_ms < remaining_global_ms ? problem_limit_ms
                                                : remaining_global_ms;
}

} // namespace judge
} // namespace oj
