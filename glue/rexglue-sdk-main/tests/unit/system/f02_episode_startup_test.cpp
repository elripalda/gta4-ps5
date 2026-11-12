// Host-side tests for the F02 episode-startup session state.
//
// The bug this guards: one in-process session consumed the hub's Home choice
// and the next session (after Return to Hub) never saw its own choice, so the
// retail newest-save scan loaded another episode.
#include <string>

#include "../../../gta4-recomp/src/gta4_episode_startup_policy.h"

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

using gta4::episode_startup::Session;
using gta4::episode_startup::SessionState;

// The hub writes the choice before each launch; the start's one-shot gate is
// the only state that survives a session boundary.
bool StartSessionWithChoice(int32_t choice) {
  if (!Session().ConsumeHomeChoice(choice)) return false;
  Session().boot_episode = choice;
  return true;
}

void FirstSessionUsesTheHomeChoice() {
  Session().Reset();
  CHECK_F02(StartSessionWithChoice(1));
  CHECK_F02(Session().start_episode_used);
  CHECK_F02(Session().boot_episode == 1);
}

void SecondSessionAfterReturnUsesItsOwnChoice() {
  Session().Reset();
  CHECK_F02(StartSessionWithChoice(1));  // TLAD chosen on Home
  Session().Reset();                     // Return to Hub ends the session
  CHECK_F02(StartSessionWithChoice(0));  // GTA IV chosen on Home
  CHECK_F02(Session().boot_episode == 0);
}

void WithoutTheResetTheSecondChoiceIsIgnored() {
  Session().Reset();
  CHECK_F02(StartSessionWithChoice(1));
  // A session boundary without Reset reproduces the hardware bug: the next
  // launch falls through to retail's newest-save scan.
  CHECK_F02(!StartSessionWithChoice(0));
}

void ChoiceIsConsumedOncePerSession() {
  Session().Reset();
  CHECK_F02(StartSessionWithChoice(2));
  CHECK_F02(!Session().ConsumeHomeChoice(2));  // the start only
  CHECK_F02(!Session().ConsumeHomeChoice(0));
}

void NoChoiceLeavesRetailBehavior() {
  Session().Reset();
  CHECK_F02(!Session().ConsumeHomeChoice(-1));
  CHECK_F02(!Session().start_episode_used);
  CHECK_F02(Session().boot_episode == -1);
  // The next start in the same session still sees a choice written for it.
  CHECK_F02(Session().ConsumeHomeChoice(2));
}

void ResetClearsEveryField() {
  Session().Reset();
  Session().answer_yes_for = 2;
  Session().start_episode_used = true;
  Session().boot_episode = 2;
  Session().in_start_steps = true;
  Session().Reset();
  const SessionState fresh;
  CHECK_F02(Session().answer_yes_for == fresh.answer_yes_for);
  CHECK_F02(Session().start_episode_used == fresh.start_episode_used);
  CHECK_F02(Session().boot_episode == fresh.boot_episode);
  CHECK_F02(Session().in_start_steps == fresh.in_start_steps);
}

void RepeatedHubCyclesEachUseTheirChoice() {
  Session().Reset();
  const int32_t choices[] = {0, 1, 2, 0, 2, 1};
  for (const int32_t choice : choices) {
    CHECK_F02(StartSessionWithChoice(choice));
    CHECK_F02(Session().boot_episode == choice);
    // The start's steps complete and clear the scan state.
    Session().in_start_steps = true;
    Session().in_start_steps = false;
    Session().boot_episode = -1;
    Session().Reset();  // Return to Hub
  }
}

void ColdStartWithNoHomeChoiceStaysRetail() {
  Session().Reset();
  CHECK_F02(!Session().ConsumeHomeChoice(-1));
  CHECK_F02(!Session().start_episode_used);
}

}  // namespace

#ifdef GTA4_F02_STANDALONE
int main() {
  RUN_F02(FirstSessionUsesTheHomeChoice);
  RUN_F02(SecondSessionAfterReturnUsesItsOwnChoice);
  RUN_F02(WithoutTheResetTheSecondChoiceIsIgnored);
  RUN_F02(ChoiceIsConsumedOncePerSession);
  RUN_F02(NoChoiceLeavesRetailBehavior);
  RUN_F02(ResetClearsEveryField);
  RUN_F02(RepeatedHubCyclesEachUseTheirChoice);
  RUN_F02(ColdStartWithNoHomeChoiceStaysRetail);
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
#else
TEST_CASE("F02 episode startup state", "[unit][f02]") {
  RUN_F02(FirstSessionUsesTheHomeChoice);
  RUN_F02(SecondSessionAfterReturnUsesItsOwnChoice);
  RUN_F02(WithoutTheResetTheSecondChoiceIsIgnored);
  RUN_F02(ChoiceIsConsumedOncePerSession);
  RUN_F02(NoChoiceLeavesRetailBehavior);
  RUN_F02(ResetClearsEveryField);
  RUN_F02(RepeatedHubCyclesEachUseTheirChoice);
  RUN_F02(ColdStartWithNoHomeChoiceStaysRetail);
}
#endif
