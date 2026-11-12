// Host-side tests for the F02 Return to Hub lifecycle model.
//
// Builds either as part of the Catch2 unit suite or standalone; see the
// include path used by tests/unit/CMakeLists.txt.
#include <string>

#include "../../../gta4-recomp/src/ps5/lifecycle/f02_return_to_hub.h"

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

using namespace gta4::ps5::lifecycle::f02;

const Capabilities kAll = Capabilities::AllSupported();

ReturnToHubController Supported() { return ReturnToHubController(true, kAll); }

void ReachPauseMenu(ReturnToHubController& controller) {
  CHECK_F02(controller.state() == State::kHub);
  CHECK_F02(controller.Launch(Game::kGta4).result == Result::kAccepted);
  CHECK_F02(controller.OnEvent(Event::kLaunchCompleted).result == Result::kAccepted);
  CHECK_F02(controller.state() == State::kInGame);
  CHECK_F02(controller.OnEvent(Event::kPauseOpened).result == Result::kAccepted);
  CHECK_F02(controller.state() == State::kPauseMenu);
}

void ReachDraining(ReturnToHubController& controller) {
  ReachPauseMenu(controller);
  CHECK_F02(controller.OnEvent(Event::kReturnToHubRequested).result == Result::kAccepted);
  CHECK_F02(controller.state() == State::kConfirming);
  CHECK_F02(controller.OnEvent(Event::kConfirmAccepted).result == Result::kAccepted);
  CHECK_F02(controller.state() == State::kPreparing);
  CHECK_F02(controller.OnEvent(Event::kSaveBarrierCompleted).result == Result::kAccepted);
  CHECK_F02(controller.state() == State::kDraining);
}

void DisabledKeepsUpstreamBehavior() {
  ReturnToHubController controller;
  CHECK_F02(!controller.enabled());
  CHECK_F02(controller.state() == State::kDisabled);

  const Outcome exit = controller.OnEvent(Event::kGuestExitRequested);
  CHECK_F02(exit.result == Result::kAccepted);
  CHECK_F02(exit.reason == Reason::kExistingTerminationPath);
  CHECK_F02(exit.state == State::kDisabled);

  const Outcome request = controller.OnEvent(Event::kReturnToHubRequested);
  CHECK_F02(request.result == Result::kIgnored);
  CHECK_F02(request.reason == Reason::kFeatureDisabled);

  const Outcome launch = controller.Launch(Game::kGta4);
  CHECK_F02(launch.result == Result::kIgnored);
  CHECK_F02(launch.reason == Reason::kFeatureDisabled);
  CHECK_F02(controller.generation() == 0);
  CHECK_F02(controller.hub_returns() == 0);
}

void ProductionProfileRefusesTheRequest() {
  CHECK_F02(!Capabilities::Production().SupportsReturnToHub());
  CHECK_F02(!Capabilities::Production().Missing().empty());
  CHECK_F02(Capabilities::AllSupported().SupportsReturnToHub());
  CHECK_F02(Capabilities::AllSupported().Missing().empty());

  ReturnToHubController controller(true, Capabilities::Production());
  CHECK_F02(controller.state() == State::kHub);

  controller.OnEvent(Event::kLaunchCompleted);  // not applicable yet
  CHECK_F02(controller.state() == State::kHub);

  CHECK_F02(controller.Launch(Game::kLostAndDamned).result == Result::kAccepted);
  CHECK_F02(controller.OnEvent(Event::kLaunchCompleted).result == Result::kAccepted);
  CHECK_F02(controller.generation() == 1);
  CHECK_F02(controller.OnEvent(Event::kPauseOpened).result == Result::kAccepted);
  CHECK_F02(controller.state() == State::kPauseMenu);

  const Outcome request = controller.OnEvent(Event::kReturnToHubRequested);
  CHECK_F02(request.result == Result::kRejected);
  CHECK_F02(request.reason == Reason::kMissingCapabilities);
  CHECK_F02(request.state == State::kPauseMenu);

  // The game keeps running; nothing was torn down.
  CHECK_F02(controller.state() == State::kPauseMenu);
  CHECK_F02(controller.generation() == 1);
  CHECK_F02(controller.hub_returns() == 0);

  CHECK_F02(controller.OnEvent(Event::kCancelRequested).result == Result::kAccepted);
  CHECK_F02(controller.state() == State::kPauseMenu);
  CHECK_F02(controller.OnEvent(Event::kPauseClosed).result == Result::kAccepted);
  CHECK_F02(controller.state() == State::kInGame);
}

void HappyPathReturnsAndStartsTheNextTitle() {
  ReturnToHubController controller = Supported();
  ReachPauseMenu(controller);

  CHECK_F02(controller.OnEvent(Event::kReturnToHubRequested).result == Result::kAccepted);
  CHECK_F02(controller.state() == State::kConfirming);
  CHECK_F02(controller.OnEvent(Event::kConfirmAccepted).result == Result::kAccepted);
  CHECK_F02(controller.state() == State::kPreparing);
  CHECK_F02(controller.OnEvent(Event::kSaveBarrierCompleted).result == Result::kAccepted);
  CHECK_F02(controller.state() == State::kDraining);
  CHECK_F02(controller.OnEvent(Event::kDrainCompleted).result == Result::kAccepted);
  CHECK_F02(controller.state() == State::kTearingDown);
  CHECK_F02(controller.OnEvent(Event::kTeardownCompleted).result == Result::kAccepted);
  CHECK_F02(controller.state() == State::kHub);
  CHECK_F02(controller.hub_returns() == 1);
  CHECK_F02(controller.generation() == 1);

  CHECK_F02(controller.Launch(Game::kBalladOfGayTony).result == Result::kAccepted);
  CHECK_F02(controller.pending_game() == Game::kBalladOfGayTony);
  CHECK_F02(controller.state() == State::kLaunching);
  CHECK_F02(controller.OnEvent(Event::kLaunchCompleted).result == Result::kAccepted);
  CHECK_F02(controller.state() == State::kInGame);
  CHECK_F02(controller.generation() == 2);
  CHECK_F02(controller.hub_returns() == 1);
}

void CancelAtConfirmationStaysInTheGame() {
  ReturnToHubController controller = Supported();
  ReachPauseMenu(controller);

  CHECK_F02(controller.OnEvent(Event::kReturnToHubRequested).result == Result::kAccepted);
  CHECK_F02(controller.state() == State::kConfirming);
  const Outcome cancel = controller.OnEvent(Event::kCancelRequested);
  CHECK_F02(cancel.result == Result::kAccepted);
  CHECK_F02(cancel.reason == Reason::kCancelled);
  CHECK_F02(cancel.state == State::kPauseMenu);
  CHECK_F02(controller.generation() == 1);
  CHECK_F02(controller.hub_returns() == 0);

  CHECK_F02(controller.OnEvent(Event::kPauseClosed).result == Result::kAccepted);
  CHECK_F02(controller.state() == State::kInGame);
  CHECK_F02(controller.generation() == 1);
}

void CancelInPauseMenuIsHarmless() {
  ReturnToHubController controller = Supported();
  ReachPauseMenu(controller);

  const Outcome cancel = controller.OnEvent(Event::kCancelRequested);
  CHECK_F02(cancel.result == Result::kAccepted);
  CHECK_F02(cancel.reason == Reason::kCancelled);
  CHECK_F02(cancel.state == State::kPauseMenu);
  CHECK_F02(controller.hub_returns() == 0);
}

void CancelAfterConfirmationIsRejected() {
  ReturnToHubController controller = Supported();
  ReachPauseMenu(controller);
  CHECK_F02(controller.OnEvent(Event::kReturnToHubRequested).result == Result::kAccepted);
  CHECK_F02(controller.OnEvent(Event::kConfirmAccepted).result == Result::kAccepted);
  CHECK_F02(controller.state() == State::kPreparing);

  const Outcome cancel = controller.OnEvent(Event::kCancelRequested);
  CHECK_F02(cancel.result == Result::kRejected);
  CHECK_F02(cancel.reason == Reason::kCommitPointPassed);
  CHECK_F02(cancel.state == State::kPreparing);

  // The committed flow continues.
  CHECK_F02(controller.OnEvent(Event::kSaveBarrierCompleted).result == Result::kAccepted);
  CHECK_F02(controller.state() == State::kDraining);
}

void SaveBarrierFailureReturnsToTheGame() {
  ReturnToHubController controller = Supported();
  ReachPauseMenu(controller);
  CHECK_F02(controller.OnEvent(Event::kReturnToHubRequested).result == Result::kAccepted);
  CHECK_F02(controller.OnEvent(Event::kConfirmAccepted).result == Result::kAccepted);
  CHECK_F02(controller.state() == State::kPreparing);

  const Outcome failed = controller.OnEvent(Event::kSaveBarrierFailed);
  CHECK_F02(failed.result == Result::kAccepted);
  CHECK_F02(failed.reason == Reason::kSaveBarrierFailed);
  CHECK_F02(failed.state == State::kInGame);
  CHECK_F02(controller.generation() == 1);
  CHECK_F02(controller.hub_returns() == 0);

  // The session is intact and the request can be attempted again.
  CHECK_F02(controller.OnEvent(Event::kPauseOpened).result == Result::kAccepted);
  CHECK_F02(controller.OnEvent(Event::kReturnToHubRequested).result == Result::kAccepted);
  CHECK_F02(controller.state() == State::kConfirming);
}

void GuestExitMapsToConfirmation() {
  ReturnToHubController controller = Supported();
  CHECK_F02(controller.Launch(Game::kGta4).result == Result::kAccepted);
  CHECK_F02(controller.OnEvent(Event::kLaunchCompleted).result == Result::kAccepted);

  const Outcome exit = controller.OnEvent(Event::kGuestExitRequested);
  CHECK_F02(exit.result == Result::kAccepted);
  CHECK_F02(exit.state == State::kConfirming);
  CHECK_F02(controller.hub_returns() == 0);

  CHECK_F02(controller.OnEvent(Event::kCancelRequested).result == Result::kAccepted);
  CHECK_F02(controller.state() == State::kPauseMenu);
}

void ReturnRequiresThePauseMenu() {
  ReturnToHubController controller = Supported();
  CHECK_F02(controller.Launch(Game::kGta4).result == Result::kAccepted);
  CHECK_F02(controller.OnEvent(Event::kLaunchCompleted).result == Result::kAccepted);

  const Outcome request = controller.OnEvent(Event::kReturnToHubRequested);
  CHECK_F02(request.result == Result::kRejected);
  CHECK_F02(request.reason == Reason::kNotInPauseMenu);
  CHECK_F02(request.state == State::kInGame);
  CHECK_F02(controller.hub_returns() == 0);
}

void DrainFailureBlocksInsteadOfForcingThreads() {
  ReturnToHubController controller = Supported();
  ReachDraining(controller);

  const Outcome failed = controller.OnEvent(Event::kDrainFailed);
  CHECK_F02(failed.result == Result::kAccepted);
  CHECK_F02(failed.reason == Reason::kGuestDrainIncomplete);
  CHECK_F02(failed.state == State::kBlocked);
  CHECK_F02(controller.hub_returns() == 0);
  CHECK_F02(controller.generation() == 1);

  // A late teardown completion must not reach the hub after a failed drain.
  CHECK_F02(controller.OnEvent(Event::kTeardownCompleted).result == Result::kIgnored);
  CHECK_F02(controller.state() == State::kBlocked);
  CHECK_F02(controller.hub_returns() == 0);

  // Recovery is a host restart into the hub, not an in-session reset.
  CHECK_F02(controller.OnEvent(Event::kReset).result == Result::kAccepted);
  CHECK_F02(controller.state() == State::kHub);
  CHECK_F02(controller.generation() == 1);
  CHECK_F02(controller.hub_returns() == 0);
}

void TeardownFailureBlocksTheSession() {
  ReturnToHubController controller = Supported();
  ReachDraining(controller);
  CHECK_F02(controller.OnEvent(Event::kDrainCompleted).result == Result::kAccepted);
  CHECK_F02(controller.state() == State::kTearingDown);

  const Outcome failed = controller.OnEvent(Event::kTeardownFailed);
  CHECK_F02(failed.result == Result::kAccepted);
  CHECK_F02(failed.reason == Reason::kTeardownFailed);
  CHECK_F02(failed.state == State::kBlocked);
  CHECK_F02(controller.hub_returns() == 0);
}

void LaunchFailureReturnsToTheHub() {
  ReturnToHubController controller = Supported();
  CHECK_F02(controller.Launch(Game::kGta4).result == Result::kAccepted);
  CHECK_F02(controller.state() == State::kLaunching);

  const Outcome failed = controller.OnEvent(Event::kLaunchFailed);
  CHECK_F02(failed.result == Result::kAccepted);
  CHECK_F02(failed.reason == Reason::kLaunchFailed);
  CHECK_F02(failed.state == State::kHub);
  CHECK_F02(controller.generation() == 0);

  CHECK_F02(controller.Launch(Game::kLostAndDamned).result == Result::kAccepted);
  CHECK_F02(controller.OnEvent(Event::kLaunchCompleted).result == Result::kAccepted);
  CHECK_F02(controller.state() == State::kInGame);
  CHECK_F02(controller.generation() == 1);
}

void RepeatedReturnsKeepTheirOwnGenerations() {
  ReturnToHubController controller = Supported();

  CHECK_F02(controller.Launch(Game::kGta4).result == Result::kAccepted);
  CHECK_F02(controller.OnEvent(Event::kLaunchCompleted).result == Result::kAccepted);

  const Game order[] = {Game::kLostAndDamned, Game::kBalladOfGayTony, Game::kGta4};
  for (const Game next : order) {
    CHECK_F02(controller.OnEvent(Event::kPauseOpened).result == Result::kAccepted);
    CHECK_F02(controller.OnEvent(Event::kReturnToHubRequested).result == Result::kAccepted);
    CHECK_F02(controller.OnEvent(Event::kConfirmAccepted).result == Result::kAccepted);
    CHECK_F02(controller.OnEvent(Event::kSaveBarrierCompleted).result == Result::kAccepted);
    CHECK_F02(controller.OnEvent(Event::kDrainCompleted).result == Result::kAccepted);
    CHECK_F02(controller.OnEvent(Event::kTeardownCompleted).result == Result::kAccepted);
    CHECK_F02(controller.state() == State::kHub);

    CHECK_F02(controller.Launch(next).result == Result::kAccepted);
    CHECK_F02(controller.pending_game() == next);
    CHECK_F02(controller.OnEvent(Event::kLaunchCompleted).result == Result::kAccepted);
    CHECK_F02(controller.state() == State::kInGame);
  }

  CHECK_F02(controller.generation() == 4);
  CHECK_F02(controller.hub_returns() == 3);
}

void LaunchIsOnlyValidFromTheHub() {
  ReturnToHubController controller = Supported();
  CHECK_F02(controller.Launch(Game::kGta4).result == Result::kAccepted);
  CHECK_F02(controller.OnEvent(Event::kLaunchCompleted).result == Result::kAccepted);

  const Outcome launch = controller.Launch(Game::kLostAndDamned);
  CHECK_F02(launch.result == Result::kRejected);
  CHECK_F02(launch.reason == Reason::kNotApplicable);
  CHECK_F02(launch.state == State::kInGame);
  CHECK_F02(controller.pending_game() == Game::kGta4);
}

void EventsOutOfOrderAreIgnored() {
  ReturnToHubController controller = Supported();

  CHECK_F02(controller.OnEvent(Event::kConfirmAccepted).result == Result::kIgnored);
  CHECK_F02(controller.OnEvent(Event::kSaveBarrierCompleted).result == Result::kIgnored);
  CHECK_F02(controller.OnEvent(Event::kDrainCompleted).result == Result::kIgnored);
  CHECK_F02(controller.OnEvent(Event::kTeardownCompleted).result == Result::kIgnored);
  CHECK_F02(controller.OnEvent(Event::kLaunchCompleted).result == Result::kIgnored);
  CHECK_F02(controller.OnEvent(Event::kPauseOpened).result == Result::kIgnored);
  CHECK_F02(controller.state() == State::kHub);
  CHECK_F02(controller.hub_returns() == 0);
  CHECK_F02(controller.generation() == 0);

  const Outcome reset = controller.OnEvent(Event::kReset);
  CHECK_F02(reset.result == Result::kIgnored);
  CHECK_F02(controller.state() == State::kHub);
}

}  // namespace

#ifdef GTA4_F02_STANDALONE
int main() {
  RUN_F02(DisabledKeepsUpstreamBehavior);
  RUN_F02(ProductionProfileRefusesTheRequest);
  RUN_F02(HappyPathReturnsAndStartsTheNextTitle);
  RUN_F02(CancelAtConfirmationStaysInTheGame);
  RUN_F02(CancelInPauseMenuIsHarmless);
  RUN_F02(CancelAfterConfirmationIsRejected);
  RUN_F02(SaveBarrierFailureReturnsToTheGame);
  RUN_F02(GuestExitMapsToConfirmation);
  RUN_F02(ReturnRequiresThePauseMenu);
  RUN_F02(DrainFailureBlocksInsteadOfForcingThreads);
  RUN_F02(TeardownFailureBlocksTheSession);
  RUN_F02(LaunchFailureReturnsToTheHub);
  RUN_F02(RepeatedReturnsKeepTheirOwnGenerations);
  RUN_F02(LaunchIsOnlyValidFromTheHub);
  RUN_F02(EventsOutOfOrderAreIgnored);

  std::printf("%d checks, %d failed scenario(s)\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
#else
TEST_CASE("F02 disabled keeps upstream termination behavior") {
  RUN_F02(DisabledKeepsUpstreamBehavior);
}
TEST_CASE("F02 production capability profile refuses return to hub") {
  RUN_F02(ProductionProfileRefusesTheRequest);
}
TEST_CASE("F02 happy path returns to hub and starts the next title") {
  RUN_F02(HappyPathReturnsAndStartsTheNextTitle);
}
TEST_CASE("F02 cancel at confirmation stays in the game") {
  RUN_F02(CancelAtConfirmationStaysInTheGame);
}
TEST_CASE("F02 cancel in the pause menu is harmless") {
  RUN_F02(CancelInPauseMenuIsHarmless);
}
TEST_CASE("F02 cancel after confirmation is rejected") {
  RUN_F02(CancelAfterConfirmationIsRejected);
}
TEST_CASE("F02 save barrier failure returns to the game") {
  RUN_F02(SaveBarrierFailureReturnsToTheGame);
}
TEST_CASE("F02 guest exit maps to confirmation") {
  RUN_F02(GuestExitMapsToConfirmation);
}
TEST_CASE("F02 return to hub requires the pause menu") {
  RUN_F02(ReturnRequiresThePauseMenu);
}
TEST_CASE("F02 drain failure blocks instead of forcing threads") {
  RUN_F02(DrainFailureBlocksInsteadOfForcingThreads);
}
TEST_CASE("F02 teardown failure blocks the session") {
  RUN_F02(TeardownFailureBlocksTheSession);
}
TEST_CASE("F02 launch failure returns to the hub") {
  RUN_F02(LaunchFailureReturnsToTheHub);
}
TEST_CASE("F02 repeated returns keep their own generations") {
  RUN_F02(RepeatedReturnsKeepTheirOwnGenerations);
}
TEST_CASE("F02 launch is only valid from the hub") {
  RUN_F02(LaunchIsOnlyValidFromTheHub);
}
TEST_CASE("F02 events out of order are ignored") {
  RUN_F02(EventsOutOfOrderAreIgnored);
}
#endif
