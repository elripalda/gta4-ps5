/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026. All rights reserved.                                       *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

namespace rex::kernel::xboxkrnl {

// Polls the guest-visible header of VdHSIOCalibrationLock. The lock lives in
// guest SystemHeap memory, which is unmapped during session teardown; a worker
// that outlives the mapping faults on the first read after unmapping. Owners
// must therefore keep this object alive across the session and let Stop() join
// before guest memory is released. The worker checks the stop request before
// every guest-memory access.
class VdHsioLockWatcher {
 public:
  using ChangeHandler = std::function<void(uint32_t guest_addr, uint8_t old_type,
                                           uint8_t new_type, uint8_t old_spin,
                                           uint8_t new_spin, const uint8_t* raw)>;

  VdHsioLockWatcher() = default;
  ~VdHsioLockWatcher();

  VdHsioLockWatcher(const VdHsioLockWatcher&) = delete;
  VdHsioLockWatcher& operator=(const VdHsioLockWatcher&) = delete;

  // Starts polling, replacing any worker that is already running. Returns false
  // and leaves no worker behind when the arguments or thread creation fail.
  bool Start(uint32_t guest_addr, const volatile uint8_t* header, ChangeHandler on_change,
             std::chrono::milliseconds poll_interval = std::chrono::milliseconds(1));

  // Stops polling and joins the worker. Idempotent; safe without Start().
  void Stop();

  bool running() const { return running_.load(std::memory_order_acquire); }

  // Number of completed guest-memory sampling passes. Diagnostic accessor used
  // to prove that a stopped worker never touches the previous guest pointer.
  uint64_t guest_accesses() const { return guest_accesses_.load(std::memory_order_acquire); }

 private:
  void Run(uint32_t guest_addr, const volatile uint8_t* header, ChangeHandler on_change,
           std::chrono::milliseconds poll_interval);
  void StopLocked();

  std::mutex lifecycle_mutex_;
  std::mutex wait_mutex_;
  std::condition_variable wait_cv_;
  std::thread worker_;
  std::atomic<bool> stop_requested_{false};
  std::atomic<bool> running_{false};
  std::atomic<uint64_t> guest_accesses_{0};
};

}  // namespace rex::kernel::xboxkrnl
