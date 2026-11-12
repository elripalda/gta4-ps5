// Host-side tests for the F02 native pause-menu return confirmation.
//
// The entry point is the retail Pause Menu -> Game page row (tested in
// f02_game_menu_test.cpp); this file tests the decision model that runs while
// the retail message box is up: which press answers it, and that an answer is
// produced at most once.
#include <cstdint>
#include <string>

#include "../../../gta4-recomp/src/ps5/lifecycle/f02_return_prompt.h"

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

using gta4::ps5::lifecycle::f02::ConfirmAction;
using gta4::ps5::lifecycle::f02::ConfirmState;
using gta4::ps5::lifecycle::f02::kCancelButton;
using gta4::ps5::lifecycle::f02::kConfirmButton;
using gta4::ps5::lifecycle::f02::ReturnConfirmation;

void IdleIgnoresThePad() {
  ReturnConfirmation confirmation;
  CHECK_F02(!confirmation.active());
  CHECK_F02(confirmation.state() == ConfirmState::kIdle);
  CHECK_F02(confirmation.Update(kConfirmButton) == ConfirmAction::kNone);
  CHECK_F02(confirmation.Update(kCancelButton) == ConfirmAction::kNone);
  CHECK_F02(!confirmation.active());
}

void BeginOpensTheDialogState() {
  ReturnConfirmation confirmation;
  CHECK_F02(confirmation.Begin(0));
  CHECK_F02(confirmation.active());
  CHECK_F02(confirmation.dialog_open());
  CHECK_F02(!confirmation.committed());
  CHECK_F02(confirmation.state() == ConfirmState::kOpen);
}

void BeginRefusesASecondConfirmation() {
  ReturnConfirmation confirmation;
  CHECK_F02(confirmation.Begin(0));
  CHECK_F02(!confirmation.Begin(0));
  CHECK_F02(confirmation.state() == ConfirmState::kOpen);
  // A committed confirmation is active too: the open dialog must not be
  // replaced while the session drains.
  confirmation.Update(kConfirmButton);
  CHECK_F02(confirmation.committed());
  CHECK_F02(!confirmation.Begin(0));
  CHECK_F02(confirmation.committed());
}

void CrossCommitsExactlyOnce() {
  ReturnConfirmation confirmation;
  confirmation.Begin(0);
  CHECK_F02(confirmation.Update(kConfirmButton) == ConfirmAction::kCommit);
  CHECK_F02(confirmation.committed());
  CHECK_F02(!confirmation.dialog_open());
  // Held and repeated presses never dispatch a second return.
  CHECK_F02(confirmation.Update(kConfirmButton) == ConfirmAction::kNone);
  CHECK_F02(confirmation.Update(0) == ConfirmAction::kNone);
  CHECK_F02(confirmation.Update(kConfirmButton) == ConfirmAction::kNone);
  CHECK_F02(confirmation.committed());
}

void CircleDismissesAndFreezesUntilRelease() {
  ReturnConfirmation confirmation;
  confirmation.Begin(0);
  CHECK_F02(confirmation.Update(kCancelButton) == ConfirmAction::kDismiss);
  CHECK_F02(confirmation.state() == ConfirmState::kAwaitRelease);
  CHECK_F02(confirmation.active());
  // The pause frame stays frozen (active) while Circle is still held, so the
  // retail menu does not consume the same press.
  for (int frame = 0; frame < 30; ++frame) {
    CHECK_F02(confirmation.Update(kCancelButton) == ConfirmAction::kNone);
    CHECK_F02(confirmation.active());
  }
  // A Cross press during the release window must not turn the dismissal into a
  // return.
  CHECK_F02(confirmation.Update(kConfirmButton) == ConfirmAction::kNone);
  CHECK_F02(!confirmation.committed());
  // Both buttons up: back to idle, the pause frame may run again.
  CHECK_F02(confirmation.Update(0) == ConfirmAction::kNone);
  CHECK_F02(confirmation.state() == ConfirmState::kIdle);
  CHECK_F02(!confirmation.active());
}

void HoldingCrossFromTheRowSelectionIsNotAnAnswer() {
  ReturnConfirmation confirmation;
  // The row was activated with Cross held; Begin latches it.
  CHECK_F02(confirmation.Begin(kConfirmButton));
  CHECK_F02(confirmation.Update(kConfirmButton) == ConfirmAction::kNone);
  CHECK_F02(confirmation.dialog_open());
  // Releasing and pressing again is a fresh answer.
  CHECK_F02(confirmation.Update(0) == ConfirmAction::kNone);
  CHECK_F02(confirmation.Update(kConfirmButton) == ConfirmAction::kCommit);
}

void HoldingCircleFromTheRowSelectionIsNotAnAnswer() {
  ReturnConfirmation confirmation;
  CHECK_F02(confirmation.Begin(kCancelButton));
  CHECK_F02(confirmation.Update(kCancelButton) == ConfirmAction::kNone);
  CHECK_F02(confirmation.dialog_open());
  CHECK_F02(confirmation.Update(0) == ConfirmAction::kNone);
  CHECK_F02(confirmation.dialog_open());
}

void ConfirmedSessionCannotBeDismissed() {
  ReturnConfirmation confirmation;
  confirmation.Begin(0);
  confirmation.Update(kConfirmButton);
  CHECK_F02(confirmation.Update(kCancelButton) == ConfirmAction::kNone);
  CHECK_F02(confirmation.committed());
  CHECK_F02(confirmation.active());
}

void HeldButtonsNeverReleaseThePauseMenu() {
  ReturnConfirmation confirmation;
  confirmation.Begin(0);
  confirmation.Update(kCancelButton);
  for (int frame = 0; frame < 1200; ++frame) {
    confirmation.Update(kCancelButton | kConfirmButton);
    CHECK_F02(confirmation.state() == ConfirmState::kAwaitRelease);
  }
  CHECK_F02(!confirmation.committed());
  confirmation.Update(0);
  CHECK_F02(!confirmation.active());
}

void ResetReturnsToIdle() {
  ReturnConfirmation confirmation;
  confirmation.Begin(kConfirmButton);
  confirmation.Reset();
  CHECK_F02(confirmation.state() == ConfirmState::kIdle);
  CHECK_F02(!confirmation.active());
  // A new confirmation after the reset starts with pristine edges.
  CHECK_F02(confirmation.Begin(0));
  CHECK_F02(confirmation.Update(kConfirmButton) == ConfirmAction::kCommit);
}

void RepeatedCyclesAreIndependent() {
  ReturnConfirmation confirmation;
  for (int cycle = 0; cycle < 4; ++cycle) {
    CHECK_F02(confirmation.Begin(0));
    CHECK_F02(confirmation.Update(kCancelButton) == ConfirmAction::kDismiss);
    CHECK_F02(confirmation.Update(0) == ConfirmAction::kNone);
    CHECK_F02(!confirmation.active());
  }
  CHECK_F02(confirmation.Begin(0));
  CHECK_F02(confirmation.Update(kConfirmButton) == ConfirmAction::kCommit);
}

void CommittedSurvivesPauseMenuLoss() {
  ReturnConfirmation confirmation;
  confirmation.Begin(0);
  confirmation.Update(kConfirmButton);
  // The guest pause frame stops running once the host drains the session; the
  // model must stay committed (the host is already tearing down), never fall
  // back to idle on an empty update.
  CHECK_F02(confirmation.Update(0) == ConfirmAction::kNone);
  CHECK_F02(confirmation.committed());
}

}  // namespace

#ifdef GTA4_F02_STANDALONE
int main() {
  RUN_F02(IdleIgnoresThePad);
  RUN_F02(BeginOpensTheDialogState);
  RUN_F02(BeginRefusesASecondConfirmation);
  RUN_F02(CrossCommitsExactlyOnce);
  RUN_F02(CircleDismissesAndFreezesUntilRelease);
  RUN_F02(HoldingCrossFromTheRowSelectionIsNotAnAnswer);
  RUN_F02(HoldingCircleFromTheRowSelectionIsNotAnAnswer);
  RUN_F02(ConfirmedSessionCannotBeDismissed);
  RUN_F02(HeldButtonsNeverReleaseThePauseMenu);
  RUN_F02(ResetReturnsToIdle);
  RUN_F02(RepeatedCyclesAreIndependent);
  RUN_F02(CommittedSurvivesPauseMenuLoss);
  std::printf("f02_return_prompt: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
#else
TEST_CASE("F02 confirmation ignores the pad while idle") {
  RUN_F02(IdleIgnoresThePad);
}
TEST_CASE("F02 confirmation begins in the open dialog state") {
  RUN_F02(BeginOpensTheDialogState);
}
TEST_CASE("F02 confirmation refuses a second begin") {
  RUN_F02(BeginRefusesASecondConfirmation);
}
TEST_CASE("F02 Cross commits exactly one return") {
  RUN_F02(CrossCommitsExactlyOnce);
}
TEST_CASE("F02 Circle dismisses and freezes until release") {
  RUN_F02(CircleDismissesAndFreezesUntilRelease);
}
TEST_CASE("F02 Cross held from the row selection is not an answer") {
  RUN_F02(HoldingCrossFromTheRowSelectionIsNotAnAnswer);
}
TEST_CASE("F02 Circle held from the row selection is not an answer") {
  RUN_F02(HoldingCircleFromTheRowSelectionIsNotAnAnswer);
}
TEST_CASE("F02 a committed confirmation cannot be dismissed") {
  RUN_F02(ConfirmedSessionCannotBeDismissed);
}
TEST_CASE("F02 held buttons never release the pause menu") {
  RUN_F02(HeldButtonsNeverReleaseThePauseMenu);
}
TEST_CASE("F02 reset returns to idle") {
  RUN_F02(ResetReturnsToIdle);
}
TEST_CASE("F02 confirmation supports repeated cycles") {
  RUN_F02(RepeatedCyclesAreIndependent);
}
TEST_CASE("F02 committed confirmation survives the pause frame stopping") {
  RUN_F02(CommittedSurvivesPauseMenuLoss);
}
#endif
