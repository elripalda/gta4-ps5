// F02 Return to Hub: the decision model for the native pause-menu confirmation.
//
// The confirmation itself is the retail message box (see gta4_frontend_hooks.cpp):
// the guest pause frame stops running while the box is up, so this model is the
// only reader of the pad during that time. It is dependency-free so the edge
// rules can be tested without a console, a guest, or ImGui.
//
// The press that selected the row must never count as an answer: `Begin`
// latches whatever is held at that moment. Every answer needs a fresh press,
// and after a cancellation both buttons must be released before the retail
// pause menu is allowed to run again, so a held Circle cannot close the menu.
#pragma once

#include <cstdint>

namespace gta4::ps5::lifecycle::f02 {

// The Xbox button bits the confirmation answers to (the merged UI pad).
constexpr std::uint16_t kConfirmButton = 0x1000;  // Cross / Accept
constexpr std::uint16_t kCancelButton = 0x2000;   // Circle / Cancel

enum class ConfirmState : std::uint8_t {
  kIdle,          // no confirmation; the retail pause frame runs normally
  kOpen,          // the native message box is up, waiting for an answer
  kAwaitRelease,  // dismissed; the retail pause frame stays frozen until the pad is released
  kCommitted,     // one return request is posted; the session is draining
};

enum class ConfirmAction : std::uint8_t {
  kNone,
  kCommit,   // exactly once: post the return request
  kDismiss,  // close the box and resume the Game page
};

class ReturnConfirmation {
 public:
  // Starts a confirmation. `held_buttons` is the merged pad sampled at the
  // moment of the answer; those buttons are latched so they are not edges.
  // Returns false when a confirmation is already active.
  bool Begin(std::uint16_t held_buttons) {
    if (active()) {
      return false;
    }
    state_ = ConfirmState::kOpen;
    previous_ = held_buttons;
    return true;
  }

  // One pause frame. Returns kCommit only on the falling-to-rising edge of the
  // confirm button, and only once per confirmation.
  ConfirmAction Update(std::uint16_t buttons) {
    const std::uint16_t pressed = static_cast<std::uint16_t>(buttons & ~previous_);
    previous_ = buttons;
    switch (state_) {
      case ConfirmState::kIdle:
      case ConfirmState::kCommitted:
        return ConfirmAction::kNone;
      case ConfirmState::kOpen:
        if ((pressed & kCancelButton) != 0) {
          state_ = ConfirmState::kAwaitRelease;
          return ConfirmAction::kDismiss;
        }
        if ((pressed & kConfirmButton) != 0) {
          state_ = ConfirmState::kCommitted;
          return ConfirmAction::kCommit;
        }
        return ConfirmAction::kNone;
      case ConfirmState::kAwaitRelease:
        if ((buttons & (kConfirmButton | kCancelButton)) == 0) {
          state_ = ConfirmState::kIdle;
          return ConfirmAction::kNone;
        }
        return ConfirmAction::kNone;
    }
    return ConfirmAction::kNone;
  }

  // A new session, a refused return, or a lost pause system.
  void Reset() {
    state_ = ConfirmState::kIdle;
    previous_ = 0;
  }

  bool active() const { return state_ != ConfirmState::kIdle; }
  bool dialog_open() const { return state_ == ConfirmState::kOpen; }
  bool committed() const { return state_ == ConfirmState::kCommitted; }
  ConfirmState state() const { return state_; }

 private:
  ConfirmState state_ = ConfirmState::kIdle;
  std::uint16_t previous_ = 0;
};

}  // namespace gta4::ps5::lifecycle::f02
