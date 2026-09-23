#pragma once

#include <condition_variable>
#include <mutex>

#include "judge/deadline.h"

namespace oj {
namespace judge {

// 全局编译并发门限。
//
// 编译阶段（尤其接入 ASan/UBSan 后）是单次判题中内存占用最高的部分；低内存服务器
// 上同时进行过多编译会耗尽内存。本门限把「正在编译」的任务数限制在 max_concurrent，
// 而运行阶段仍由 worker 数控制，从而在不改变 SPEC JUDGE-09 worker 数的前提下单独
// 约束高内存阶段。
//
// 等待门限的时间计入调用方传入的全局截止时间，并周期性响应取消令牌；不会引入新的
// 无限等待：deadline 到期或收到取消即放弃获取。
class CompileGate {
public:
  explicit CompileGate(int max_concurrent);

  // 在 deadline 之前获取一个编译许可。期间周期性轮询 cancel。
  // 成功返回 true；截止时间耗尽或已取消返回 false（调用方据此判定为全局超时/取消，
  // 不启动编译）。
  bool acquire(const Deadline &deadline, const CancellationToken *cancel);

  // 释放一个编译许可。
  void release();

  int max_concurrent() const { return max_concurrent_; }
  int active() const;

private:
  const int max_concurrent_;
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  int active_ = 0;
};

// RAII 许可守卫：构造时获取，析构时释放，异常安全。
class CompileGateGuard {
public:
  CompileGateGuard(CompileGate *gate, const Deadline &deadline,
                   const CancellationToken *cancel)
      : gate_(gate) {
    // 无门限（gate==nullptr）时视为已获得许可，不限制。
    acquired_ = (gate_ == nullptr) || gate_->acquire(deadline, cancel);
  }
  ~CompileGateGuard() {
    if (acquired_ && gate_ != nullptr) {
      gate_->release();
    }
  }

  CompileGateGuard(const CompileGateGuard &) = delete;
  CompileGateGuard &operator=(const CompileGateGuard &) = delete;

  bool acquired() const { return acquired_; }
  // 提前释放许可（例如编译结束后、运行测试点前），避免运行阶段仍占着编译名额。
  void release_now() {
    if (acquired_ && gate_ != nullptr) {
      gate_->release();
      acquired_ = false;
    }
  }

private:
  CompileGate *gate_;
  bool acquired_ = false;
};

} // namespace judge
} // namespace oj
