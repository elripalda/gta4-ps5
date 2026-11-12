// Host-side tests for the ordered in-process session restart.
#include <string>

#include <rex/system/session_restart.h>

#ifdef GTA4_F02_STANDALONE
#include <cstdio>
#include <stdexcept>

static int g_checks = 0;
static int g_failures = 0;
#define CHECK_F02(...)                                                       \
  do {                                                                       \
    ++g_checks;                                                              \
    if (!(__VA_ARGS__)) throw std::runtime_error(#__VA_ARGS__);              \
  } while (false)
#define RUN_F02(fn)                                                          \
  do {                                                                       \
    try {                                                                    \
      fn();                                                                  \
    } catch (const std::exception& error) {                                  \
      std::fprintf(stderr, "FAIL %s: %s\n", #fn, error.what());              \
      ++g_failures;                                                          \
    }                                                                        \
  } while (false)
#else
#include <catch2/catch_test_macros.hpp>
#define CHECK_F02(...) REQUIRE(__VA_ARGS__)
#define RUN_F02(fn) fn()
#endif

namespace {

using rex::system::SessionRestartFailure;
using rex::system::SessionRestartFailureName;
using rex::system::SessionRestartSequence;
using rex::system::SessionRestartStep;
using rex::system::SessionRestartStepName;

void RunToCompletion(SessionRestartSequence& sequence) {
  for (int step = 0; step < 16 && sequence.active(); ++step) {
    CHECK_F02(sequence.CompleteStep(true));
  }
  CHECK_F02(sequence.complete());
}

void StepsRunInTheDocumentedOrder() {
  SessionRestartSequence sequence;
  const SessionRestartStep expected[] = {
      SessionRestartStep::kQuiesceSession,       SessionRestartStep::kReleasePresentation,
      SessionRestartStep::kStopRuntime,          SessionRestartStep::kJoinModuleThread,
      SessionRestartStep::kResetRuntime,         SessionRestartStep::kRebuildPresentation,
      SessionRestartStep::kResumeHostUi,
  };
  CHECK_F02(sequence.Begin());
  for (const SessionRestartStep step : expected) {
    CHECK_F02(sequence.pending_step() == step);
    CHECK_F02(sequence.CompleteStep(true));
  }
  CHECK_F02(sequence.complete());
  CHECK_F02(sequence.failure() == SessionRestartFailure::kNone);
  CHECK_F02(sequence.failed_step() == SessionRestartStep::kIdle);
}

void SecondRequestIsRefusedWhileActive() {
  SessionRestartSequence sequence;
  CHECK_F02(sequence.Begin());
  CHECK_F02(!sequence.Begin());
  CHECK_F02(!sequence.complete());
  CHECK_F02(sequence.active());
  // The original sequence is untouched by the refused request.
  CHECK_F02(sequence.pending_step() == SessionRestartStep::kQuiesceSession);
}

void NoStepRunsAfterAFailure() {
  const SessionRestartStep steps[] = {
      SessionRestartStep::kQuiesceSession,       SessionRestartStep::kReleasePresentation,
      SessionRestartStep::kStopRuntime,          SessionRestartStep::kJoinModuleThread,
      SessionRestartStep::kResetRuntime,         SessionRestartStep::kRebuildPresentation,
      SessionRestartStep::kResumeHostUi,
  };
  const SessionRestartFailure failures[] = {
      SessionRestartFailure::kQuiesceFailed,
      SessionRestartFailure::kPresentationReleaseFailed,
      SessionRestartFailure::kShutdownBlocked,
      SessionRestartFailure::kModuleThreadJoinFailed,
      SessionRestartFailure::kRuntimeResetFailed,
      SessionRestartFailure::kPresentationRebuildFailed,
      SessionRestartFailure::kHostUiResumeFailed,
  };
  for (size_t index = 0; index < sizeof(steps) / sizeof(steps[0]); ++index) {
    SessionRestartSequence sequence;
    CHECK_F02(sequence.Begin());
    for (size_t prior = 0; prior < index; ++prior) {
      CHECK_F02(sequence.pending_step() == steps[prior]);
      CHECK_F02(sequence.CompleteStep(true));
    }
    CHECK_F02(sequence.pending_step() == steps[index]);
    CHECK_F02(!sequence.CompleteStep(false));
    CHECK_F02(sequence.failed());
    CHECK_F02(!sequence.active());
    CHECK_F02(sequence.failed_step() == steps[index]);
    CHECK_F02(sequence.failure() == failures[index]);
    // A failed sequence cannot advance, even with a success.
    CHECK_F02(sequence.CompleteStep(true) == false);
    CHECK_F02(sequence.failed());
  }
}

void AbortBeforeDestructionReturnsToIdle() {
  SessionRestartSequence sequence;
  CHECK_F02(sequence.Begin());
  CHECK_F02(sequence.CompleteStep(true));  // quiesce
  CHECK_F02(sequence.pending_step() == SessionRestartStep::kReleasePresentation);
  sequence.Abort();
  CHECK_F02(!sequence.active());
  CHECK_F02(!sequence.complete());
  CHECK_F02(!sequence.failed());
  CHECK_F02(sequence.pending_step() == SessionRestartStep::kIdle);
  CHECK_F02(sequence.failure() == SessionRestartFailure::kNone);
}

void RepeatedCyclesStartFresh() {
  SessionRestartSequence sequence;
  for (int cycle = 0; cycle < 4; ++cycle) {
    CHECK_F02(sequence.Begin());
    RunToCompletion(sequence);
    CHECK_F02(sequence.failure() == SessionRestartFailure::kNone);
  }
}

void NamesAreStable() {
  CHECK_F02(std::string(SessionRestartStepName(SessionRestartStep::kQuiesceSession)) ==
            "quiesce-session");
  CHECK_F02(std::string(SessionRestartStepName(SessionRestartStep::kStopRuntime)) ==
            "stop-runtime");
  CHECK_F02(std::string(SessionRestartStepName(SessionRestartStep::kComplete)) == "complete");
  CHECK_F02(std::string(SessionRestartFailureName(SessionRestartFailure::kShutdownBlocked)) ==
            "shutdown-blocked");
  CHECK_F02(std::string(SessionRestartFailureName(SessionRestartFailure::kNone)) == "none");
}

}  // namespace

#ifdef GTA4_F02_STANDALONE
int main() {
  RUN_F02(StepsRunInTheDocumentedOrder);
  RUN_F02(SecondRequestIsRefusedWhileActive);
  RUN_F02(NoStepRunsAfterAFailure);
  RUN_F02(AbortBeforeDestructionReturnsToIdle);
  RUN_F02(RepeatedCyclesStartFresh);
  RUN_F02(NamesAreStable);
  std::printf("f02_session_restart: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
#else
TEST_CASE("F02 restart steps run in order") {
  RUN_F02(StepsRunInTheDocumentedOrder);
}
TEST_CASE("F02 restart refuses a second request") {
  RUN_F02(SecondRequestIsRefusedWhileActive);
}
TEST_CASE("F02 restart stops at the first failure") {
  RUN_F02(NoStepRunsAfterAFailure);
}
TEST_CASE("F02 restart can abort before teardown") {
  RUN_F02(AbortBeforeDestructionReturnsToIdle);
}
TEST_CASE("F02 restart supports repeated cycles") {
  RUN_F02(RepeatedCyclesStartFresh);
}
TEST_CASE("F02 restart names are stable") {
  RUN_F02(NamesAreStable);
}
#endif
