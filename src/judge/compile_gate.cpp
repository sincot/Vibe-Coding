#include "judge/compile_gate.h"

#include <algorithm>
#include <chrono>

namespace oj {
namespace judge {

CompileGate::CompileGate(int max_concurrent)
    : max_concurrent_(max_concurrent < 1 ? 1 : max_concurrent) {}

bool CompileGate::acquire(const Deadline &deadline,
                          const CancellationToken *cancel) {
  std::unique_lock<std::mutex> lock(mutex_);
  while (active_ >= max_concurrent_) {
    if (cancel != nullptr && cancel->cancelled()) {
      return false;
    }
    const int remaining = deadline.remaining_ms();
    if (remaining <= 0) {
      return false;
    }
    // 轮询周期上限 20ms：既保证取消/全局超时能被及时观察到，也避免忙等。
    const int wait_ms = std::min(remaining, 20);
    condition_.wait_for(lock, std::chrono::milliseconds(wait_ms));
  }
  ++active_;
  return true;
}

void CompileGate::release() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (active_ > 0) {
    --active_;
  }
  condition_.notify_one();
}

int CompileGate::active() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return active_;
}

} // namespace judge
} // namespace oj
