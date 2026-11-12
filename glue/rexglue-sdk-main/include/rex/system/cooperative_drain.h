// Cooperative guest-thread drain.
//
// Polls a caller-supplied running predicate until every listed thread has
// exited or the timeout elapses. It never terminates a thread: a straggler is
// reported, not killed. The clock and sleep are injected so the policy can be
// tested on the host without real waits.
#pragma once

#include <chrono>
#include <cstdint>
#include <vector>

namespace rex::system {

enum class DrainStatus {
  kClean,
  kStragglers,
};

struct DrainResult {
  DrainStatus status = DrainStatus::kStragglers;
  uint32_t requested_threads = 0;
  std::vector<uint32_t> surviving_threads;
  std::chrono::milliseconds waited{0};

  bool clean() const { return status == DrainStatus::kClean; }
};

template <typename IsRunning, typename Now, typename SleepFor>
DrainResult DrainCooperatively(const std::vector<uint32_t>& thread_ids, IsRunning is_running,
                               std::chrono::milliseconds timeout, Now now, SleepFor sleep_for) {
  const auto start = now();
  const auto deadline = start + timeout;
  DrainResult result;
  result.requested_threads = static_cast<uint32_t>(thread_ids.size());

  for (;;) {
    result.surviving_threads.clear();
    for (const uint32_t thread_id : thread_ids) {
      if (is_running(thread_id)) {
        result.surviving_threads.push_back(thread_id);
      }
    }
    if (result.surviving_threads.empty()) {
      result.status = DrainStatus::kClean;
      break;
    }
    if (now() >= deadline) {
      result.status = DrainStatus::kStragglers;
      break;
    }
    sleep_for(std::chrono::milliseconds(1));
  }

  result.waited = std::chrono::duration_cast<std::chrono::milliseconds>(now() - start);
  return result;
}

}  // namespace rex::system
