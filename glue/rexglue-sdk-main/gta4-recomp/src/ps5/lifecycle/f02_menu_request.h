// F02 Return to Hub: the hand-off from the retail pause-menu row to the UI
// overlay.
//
// The row is activated on the guest/module thread inside the recompiled menu
// dispatcher. The confirmation and the session teardown run on the UI thread.
// A counted request (instead of a plain flag) keeps repeated activations from
// being merged and is consumed exactly once; the overlay polls it every game
// frame, so a request cannot survive into a later pause.
#pragma once

#include <atomic>
#include <cstdint>

namespace gta4::ps5::lifecycle::f02 {

inline std::atomic<uint32_t>& MenuRequestCounter() {
  static std::atomic<uint32_t> counter{0};
  return counter;
}

inline void RequestReturnFromMenu() {
  MenuRequestCounter().fetch_add(1, std::memory_order_release);
}

// True once per request; a stale request (pause menu already gone) is dropped
// by the caller.
inline bool ConsumeReturnRequest() {
  return MenuRequestCounter().exchange(0, std::memory_order_acq_rel) != 0;
}

inline void ResetReturnRequest() {
  MenuRequestCounter().store(0, std::memory_order_relaxed);
}

// The confirmation controller runs in the guest pause frame; the host may need
// it dropped (the return was refused, or the committed session could not be
// torn down) or cleared for a new session. A counted request, consumed on the
// guest thread, keeps the host from touching guest state directly.
inline std::atomic<uint32_t>& ConfirmationResetCounter() {
  static std::atomic<uint32_t> counter{0};
  return counter;
}

inline void RequestConfirmationReset() {
  ConfirmationResetCounter().fetch_add(1, std::memory_order_release);
}

inline bool ConsumeConfirmationReset() {
  return ConfirmationResetCounter().exchange(0, std::memory_order_acq_rel) != 0;
}

}  // namespace gta4::ps5::lifecycle::f02
