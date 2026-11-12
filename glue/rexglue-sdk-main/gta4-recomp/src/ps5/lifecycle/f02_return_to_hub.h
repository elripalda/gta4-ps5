// F02 Return to Hub: session lifecycle model.
//
// A dependency-free description of the pause-menu -> hub -> new-session flow.
// It exists so the transitions, cancellation rules, and failure handling can be
// tested on the host without a PS5, a guest, or the graphics stack.
//
// This model is intentionally not wired into the runtime yet: every entry in
// Capabilities is false for production because the current runtime cannot
// satisfy the contract (see docs/F02_LIFECYCLE_DESIGN.md). A controller built
// with the production profile rejects a return request and leaves the game
// running; it never fakes teardown with exit(), forced thread cancellation, or
// partial resets.
#pragma once

#include <cstdint>
#include <string>

namespace gta4::ps5::lifecycle::f02 {

// The three titles the hub can start.
enum class Game : std::uint8_t {
  kGta4 = 0,
  kLostAndDamned = 1,
  kBalladOfGayTony = 2,
};

enum class State : std::uint8_t {
  kDisabled,    // flag off: upstream behavior, no hub flow
  kHub,         // hub shown, no guest session
  kInGame,      // guest session running
  kPauseMenu,   // pause menu open
  kConfirming,  // return-to-hub confirmation shown
  kPreparing,   // confirmed: pending saves being completed
  kDraining,    // guest threads draining cooperatively
  kTearingDown, // host subsystems being torn down
  kLaunching,   // selected title being started in a clean session
  kBlocked,     // unsafe to continue; process restart required
};

enum class Event : std::uint8_t {
  kGuestExitRequested,   // EXIT / XamLoaderTerminateTitle from guest code
  kPauseOpened,
  kPauseClosed,
  kReturnToHubRequested, // the pause-menu row
  kConfirmAccepted,
  kCancelRequested,      // the confirmation dialog's Cancel
  kSaveBarrierCompleted, // every admitted save is durable
  kSaveBarrierFailed,
  kDrainCompleted,       // every guest thread left voluntarily
  kDrainFailed,          // a thread did not leave; never forced, so this blocks
  kTeardownCompleted,
  kTeardownFailed,
  kLaunchCompleted,
  kLaunchFailed,
  kReset,                // host restarted into the hub after kBlocked
};

enum class Result : std::uint8_t {
  kAccepted,
  kRejected,
  kIgnored, // valid event, not meaningful in this state
  kInvalid, // not a known event
};

enum class Reason : std::uint8_t {
  kNone,
  kFeatureDisabled,
  kExistingTerminationPath,
  kNotApplicable,
  kNotInPauseMenu,
  kMissingCapabilities,
  kCommitPointPassed,
  kCancelled,
  kSaveBarrierFailed,
  kGuestDrainIncomplete,
  kTeardownFailed,
  kLaunchFailed,
  kBlocked,
  kReset,
};

// What the host runtime must be able to do for F02 to be safe. The production
// profile leaves every bit false: the current runtime can do none of these
// during a live session.
struct Capabilities {
  bool show_hub_without_guest = false;    // hub draws with no runtime alive
  bool barrier_pending_saves = false;     // complete/observe pending saves
  bool drain_guest_threads = false;       // guest threads leave voluntarily
  bool teardown_host_subsystems = false;  // graphics/audio/input/runtime
  bool reinitialize_runtime = false;      // rebuild Runtime + factories
  bool reinitialize_title_state = false;  // reset hook/trophy/session statics

  static constexpr Capabilities Production() { return {}; }

  static constexpr Capabilities AllSupported() {
    return Capabilities{true, true, true, true, true, true};
  }

  constexpr bool SupportsReturnToHub() const {
    return show_hub_without_guest && barrier_pending_saves &&
           drain_guest_threads && teardown_host_subsystems &&
           reinitialize_runtime && reinitialize_title_state;
  }

  std::string Missing() const {
    std::string missing;
    const auto add = [&missing](bool present, const char* name) {
      if (present) return;
      if (!missing.empty()) missing += ',';
      missing += name;
    };
    add(show_hub_without_guest, "show_hub_without_guest");
    add(barrier_pending_saves, "barrier_pending_saves");
    add(drain_guest_threads, "drain_guest_threads");
    add(teardown_host_subsystems, "teardown_host_subsystems");
    add(reinitialize_runtime, "reinitialize_runtime");
    add(reinitialize_title_state, "reinitialize_title_state");
    return missing;
  }
};

struct Outcome {
  Result result = Result::kInvalid;
  State state = State::kDisabled;
  Reason reason = Reason::kNone;
};

// Encode the desired product behavior. Every unsafe transition is rejected
// before it can reach a teardown step; the only terminal states are kBlocked
// (a failed drain/teardown left the host in an unknown state) and kDisabled
// (the upstream exit path is unchanged).
class ReturnToHubController {
 public:
  explicit ReturnToHubController(bool enabled = false,
                                 Capabilities capabilities = Capabilities::Production())
      : enabled_(enabled), capabilities_(capabilities) {
    state_ = enabled_ ? State::kHub : State::kDisabled;
  }

  State state() const { return state_; }
  bool enabled() const { return enabled_; }
  const Capabilities& capabilities() const { return capabilities_; }
  std::uint32_t generation() const { return generation_; }
  std::uint32_t hub_returns() const { return hub_returns_; }
  Game pending_game() const { return pending_game_; }

  Outcome Launch(Game game) {
    if (!enabled_) {
      return {Result::kIgnored, state_, Reason::kFeatureDisabled};
    }
    if (state_ != State::kHub) {
      return {Result::kRejected, state_, Reason::kNotApplicable};
    }
    pending_game_ = game;
    state_ = State::kLaunching;
    return {Result::kAccepted, state_, Reason::kNone};
  }

  Outcome OnEvent(Event event) {
    if (!enabled_) {
      if (event == Event::kGuestExitRequested) {
        // The upstream path terminates the title/process; F02 adds nothing.
        return {Result::kAccepted, state_, Reason::kExistingTerminationPath};
      }
      if (event == Event::kReset) {
        return {Result::kAccepted, state_, Reason::kFeatureDisabled};
      }
      return {Result::kIgnored, state_, Reason::kFeatureDisabled};
    }

    switch (state_) {
      case State::kDisabled:
        return {Result::kIgnored, state_, Reason::kFeatureDisabled};

      case State::kBlocked:
        // Only a host restart into the hub can leave this state.
        if (event == Event::kReset) {
          state_ = State::kHub;
          return {Result::kAccepted, state_, Reason::kReset};
        }
        return {Result::kIgnored, state_, Reason::kBlocked};

      case State::kHub:
        return InHub(event);

      case State::kInGame:
        return InGame(event);

      case State::kPauseMenu:
        return InPauseMenu(event);

      case State::kConfirming:
        return Confirming(event);

      case State::kPreparing:
        return Preparing(event);

      case State::kDraining:
        return Draining(event);

      case State::kTearingDown:
        return TearingDown(event);

      case State::kLaunching:
        return Launching(event);
    }
    return {Result::kInvalid, state_, Reason::kNotApplicable};
  }

 private:
  Outcome InHub(Event) {
    return {Result::kIgnored, state_, Reason::kNotApplicable};
  }

  Outcome InGame(Event event) {
    switch (event) {
      case Event::kPauseOpened:
        state_ = State::kPauseMenu;
        return {Result::kAccepted, state_, Reason::kNone};
      case Event::kGuestExitRequested:
        // EXIT maps onto the same confirmed flow; it does not skip it.
        state_ = State::kConfirming;
        return {Result::kAccepted, state_, Reason::kNone};
      case Event::kReturnToHubRequested:
        return {Result::kRejected, state_, Reason::kNotInPauseMenu};
      default:
        return {Result::kIgnored, state_, Reason::kNotApplicable};
    }
  }

  Outcome InPauseMenu(Event event) {
    switch (event) {
      case Event::kPauseClosed:
        state_ = State::kInGame;
        return {Result::kAccepted, state_, Reason::kNone};
      case Event::kCancelRequested:
        return {Result::kAccepted, state_, Reason::kCancelled};
      case Event::kReturnToHubRequested:
        if (!capabilities_.SupportsReturnToHub()) {
          // Refuse the row rather than start an unsafe teardown; the game
          // keeps running exactly where it was.
          return {Result::kRejected, state_, Reason::kMissingCapabilities};
        }
        state_ = State::kConfirming;
        return {Result::kAccepted, state_, Reason::kNone};
      default:
        return {Result::kIgnored, state_, Reason::kNotApplicable};
    }
  }

  Outcome Confirming(Event event) {
    switch (event) {
      case Event::kCancelRequested:
        state_ = State::kPauseMenu;
        return {Result::kAccepted, state_, Reason::kCancelled};
      case Event::kConfirmAccepted:
        if (!capabilities_.SupportsReturnToHub()) {
          return {Result::kRejected, state_, Reason::kMissingCapabilities};
        }
        state_ = State::kPreparing;
        return {Result::kAccepted, state_, Reason::kNone};
      default:
        return {Result::kIgnored, state_, Reason::kNotApplicable};
    }
  }

  Outcome Preparing(Event event) {
    switch (event) {
      case Event::kSaveBarrierCompleted:
        state_ = State::kDraining;
        return {Result::kAccepted, state_, Reason::kNone};
      case Event::kSaveBarrierFailed:
        // Nothing has been torn down yet, so the session can continue.
        state_ = State::kInGame;
        return {Result::kAccepted, state_, Reason::kSaveBarrierFailed};
      case Event::kCancelRequested:
        // Saves may be in flight; aborting them is not a safe cancellation.
        return {Result::kRejected, state_, Reason::kCommitPointPassed};
      default:
        return {Result::kIgnored, state_, Reason::kNotApplicable};
    }
  }

  Outcome Draining(Event event) {
    switch (event) {
      case Event::kDrainCompleted:
        state_ = State::kTearingDown;
        return {Result::kAccepted, state_, Reason::kNone};
      case Event::kDrainFailed:
        // A straggler guest thread still owns host state. Never force it.
        state_ = State::kBlocked;
        return {Result::kAccepted, state_, Reason::kGuestDrainIncomplete};
      default:
        return {Result::kIgnored, state_, Reason::kNotApplicable};
    }
  }

  Outcome TearingDown(Event event) {
    switch (event) {
      case Event::kTeardownCompleted:
        state_ = State::kHub;
        ++hub_returns_;
        return {Result::kAccepted, state_, Reason::kNone};
      case Event::kTeardownFailed:
        state_ = State::kBlocked;
        return {Result::kAccepted, state_, Reason::kTeardownFailed};
      default:
        return {Result::kIgnored, state_, Reason::kNotApplicable};
    }
  }

  Outcome Launching(Event event) {
    switch (event) {
      case Event::kLaunchCompleted:
        state_ = State::kInGame;
        ++generation_;
        return {Result::kAccepted, state_, Reason::kNone};
      case Event::kLaunchFailed:
        // The hub never went away, so it can offer the selection again.
        state_ = State::kHub;
        return {Result::kAccepted, state_, Reason::kLaunchFailed};
      case Event::kGuestExitRequested:
        return {Result::kIgnored, state_, Reason::kNotApplicable};
      default:
        return {Result::kIgnored, state_, Reason::kNotApplicable};
    }
  }

  bool enabled_ = false;
  Capabilities capabilities_;
  State state_ = State::kDisabled;
  Game pending_game_ = Game::kGta4;
  std::uint32_t generation_ = 0;
  std::uint32_t hub_returns_ = 0;
};

}  // namespace gta4::ps5::lifecycle::f02
