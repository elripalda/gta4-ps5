// Host tests for the second-title relaunch policy.
#include <cstdint>
#include <string>

#include <rex/system/relaunch_policy.h>

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

using rex::system::EvaluateRelaunch;
using rex::system::RelaunchBlocker;
using rex::system::RelaunchBlockerName;
using rex::system::RelaunchDecision;
using rex::system::RelaunchState;

RelaunchState ReadyState() {
  RelaunchState state;
  state.process_exit_pending = false;
  state.shutdown_clean = true;
  state.module_thread_joined = true;
  state.presentation_reusable = true;
  state.config_available = true;
  return state;
}

void EveryPreconditionIsRequired() {
  const RelaunchDecision ready = EvaluateRelaunch(ReadyState());
  CHECK_F02(ready.allowed);
  CHECK_F02(ready.blocker == RelaunchBlocker::kNone);

  RelaunchState state = ReadyState();
  state.shutdown_clean = false;
  CHECK_F02(!EvaluateRelaunch(state).allowed);

  state = ReadyState();
  state.module_thread_joined = false;
  CHECK_F02(!EvaluateRelaunch(state).allowed);

  state = ReadyState();
  state.presentation_reusable = false;
  CHECK_F02(!EvaluateRelaunch(state).allowed);

  state = ReadyState();
  state.config_available = false;
  CHECK_F02(!EvaluateRelaunch(state).allowed);
}

void DefaultStateIsBlocked() {
  const RelaunchDecision decision = EvaluateRelaunch(RelaunchState{});
  CHECK_F02(!decision.allowed);
  CHECK_F02(decision.blocker == RelaunchBlocker::kShutdownIncomplete);
}

void BlockersAreReportedInSeverityOrder() {
  // Process exit wins over everything: a closing app never relaunches.
  RelaunchState state;
  state.process_exit_pending = true;
  CHECK_F02(EvaluateRelaunch(state).blocker == RelaunchBlocker::kProcessExitPending);

  // An unclean shutdown wins over thread and presentation concerns.
  state = RelaunchState{};
  state.process_exit_pending = false;
  state.shutdown_clean = false;
  state.module_thread_joined = false;
  state.presentation_reusable = false;
  state.config_available = false;
  CHECK_F02(EvaluateRelaunch(state).blocker == RelaunchBlocker::kShutdownIncomplete);

  // A live module thread wins over presentation and config.
  state.shutdown_clean = true;
  CHECK_F02(EvaluateRelaunch(state).blocker == RelaunchBlocker::kModuleThreadLive);

  // Presentation wins over a missing config.
  state.module_thread_joined = true;
  CHECK_F02(EvaluateRelaunch(state).blocker == RelaunchBlocker::kPresentationUnavailable);

  state.presentation_reusable = true;
  CHECK_F02(EvaluateRelaunch(state).blocker == RelaunchBlocker::kConfigUnavailable);
}

void BlockerNamesAreStable() {
  CHECK_F02(std::string(RelaunchBlockerName(RelaunchBlocker::kNone)) == "none");
  CHECK_F02(std::string(RelaunchBlockerName(RelaunchBlocker::kProcessExitPending)) ==
            "process-exit-pending");
  CHECK_F02(std::string(RelaunchBlockerName(RelaunchBlocker::kShutdownIncomplete)) ==
            "shutdown-incomplete");
  CHECK_F02(std::string(RelaunchBlockerName(RelaunchBlocker::kModuleThreadLive)) ==
            "module-thread-live");
  CHECK_F02(std::string(RelaunchBlockerName(RelaunchBlocker::kPresentationUnavailable)) ==
            "presentation-unavailable");
  CHECK_F02(std::string(RelaunchBlockerName(RelaunchBlocker::kConfigUnavailable)) ==
            "config-unavailable");
}

}  // namespace

#ifdef GTA4_F02_STANDALONE
int main() {
  RUN_F02(EveryPreconditionIsRequired);
  RUN_F02(DefaultStateIsBlocked);
  RUN_F02(BlockersAreReportedInSeverityOrder);
  RUN_F02(BlockerNamesAreStable);

  std::printf("%d checks, %d failed scenario(s)\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
#else
TEST_CASE("F02 relaunch requires every precondition") { RUN_F02(EveryPreconditionIsRequired); }
TEST_CASE("F02 relaunch defaults to blocked") { RUN_F02(DefaultStateIsBlocked); }
TEST_CASE("F02 relaunch reports the first blocker") { RUN_F02(BlockersAreReportedInSeverityOrder); }
TEST_CASE("F02 relaunch blocker names are stable") { RUN_F02(BlockerNamesAreStable); }
#endif
