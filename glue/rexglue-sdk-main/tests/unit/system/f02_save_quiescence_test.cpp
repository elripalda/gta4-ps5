// Host tests for the save-quiescence admission gate and shutdown barrier.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include <rex/system/save_completion.h>

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

using rex::system::SaveActivitySnapshot;
using rex::system::SaveQuiescenceGate;
using rex::system::SaveWaitResult;
using rex::system::WaitForSaveCompletion;
using rex::system::WaitForSaveQuiescence;
using std::chrono::milliseconds;

// Deterministic clock: only the barrier's injected sleep advances it.
struct FakeClock {
  milliseconds now{0};

  milliseconds Now() const { return now; }

  void Sleep(milliseconds delay) { now += delay; }
};

SaveActivitySnapshot ClosedIdleSnapshot(bool closed = true) {
  SaveActivitySnapshot snapshot;
  snapshot.save_admission_closed = closed;
  return snapshot;
}

void GateAdmitsWhileOpenAndCountsTheOperation() {
  SaveQuiescenceGate gate;
  CHECK_F02(!gate.commit_phase());
  CHECK_F02(gate.pending() == 0);
  CHECK_F02(gate.TryBegin());
  CHECK_F02(gate.pending() == 1);
  gate.End();
  CHECK_F02(gate.pending() == 0);
}

void ClosedGateRejectsNewOperations() {
  SaveQuiescenceGate gate;
  gate.EnterCommitPhase();
  CHECK_F02(gate.commit_phase());
  CHECK_F02(!gate.TryBegin());
  CHECK_F02(gate.pending() == 0);
}

void CommitPhaseIsIdempotent() {
  SaveQuiescenceGate gate;
  gate.EnterCommitPhase();
  gate.EnterCommitPhase();
  gate.EnterCommitPhase();
  CHECK_F02(gate.commit_phase());
  CHECK_F02(!gate.TryBegin());
}

void AdmittedOperationStaysVisibleAfterTheGateCloses() {
  SaveQuiescenceGate gate;
  CHECK_F02(gate.TryBegin());
  gate.EnterCommitPhase();
  // The admission happened before the close, so the operation is still
  // counted and must finish on its own.
  CHECK_F02(gate.pending() == 1);
  CHECK_F02(!gate.TryBegin());
  gate.End();
  CHECK_F02(gate.pending() == 0);
}

void NestedAdmissionsEndIndependently() {
  SaveQuiescenceGate gate;
  CHECK_F02(gate.TryBegin());
  CHECK_F02(gate.TryBegin());
  CHECK_F02(gate.pending() == 2);
  gate.End();
  CHECK_F02(gate.pending() == 1);
  gate.End();
  CHECK_F02(gate.pending() == 0);
  // A stray end must not corrupt the count.
  gate.End();
  CHECK_F02(gate.pending() == 0);
}

void UncheckedBeginCountsEvenAfterTheGateCloses() {
  SaveQuiescenceGate gate;
  gate.EnterCommitPhase();
  gate.Begin();
  CHECK_F02(gate.pending() == 1);
  gate.End();
  CHECK_F02(gate.pending() == 0);
}

void SnapshotQuiescenceRequiresEveryCondition() {
  SaveActivitySnapshot snapshot = ClosedIdleSnapshot();
  CHECK_F02(snapshot.clean());
  CHECK_F02(snapshot.quiescent());

  snapshot = ClosedIdleSnapshot(false);
  CHECK_F02(snapshot.clean());
  CHECK_F02(!snapshot.quiescent());

  snapshot = ClosedIdleSnapshot();
  snapshot.pending_save_operations = 1;
  CHECK_F02(!snapshot.clean());
  CHECK_F02(!snapshot.quiescent());

  snapshot = ClosedIdleSnapshot();
  snapshot.open_write_handles = 1;
  CHECK_F02(!snapshot.clean());
  CHECK_F02(!snapshot.quiescent());

  snapshot = ClosedIdleSnapshot();
  snapshot.open_save_contents = 1;
  // Host writes are complete, but the mounted root can still be written to.
  CHECK_F02(snapshot.clean());
  CHECK_F02(!snapshot.quiescent());
}

void OpenAdmissionNeverReportsQuiescence() {
  FakeClock clock;
  // Completely idle, but a save could still be admitted after the snapshot, so
  // the shutdown barrier must not succeed.
  const SaveWaitResult result =
      WaitForSaveQuiescence([] { return ClosedIdleSnapshot(false); }, milliseconds(5),
                            [&] { return clock.Now(); }, [&](milliseconds d) { clock.Sleep(d); });
  CHECK_F02(!result.clean);
  CHECK_F02(result.last.clean());
  CHECK_F02(!result.last.quiescent());
  CHECK_F02(result.waited == milliseconds(5));
}

void PendingOperationBlocksQuiescence() {
  FakeClock clock;
  const auto snapshot = [] {
    auto value = ClosedIdleSnapshot();
    value.pending_save_operations = 1;
    return value;
  };
  const SaveWaitResult result =
      WaitForSaveQuiescence(snapshot, milliseconds(4), [&] { return clock.Now(); },
                            [&](milliseconds d) { clock.Sleep(d); });
  CHECK_F02(!result.clean);
  CHECK_F02(result.last.pending_save_operations == 1);
  CHECK_F02(result.last.save_admission_closed);
  CHECK_F02(result.waited == milliseconds(4));
}

void OpenContentRootBlocksQuiescence() {
  FakeClock clock;
  const auto snapshot = [] {
    auto value = ClosedIdleSnapshot();
    value.open_save_contents = 1;
    value.open_content_roots = {"savegame"};
    return value;
  };
  const SaveWaitResult result =
      WaitForSaveQuiescence(snapshot, milliseconds(3), [&] { return clock.Now(); },
                            [&](milliseconds d) { clock.Sleep(d); });
  CHECK_F02(!result.clean);
  CHECK_F02(result.last.open_save_contents == 1);
  CHECK_F02(result.last.open_content_roots == std::vector<std::string>{"savegame"});
  CHECK_F02(result.waited == milliseconds(3));
}

void OpenWriteHandlesBlockQuiescence() {
  FakeClock clock;
  const auto snapshot = [] {
    auto value = ClosedIdleSnapshot();
    value.open_write_handles = 2;
    value.pending_files = {"saves/slot1.sav", "saves/slot2.sav"};
    return value;
  };
  const SaveWaitResult result =
      WaitForSaveQuiescence(snapshot, milliseconds(4), [&] { return clock.Now(); },
                            [&](milliseconds d) { clock.Sleep(d); });
  CHECK_F02(!result.clean);
  CHECK_F02(result.last.open_write_handles == 2);
  CHECK_F02(result.last.pending_files.size() == 2);
  CHECK_F02(result.waited == milliseconds(4));
}

void QueuedOperationThatRunsAfterCommitIsWaitedFor() {
  // Mirrors a deferred XamContentCreate callback: the operation was admitted
  // before the commit phase, then executes and ends while the barrier waits.
  SaveQuiescenceGate gate;
  CHECK_F02(gate.TryBegin());
  gate.EnterCommitPhase();

  FakeClock clock;
  const auto snapshot = [&gate] {
    auto value = ClosedIdleSnapshot(gate.commit_phase());
    value.pending_save_operations = gate.pending();
    return value;
  };
  const SaveWaitResult result = WaitForSaveQuiescence(
      snapshot, milliseconds(10), [&] { return clock.Now(); }, [&](milliseconds d) {
        clock.Sleep(d);
        if (clock.now >= milliseconds(3)) gate.End();
      });
  CHECK_F02(result.clean);
  CHECK_F02(result.last.pending_save_operations == 0);
  CHECK_F02(result.last.quiescent());
  CHECK_F02(result.waited == milliseconds(3));
}

void RejectedSaveLeavesNoPendingWork() {
  SaveQuiescenceGate gate;
  gate.EnterCommitPhase();
  // The XAM entry point rejects the transaction instead of admitting it.
  const bool admitted = gate.TryBegin();
  CHECK_F02(!admitted);
  CHECK_F02(gate.pending() == 0);

  FakeClock clock;
  const SaveWaitResult result = WaitForSaveQuiescence(
      [&gate] {
        auto value = ClosedIdleSnapshot(gate.commit_phase());
        value.pending_save_operations = gate.pending();
        return value;
      },
      milliseconds(0), [&] { return clock.Now(); }, [&](milliseconds d) { clock.Sleep(d); });
  CHECK_F02(result.clean);
  CHECK_F02(result.waited == milliseconds(0));
}

void RepeatedShutdownRequestsRecover() {
  SaveQuiescenceGate gate;
  CHECK_F02(gate.TryBegin());
  gate.EnterCommitPhase();
  gate.EnterCommitPhase();  // repeated request while a save is in flight

  FakeClock clock;
  const auto snapshot = [&gate] {
    auto value = ClosedIdleSnapshot(gate.commit_phase());
    value.pending_save_operations = gate.pending();
    return value;
  };

  const SaveWaitResult first =
      WaitForSaveQuiescence(snapshot, milliseconds(2), [&] { return clock.Now(); },
                            [&](milliseconds d) { clock.Sleep(d); });
  CHECK_F02(!first.clean);
  CHECK_F02(first.last.pending_save_operations == 1);

  gate.End();
  gate.EnterCommitPhase();  // a later request still observes the closed gate

  const SaveWaitResult second =
      WaitForSaveQuiescence(snapshot, milliseconds(2), [&] { return clock.Now(); },
                            [&](milliseconds d) { clock.Sleep(d); });
  CHECK_F02(second.clean);
  CHECK_F02(second.last.quiescent());
  CHECK_F02(second.waited == milliseconds(0));
}

void ZeroTimeoutNeverClaimsQuiescence() {
  FakeClock clock;
  const auto snapshot = [] {
    auto value = ClosedIdleSnapshot();
    value.pending_save_operations = 1;
    return value;
  };
  const SaveWaitResult result =
      WaitForSaveQuiescence(snapshot, milliseconds(0), [&] { return clock.Now(); },
                            [&](milliseconds d) { clock.Sleep(d); });
  CHECK_F02(!result.clean);
  CHECK_F02(result.waited == milliseconds(0));
}

void AdmissionStopsAsSoonAsTheCommitPhaseReturns() {
  SaveQuiescenceGate gate;
  constexpr int kThreads = 4;
  std::atomic<bool> stop{false};
  std::atomic<uint32_t> admitted{0};
  std::atomic<uint32_t> completed{0};

  std::vector<std::thread> workers;
  workers.reserve(kThreads);
  for (int i = 0; i < kThreads; ++i) {
    workers.emplace_back([&] {
      while (!stop.load(std::memory_order_relaxed)) {
        if (gate.TryBegin()) {
          admitted.fetch_add(1, std::memory_order_relaxed);
          gate.End();
          completed.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }

  // Let admissions run, then close the gate. EnterCommitPhase waits for any
  // in-flight TryBegin, so once it returns no transaction can start.
  std::this_thread::yield();
  gate.EnterCommitPhase();
  // A worker may still be finishing an admission that completed before the
  // close, so only new admissions are asserted here.
  for (int attempt = 0; attempt < 1000; ++attempt) {
    CHECK_F02(!gate.TryBegin());
  }

  stop.store(true, std::memory_order_relaxed);
  for (auto& worker : workers) {
    worker.join();
  }

  CHECK_F02(gate.pending() == 0);
  CHECK_F02(admitted.load() == completed.load());
  CHECK_F02(!gate.TryBegin());
}

void TimeoutReportsTheLastTrackedWork() {
  FakeClock clock;
  const auto snapshot = [] {
    auto value = ClosedIdleSnapshot();
    value.pending_save_operations = 1;
    value.open_write_handles = 1;
    value.open_save_contents = 1;
    value.pending_files = {"saves/slot1.sav"};
    value.open_content_roots = {"savegame"};
    return value;
  };
  const SaveWaitResult result =
      WaitForSaveQuiescence(snapshot, milliseconds(1), [&] { return clock.Now(); },
                            [&](milliseconds d) { clock.Sleep(d); });
  CHECK_F02(!result.clean);
  CHECK_F02(!result.last.clean());
  CHECK_F02(!result.last.quiescent());
  CHECK_F02(result.last.pending_save_operations == 1);
  CHECK_F02(result.last.open_write_handles == 1);
  CHECK_F02(result.last.open_save_contents == 1);
  CHECK_F02(result.last.pending_files == std::vector<std::string>{"saves/slot1.sav"});
  CHECK_F02(result.last.open_content_roots == std::vector<std::string>{"savegame"});
}

void CompletionBarrierStillIgnoresAdmissionState() {
  // B5 semantics are preserved: WaitForSaveCompletion reports host completion
  // even when the gate is still open or a root is mounted.
  FakeClock clock;
  uint32_t roots = 1;
  const auto snapshot = [&roots] {
    auto value = ClosedIdleSnapshot(false);
    value.open_save_contents = roots;
    return value;
  };
  const SaveWaitResult result =
      WaitForSaveCompletion(snapshot, milliseconds(2), [&] { return clock.Now(); },
                            [&](milliseconds d) { clock.Sleep(d); });
  CHECK_F02(result.clean);
  CHECK_F02(result.last.clean());
  CHECK_F02(!result.last.quiescent());
}

}  // namespace

#ifdef GTA4_F02_STANDALONE
int main() {
  RUN_F02(GateAdmitsWhileOpenAndCountsTheOperation);
  RUN_F02(ClosedGateRejectsNewOperations);
  RUN_F02(CommitPhaseIsIdempotent);
  RUN_F02(AdmittedOperationStaysVisibleAfterTheGateCloses);
  RUN_F02(NestedAdmissionsEndIndependently);
  RUN_F02(UncheckedBeginCountsEvenAfterTheGateCloses);
  RUN_F02(SnapshotQuiescenceRequiresEveryCondition);
  RUN_F02(OpenAdmissionNeverReportsQuiescence);
  RUN_F02(PendingOperationBlocksQuiescence);
  RUN_F02(OpenContentRootBlocksQuiescence);
  RUN_F02(OpenWriteHandlesBlockQuiescence);
  RUN_F02(QueuedOperationThatRunsAfterCommitIsWaitedFor);
  RUN_F02(RejectedSaveLeavesNoPendingWork);
  RUN_F02(RepeatedShutdownRequestsRecover);
  RUN_F02(ZeroTimeoutNeverClaimsQuiescence);
  RUN_F02(AdmissionStopsAsSoonAsTheCommitPhaseReturns);
  RUN_F02(TimeoutReportsTheLastTrackedWork);
  RUN_F02(CompletionBarrierStillIgnoresAdmissionState);

  std::printf("%d checks, %d failed scenario(s)\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
#else
TEST_CASE("F02 gate admits while open and counts the operation") {
  RUN_F02(GateAdmitsWhileOpenAndCountsTheOperation);
}
TEST_CASE("F02 gate rejects new operations once closed") { RUN_F02(ClosedGateRejectsNewOperations); }
TEST_CASE("F02 commit phase is idempotent") { RUN_F02(CommitPhaseIsIdempotent); }
TEST_CASE("F02 admitted operation stays visible after close") {
  RUN_F02(AdmittedOperationStaysVisibleAfterTheGateCloses);
}
TEST_CASE("F02 nested admissions end independently") { RUN_F02(NestedAdmissionsEndIndependently); }
TEST_CASE("F02 unchecked begin counts after close") {
  RUN_F02(UncheckedBeginCountsEvenAfterTheGateCloses);
}
TEST_CASE("F02 quiescence requires every condition") {
  RUN_F02(SnapshotQuiescenceRequiresEveryCondition);
}
TEST_CASE("F02 open admission never reports quiescence") {
  RUN_F02(OpenAdmissionNeverReportsQuiescence);
}
TEST_CASE("F02 pending operation blocks quiescence") { RUN_F02(PendingOperationBlocksQuiescence); }
TEST_CASE("F02 open content root blocks quiescence") { RUN_F02(OpenContentRootBlocksQuiescence); }
TEST_CASE("F02 open write handles block quiescence") { RUN_F02(OpenWriteHandlesBlockQuiescence); }
TEST_CASE("F02 queued operation that runs after commit is waited for") {
  RUN_F02(QueuedOperationThatRunsAfterCommitIsWaitedFor);
}
TEST_CASE("F02 rejected save leaves no pending work") { RUN_F02(RejectedSaveLeavesNoPendingWork); }
TEST_CASE("F02 repeated shutdown requests recover") { RUN_F02(RepeatedShutdownRequestsRecover); }
TEST_CASE("F02 zero timeout never claims quiescence") { RUN_F02(ZeroTimeoutNeverClaimsQuiescence); }
TEST_CASE("F02 admission stops as soon as the commit phase returns") {
  RUN_F02(AdmissionStopsAsSoonAsTheCommitPhaseReturns);
}
TEST_CASE("F02 timeout reports the last tracked work") { RUN_F02(TimeoutReportsTheLastTrackedWork); }
TEST_CASE("F02 completion barrier still ignores admission state") {
  RUN_F02(CompletionBarrierStillIgnoresAdmissionState);
}
#endif
