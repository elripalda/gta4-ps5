// Cooperative shutdown policy for an in-process title transition.
//
// The sequence is fixed and testable with injected steps:
//   1. drain guest threads cooperatively (never force any thread);
//   2. only after a clean drain, close idle save content and flush it;
//   3. wait for save quiescence;
//   4. only when both phases are clean, destroy runtime resources.
//
// A blocked phase returns without calling any later step, so the runtime keeps
// every resource the still-running guest or the incomplete save could touch.
// The caller may retry; nothing here resets state to fake a clean result.
#pragma once

#include <rex/system/cooperative_drain.h>
#include <rex/system/save_completion.h>

namespace rex::system {

enum class ShutdownStatus {
  kBlockedOnGuestThreads,
  kBlockedOnSaveWork,
  kComplete,
  // Teardown ran but retained owners still hold resources; replacement is blocked.
  kReleasedIncomplete,
};

struct ShutdownReport {
  ShutdownStatus status = ShutdownStatus::kBlockedOnGuestThreads;
  DrainResult guest_threads;
  SaveWaitResult save_work;

  bool complete() const { return status == ShutdownStatus::kComplete; }
  bool blocked() const { return !complete(); }
};

// Each step is injected so the ordering and retention rules can be exercised
// without a guest, a GPU, or a filesystem. destroy returns whether every
// resource was released; false keeps the report incomplete and blocks replacement.
template <typename DrainFn, typename CloseIdleSavesFn, typename WaitSavesFn, typename DestroyFn>
ShutdownReport RunCooperativeShutdown(DrainFn drain, CloseIdleSavesFn close_idle_saves,
                                      WaitSavesFn wait_saves, DestroyFn destroy) {
  ShutdownReport report;

  report.guest_threads = drain();
  if (!report.guest_threads.clean()) {
    report.status = ShutdownStatus::kBlockedOnGuestThreads;
    return report;
  }

  // No guest thread can issue I/O now. Closing what is still mounted releases
  // its handles (flushing them) instead of leaking the mount into teardown.
  close_idle_saves();

  report.save_work = wait_saves();
  if (!report.save_work.clean) {
    report.status = ShutdownStatus::kBlockedOnSaveWork;
    return report;
  }

  if (!destroy()) {
    report.status = ShutdownStatus::kReleasedIncomplete;
    return report;
  }
  report.status = ShutdownStatus::kComplete;
  return report;
}

}  // namespace rex::system
