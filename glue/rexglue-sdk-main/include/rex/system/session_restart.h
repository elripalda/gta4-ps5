// Ordered host-side session restart for an in-process return to the hub.
//
// The steps are fixed because each one enables the next:
//   1. quiesce the title (workers, audio, music, UI capture);
//   2. release the presentation objects bound to the old runtime;
//   3. drain and destroy the runtime (cooperative shutdown, save-safe);
//   4. join the module thread that ran the guest;
//   5. reset the runtime owner so a new Runtime can be constructed;
//   6. rebuild the presentation for the new runtime;
//   7. resume the host UI (the hub) and accept another launch.
//
// A failure stops the sequence at that step and latches it. Steps after the
// failure never run, so the caller never reports a partial restart as success.
// This model is dependency-free so the order and failure contract can be
// tested on the host.
#pragma once

#include <cstdint>

namespace rex::system {

enum class SessionRestartStep : std::uint8_t {
  kIdle = 0,
  kQuiesceSession,
  kReleasePresentation,
  kStopRuntime,
  kJoinModuleThread,
  kResetRuntime,
  kRebuildPresentation,
  kResumeHostUi,
  kComplete,
  kFailed,
};

enum class SessionRestartFailure : std::uint8_t {
  kNone = 0,
  kBusy,
  kQuiesceFailed,
  kPresentationReleaseFailed,
  kShutdownBlocked,
  kModuleThreadJoinFailed,
  kRuntimeResetFailed,
  kPresentationRebuildFailed,
  kHostUiResumeFailed,
};

class SessionRestartSequence {
 public:
  // Starts a restart. A second request while one is active is refused; the
  // caller must not tear down the same session twice.
  bool Begin() {
    if (active()) {
      return false;
    }
    pending_ = SessionRestartStep::kQuiesceSession;
    failure_ = SessionRestartFailure::kNone;
    failed_step_ = SessionRestartStep::kIdle;
    return true;
  }

  // Completes the pending step. `success == false` latches the failure and
  // returns false; no later step can run. Returns true while the sequence is
  // still progressing or has completed.
  bool CompleteStep(bool success) {
    if (!active()) {
      return complete();
    }
    if (!success) {
      failed_step_ = pending_;
      failure_ = FailureForStep(pending_);
      pending_ = SessionRestartStep::kFailed;
      return false;
    }
    pending_ = NextStep(pending_);
    return true;
  }

  // Returns the sequence to idle without recording a failure. Used when the
  // request is withdrawn before any destructive step ran.
  void Abort() {
    pending_ = SessionRestartStep::kIdle;
    failure_ = SessionRestartFailure::kNone;
    failed_step_ = SessionRestartStep::kIdle;
  }

  bool active() const {
    return pending_ != SessionRestartStep::kIdle &&
           pending_ != SessionRestartStep::kComplete &&
           pending_ != SessionRestartStep::kFailed;
  }
  bool complete() const { return pending_ == SessionRestartStep::kComplete; }
  bool failed() const { return pending_ == SessionRestartStep::kFailed; }
  // The step that must run next, or kComplete/kFailed/kIdle.
  SessionRestartStep pending_step() const { return pending_; }
  SessionRestartStep failed_step() const { return failed_step_; }
  SessionRestartFailure failure() const { return failure_; }

 private:
  static SessionRestartStep NextStep(SessionRestartStep step) {
    switch (step) {
      case SessionRestartStep::kQuiesceSession:
        return SessionRestartStep::kReleasePresentation;
      case SessionRestartStep::kReleasePresentation:
        return SessionRestartStep::kStopRuntime;
      case SessionRestartStep::kStopRuntime:
        return SessionRestartStep::kJoinModuleThread;
      case SessionRestartStep::kJoinModuleThread:
        return SessionRestartStep::kResetRuntime;
      case SessionRestartStep::kResetRuntime:
        return SessionRestartStep::kRebuildPresentation;
      case SessionRestartStep::kRebuildPresentation:
        return SessionRestartStep::kResumeHostUi;
      case SessionRestartStep::kResumeHostUi:
        return SessionRestartStep::kComplete;
      default:
        return step;
    }
  }

  static SessionRestartFailure FailureForStep(SessionRestartStep step) {
    switch (step) {
      case SessionRestartStep::kQuiesceSession:
        return SessionRestartFailure::kQuiesceFailed;
      case SessionRestartStep::kReleasePresentation:
        return SessionRestartFailure::kPresentationReleaseFailed;
      case SessionRestartStep::kStopRuntime:
        return SessionRestartFailure::kShutdownBlocked;
      case SessionRestartStep::kJoinModuleThread:
        return SessionRestartFailure::kModuleThreadJoinFailed;
      case SessionRestartStep::kResetRuntime:
        return SessionRestartFailure::kRuntimeResetFailed;
      case SessionRestartStep::kRebuildPresentation:
        return SessionRestartFailure::kPresentationRebuildFailed;
      case SessionRestartStep::kResumeHostUi:
        return SessionRestartFailure::kHostUiResumeFailed;
      default:
        return SessionRestartFailure::kNone;
    }
  }

  SessionRestartStep pending_ = SessionRestartStep::kIdle;
  SessionRestartStep failed_step_ = SessionRestartStep::kIdle;
  SessionRestartFailure failure_ = SessionRestartFailure::kNone;
};

inline const char* SessionRestartStepName(SessionRestartStep step) {
  switch (step) {
    case SessionRestartStep::kIdle:
      return "idle";
    case SessionRestartStep::kQuiesceSession:
      return "quiesce-session";
    case SessionRestartStep::kReleasePresentation:
      return "release-presentation";
    case SessionRestartStep::kStopRuntime:
      return "stop-runtime";
    case SessionRestartStep::kJoinModuleThread:
      return "join-module-thread";
    case SessionRestartStep::kResetRuntime:
      return "reset-runtime";
    case SessionRestartStep::kRebuildPresentation:
      return "rebuild-presentation";
    case SessionRestartStep::kResumeHostUi:
      return "resume-host-ui";
    case SessionRestartStep::kComplete:
      return "complete";
    case SessionRestartStep::kFailed:
      return "failed";
  }
  return "unknown";
}

inline const char* SessionRestartFailureName(SessionRestartFailure failure) {
  switch (failure) {
    case SessionRestartFailure::kNone:
      return "none";
    case SessionRestartFailure::kBusy:
      return "busy";
    case SessionRestartFailure::kQuiesceFailed:
      return "quiesce-failed";
    case SessionRestartFailure::kPresentationReleaseFailed:
      return "presentation-release-failed";
    case SessionRestartFailure::kShutdownBlocked:
      return "shutdown-blocked";
    case SessionRestartFailure::kModuleThreadJoinFailed:
      return "module-thread-join-failed";
    case SessionRestartFailure::kRuntimeResetFailed:
      return "runtime-reset-failed";
    case SessionRestartFailure::kPresentationRebuildFailed:
      return "presentation-rebuild-failed";
    case SessionRestartFailure::kHostUiResumeFailed:
      return "host-ui-resume-failed";
  }
  return "unknown";
}

}  // namespace rex::system
