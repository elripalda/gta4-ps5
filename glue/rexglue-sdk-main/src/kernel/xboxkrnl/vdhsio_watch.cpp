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

#include <rex/kernel/xboxkrnl/vdhsio_watch.h>

namespace rex::kernel::xboxkrnl {
namespace {

constexpr int kMaxChanges = 8;
constexpr uint8_t kInitialType = 1;
constexpr uint8_t kInitialSpin = 40;

}  // namespace

VdHsioLockWatcher::~VdHsioLockWatcher() {
  Stop();
}

bool VdHsioLockWatcher::Start(uint32_t guest_addr, const volatile uint8_t* header,
                              ChangeHandler on_change,
                              std::chrono::milliseconds poll_interval) {
  if (!header || !on_change) {
    return false;
  }
  std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
  StopLocked();
  stop_requested_.store(false, std::memory_order_release);
  try {
    worker_ = std::thread(&VdHsioLockWatcher::Run, this, guest_addr, header,
                          std::move(on_change), poll_interval);
  } catch (...) {
    return false;
  }
  running_.store(true, std::memory_order_release);
  return true;
}

void VdHsioLockWatcher::Stop() {
  std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
  StopLocked();
}

void VdHsioLockWatcher::StopLocked() {
  stop_requested_.store(true, std::memory_order_release);
  wait_cv_.notify_all();
  if (!worker_.joinable()) {
    running_.store(false, std::memory_order_release);
    return;
  }
  // The owner must stop from a non-worker thread; joining self would deadlock.
  if (worker_.get_id() == std::this_thread::get_id()) {
    return;
  }
  worker_.join();
  running_.store(false, std::memory_order_release);
}

void VdHsioLockWatcher::Run(uint32_t guest_addr, const volatile uint8_t* header,
                            ChangeHandler on_change,
                            std::chrono::milliseconds poll_interval) {
  uint8_t last_type = kInitialType;
  uint8_t last_spin = kInitialSpin;
  int changes = 0;
  for (;;) {
    {
      std::unique_lock<std::mutex> lock(wait_mutex_);
      wait_cv_.wait_for(lock, poll_interval, [this] {
        return stop_requested_.load(std::memory_order_acquire);
      });
    }
    if (stop_requested_.load(std::memory_order_acquire)) {
      break;
    }

    guest_accesses_.fetch_add(1, std::memory_order_acq_rel);
    const uint8_t type = header[0];
    const uint8_t spin = header[1];
    if (type == last_type && spin == last_spin) {
      continue;
    }

    uint8_t raw[8];
    for (int i = 0; i < 8; ++i) {
      raw[i] = header[8 + i];
    }
    on_change(guest_addr, last_type, type, last_spin, spin, raw);
    last_type = type;
    last_spin = spin;
    if (++changes >= kMaxChanges) {
      break;
    }
  }
  running_.store(false, std::memory_order_release);
}

}  // namespace rex::kernel::xboxkrnl
