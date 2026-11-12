// Relaunch policy for launching a second title in the same process.
//
// A second guest session is only safe once the previous runtime was torn down
// cleanly, the module thread finished and was joined, presentation can be
// re-established, and a fresh backend config can be built. This policy keeps
// that decision explicit and testable; the app still owns the actual
// reinitialization steps.
#pragma once

#include <cstdint>

namespace rex::system {

enum class RelaunchBlocker : uint8_t {
  kNone = 0,
  // The app is closing; a relaunch must not start.
  kProcessExitPending,
  // The previous title did not prove guest and save quiescence.
  kShutdownIncomplete,
  // The previous module thread has not finished and been joined.
  kModuleThreadLive,
  // Presentation (window presenter, overlay drawers) is not reusable yet.
  kPresentationUnavailable,
  // No backend config can be built for the fresh Runtime.
  kConfigUnavailable,
};

struct RelaunchState {
  bool process_exit_pending = false;
  bool shutdown_clean = false;
  bool module_thread_joined = false;
  bool presentation_reusable = false;
  bool config_available = false;
};

struct RelaunchDecision {
  bool allowed = false;
  RelaunchBlocker blocker = RelaunchBlocker::kShutdownIncomplete;
};

// Evaluates the preconditions in order of severity. A false input never reads
// as allowed, and the first failing precondition is reported so the caller can
// name the smallest unresolved dependency.
inline RelaunchDecision EvaluateRelaunch(const RelaunchState& state) {
  if (state.process_exit_pending) {
    return {false, RelaunchBlocker::kProcessExitPending};
  }
  if (!state.shutdown_clean) {
    return {false, RelaunchBlocker::kShutdownIncomplete};
  }
  if (!state.module_thread_joined) {
    return {false, RelaunchBlocker::kModuleThreadLive};
  }
  if (!state.presentation_reusable) {
    return {false, RelaunchBlocker::kPresentationUnavailable};
  }
  if (!state.config_available) {
    return {false, RelaunchBlocker::kConfigUnavailable};
  }
  return {true, RelaunchBlocker::kNone};
}

inline const char* RelaunchBlockerName(RelaunchBlocker blocker) {
  switch (blocker) {
    case RelaunchBlocker::kNone:
      return "none";
    case RelaunchBlocker::kProcessExitPending:
      return "process-exit-pending";
    case RelaunchBlocker::kShutdownIncomplete:
      return "shutdown-incomplete";
    case RelaunchBlocker::kModuleThreadLive:
      return "module-thread-live";
    case RelaunchBlocker::kPresentationUnavailable:
      return "presentation-unavailable";
    case RelaunchBlocker::kConfigUnavailable:
      return "config-unavailable";
  }
  return "unknown";
}

}  // namespace rex::system
