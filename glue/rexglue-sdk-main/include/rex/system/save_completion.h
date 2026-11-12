// Pending-save completion barrier and shutdown admission gate.
//
// A save is pending while a saved-game content operation is executing or a
// write handle on a save-backed mount is open. WaitForSaveCompletion polls
// that snapshot until it is clean or the timeout elapses. A timeout is
// reported as incomplete, never as success; the last snapshot is returned so
// callers can name the files still open.
//
// For an authorized shutdown there is a second requirement: no new save
// transaction may be admitted once the commit phase begins. SaveQuiescenceGate
// provides the admission side, and WaitForSaveQuiescence requires the gate to
// be closed plus no executing operation, no open write handle, and no mounted
// save content root. Closing the gate is what makes "no work remains" stable:
// without it, a save could be admitted immediately after a clean snapshot.
//
// The clock and sleep are injected so both policies are host-testable without
// real waits.
#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace rex::system {

struct SaveActivitySnapshot {
  uint32_t pending_save_operations = 0;
  // Saved-game content roots still mounted, even when no file is open. A
  // mounted root means the guest may still issue a write for that content, so
  // quiescence waits for it to close.
  uint32_t open_save_contents = 0;
  uint32_t open_write_handles = 0;
  // True once the admission gate has entered its commit phase.
  bool save_admission_closed = false;
  // Sorted, unique host paths of files still open for writing.
  std::vector<std::string> pending_files;
  // Sorted, unique root names of saved-game content still mounted.
  std::vector<std::string> open_content_roots;

  // Host completion: every admitted save operation has returned and every
  // write handle has been closed (which flushes the host file).
  bool clean() const { return pending_save_operations == 0 && open_write_handles == 0; }

  // Shutdown quiescence: admission is closed, host writes are complete, and no
  // save content root is mounted that the guest could still write through.
  // This is an observable runtime contract, not proof that the guest will
  // never attempt another save; such an attempt is rejected by the gate.
  bool quiescent() const {
    return save_admission_closed && clean() && open_save_contents == 0;
  }
};

struct SaveWaitResult {
  bool clean = false;
  SaveActivitySnapshot last;
  std::chrono::milliseconds waited{0};
};

// Admission gate for saved-game content transactions.
//
// TryBegin() and EnterCommitPhase() share one mutex, so after
// EnterCommitPhase() returns, any transaction that could still start was
// already counted. Transactions admitted before the commit phase keep running
// and stay visible to SaveActivitySnapshot until End() is called.
class SaveQuiescenceGate {
 public:
  // Admits a new transaction unless the commit phase has begun.
  bool TryBegin() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (commit_phase_) {
      return false;
    }
    ++pending_;
    return true;
  }

  // Counts a transaction that must not be rejected (for example, closing
  // content that was already mounted).
  void Begin() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++pending_;
  }

  void End() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pending_ > 0) {
      --pending_;
    }
  }

  // Idempotent. Repeated shutdown requests leave the gate closed.
  void EnterCommitPhase() {
    std::lock_guard<std::mutex> lock(mutex_);
    commit_phase_ = true;
  }

  bool commit_phase() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return commit_phase_;
  }

  uint32_t pending() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_;
  }

 private:
  mutable std::mutex mutex_;
  bool commit_phase_ = false;
  uint32_t pending_ = 0;
};

namespace detail {

template <typename Snapshot, typename Now, typename SleepFor, typename Satisfied>
SaveWaitResult WaitForSaveState(Snapshot snapshot, std::chrono::milliseconds timeout, Now now,
                                SleepFor sleep_for, Satisfied satisfied) {
  const auto start = now();
  const auto deadline = start + timeout;
  SaveWaitResult result;

  for (;;) {
    result.last = snapshot();
    if (satisfied(result.last)) {
      result.clean = true;
      break;
    }
    if (now() >= deadline) {
      break;
    }
    sleep_for(std::chrono::milliseconds(1));
  }

  result.waited = std::chrono::duration_cast<std::chrono::milliseconds>(now() - start);
  return result;
}

}  // namespace detail

// Waits until no save operation is executing and no write handle is open.
// Admission state is not considered; this is the B5 host-completion barrier.
template <typename Snapshot, typename Now, typename SleepFor>
SaveWaitResult WaitForSaveCompletion(Snapshot snapshot, std::chrono::milliseconds timeout,
                                     Now now, SleepFor sleep_for) {
  return detail::WaitForSaveState(
      std::move(snapshot), timeout, std::move(now), std::move(sleep_for),
      [](const SaveActivitySnapshot& state) { return state.clean(); });
}

// Waits until admission is closed and every tracked save has completed. An
// open admission gate means the wait can never succeed: a save could still be
// admitted after the snapshot, so a timeout with
// last.save_admission_closed == false reports a caller error, not a clean
// shutdown.
template <typename Snapshot, typename Now, typename SleepFor>
SaveWaitResult WaitForSaveQuiescence(Snapshot snapshot, std::chrono::milliseconds timeout,
                                     Now now, SleepFor sleep_for) {
  return detail::WaitForSaveState(
      std::move(snapshot), timeout, std::move(now), std::move(sleep_for),
      [](const SaveActivitySnapshot& state) { return state.quiescent(); });
}

}  // namespace rex::system
