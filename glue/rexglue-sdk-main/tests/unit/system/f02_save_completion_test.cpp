// Host tests for the pending-save completion barrier and write tracker.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <rex/filesystem/write_activity_tracker.h>
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

using namespace rex::system;
using std::chrono::milliseconds;

// Deterministic clock: only the barrier's injected sleep advances it.
struct FakeClock {
  milliseconds now{0};

  milliseconds Now() const { return now; }

  void Sleep(milliseconds delay) { now += delay; }
};

SaveActivitySnapshot IdleSnapshot() { return SaveActivitySnapshot{}; }

SaveActivitySnapshot PendingOperations(uint32_t count, std::vector<std::string> files = {}) {
  SaveActivitySnapshot snapshot;
  snapshot.pending_save_operations = count;
  snapshot.pending_files = std::move(files);
  return snapshot;
}

SaveActivitySnapshot OpenWrites(uint32_t handles, std::vector<std::string> files) {
  SaveActivitySnapshot snapshot;
  snapshot.open_write_handles = handles;
  snapshot.pending_files = std::move(files);
  return snapshot;
}

void IdleStateIsClean() {
  FakeClock clock;
  const SaveWaitResult result =
      WaitForSaveCompletion([] { return IdleSnapshot(); }, milliseconds(10),
                            [&] { return clock.Now(); }, [&](milliseconds d) { clock.Sleep(d); });
  CHECK_F02(result.clean);
  CHECK_F02(result.last.clean());
  CHECK_F02(result.waited == milliseconds(0));
}

void PendingSaveOperationBlocksCompletionUntilTheDeadline() {
  FakeClock clock;
  const SaveWaitResult result =
      WaitForSaveCompletion([] { return PendingOperations(1); }, milliseconds(5),
                            [&] { return clock.Now(); }, [&](milliseconds d) { clock.Sleep(d); });
  CHECK_F02(!result.clean);
  CHECK_F02(result.last.pending_save_operations == 1);
  CHECK_F02(result.waited == milliseconds(5));
  CHECK_F02(!result.last.clean());
}

void OpenWriteHandlesBlockCompletionUntilTheDeadline() {
  FakeClock clock;
  const auto snapshot = [] {
    return OpenWrites(2, {"saves/slot1.sav", "saves/slot2.sav"});
  };
  const SaveWaitResult result =
      WaitForSaveCompletion(snapshot, milliseconds(4), [&] { return clock.Now(); },
                            [&](milliseconds d) { clock.Sleep(d); });
  CHECK_F02(!result.clean);
  CHECK_F02(result.last.open_write_handles == 2);
  CHECK_F02(result.last.pending_files.size() == 2);
  CHECK_F02(result.waited == milliseconds(4));
}

void SaveThatFinishesDuringTheWaitIsClean() {
  FakeClock clock;
  uint32_t remaining = 3;
  const auto snapshot = [&remaining] {
    SaveActivitySnapshot value;
    value.pending_save_operations = remaining;
    return value;
  };
  const SaveWaitResult result =
      WaitForSaveCompletion(snapshot, milliseconds(10), [&] { return clock.Now(); },
                            [&](milliseconds d) {
                              clock.Sleep(d);
                              if (clock.now >= milliseconds(2)) remaining = 0;
                            });
  CHECK_F02(result.clean);
  CHECK_F02(result.last.clean());
  CHECK_F02(result.waited == milliseconds(2));
}

void ZeroTimeoutNeverClaimsCompletion() {
  FakeClock clock;
  const SaveWaitResult result =
      WaitForSaveCompletion([] { return PendingOperations(1); }, milliseconds(0),
                            [&] { return clock.Now(); }, [&](milliseconds d) { clock.Sleep(d); });
  CHECK_F02(!result.clean);
  CHECK_F02(result.last.pending_save_operations == 1);
  CHECK_F02(result.waited == milliseconds(0));
}

void ZeroTimeoutWithNoPendingSaveIsClean() {
  FakeClock clock;
  const SaveWaitResult result =
      WaitForSaveCompletion([] { return IdleSnapshot(); }, milliseconds(0),
                            [&] { return clock.Now(); }, [&](milliseconds d) { clock.Sleep(d); });
  CHECK_F02(result.clean);
  CHECK_F02(result.waited == milliseconds(0));
}

void RepeatedWaitsRecoverAfterTheSaveFinishes() {
  FakeClock clock;
  uint32_t remaining = 1;
  const auto snapshot = [&remaining] {
    SaveActivitySnapshot value;
    value.open_write_handles = remaining;
    if (remaining > 0) value.pending_files = {"saves/slot1.sav"};
    return value;
  };

  const SaveWaitResult first =
      WaitForSaveCompletion(snapshot, milliseconds(3), [&] { return clock.Now(); },
                            [&](milliseconds d) { clock.Sleep(d); });
  CHECK_F02(!first.clean);
  CHECK_F02(first.last.pending_files == std::vector<std::string>{"saves/slot1.sav"});

  remaining = 0;
  const SaveWaitResult second =
      WaitForSaveCompletion(snapshot, milliseconds(3), [&] { return clock.Now(); },
                            [&](milliseconds d) { clock.Sleep(d); });
  CHECK_F02(second.clean);
  CHECK_F02(second.last.pending_files.empty());
  CHECK_F02(second.waited == milliseconds(0));
}

void RepeatedWaitsKeepReportingPendingSaves() {
  FakeClock clock;
  const auto snapshot = [] { return PendingOperations(2, {"a.sav", "b.sav"}); };
  for (int attempt = 0; attempt < 3; ++attempt) {
    const SaveWaitResult result =
        WaitForSaveCompletion(snapshot, milliseconds(2), [&] { return clock.Now(); },
                              [&](milliseconds d) { clock.Sleep(d); });
    CHECK_F02(!result.clean);
    CHECK_F02(result.last.pending_save_operations == 2);
    CHECK_F02(result.last.pending_files.size() == 2);
    CHECK_F02(result.waited == milliseconds(2));
  }
}

void TrackerCountsNestedWritesPerPath() {
  rex::filesystem::WriteActivityTracker tracker;
  const std::filesystem::path path = "saves/slot1.sav";
  tracker.Begin(path);
  tracker.Begin(path);

  auto snapshot = tracker.SnapshotState();
  CHECK_F02(snapshot.open_handles == 2);
  CHECK_F02(snapshot.paths.size() == 1);
  CHECK_F02(!snapshot.idle());

  CHECK_F02(tracker.End(path));
  CHECK_F02(tracker.SnapshotState().open_handles == 1);

  CHECK_F02(tracker.End(path));
  CHECK_F02(tracker.SnapshotState().idle());
  CHECK_F02(tracker.SnapshotState().paths.empty());

  // A stray close must not corrupt the count.
  CHECK_F02(!tracker.End(path));
  CHECK_F02(tracker.SnapshotState().idle());
}

void TrackerListsEachPendingPathOnce() {
  rex::filesystem::WriteActivityTracker tracker;
  tracker.Begin("saves/a.sav");
  tracker.Begin("saves/b.sav");
  tracker.Begin("saves/a.sav");

  const auto snapshot = tracker.SnapshotState();
  CHECK_F02(snapshot.open_handles == 3);
  CHECK_F02(snapshot.paths ==
            std::vector<std::string>{"saves/a.sav", "saves/b.sav"});

  tracker.End("saves/a.sav");
  tracker.End("saves/b.sav");
  tracker.End("saves/a.sav");
  CHECK_F02(tracker.SnapshotState().idle());
}

void TrackerIsSafeAcrossWritingThreads() {
  rex::filesystem::WriteActivityTracker tracker;
  constexpr int kThreads = 4;
  constexpr int kWritesPerThread = 1000;
  std::atomic<int> failures{0};

  std::vector<std::thread> writers;
  writers.reserve(kThreads);
  for (int thread = 0; thread < kThreads; ++thread) {
    writers.emplace_back([&tracker, &failures, thread] {
      const std::filesystem::path path = "saves/slot" + std::to_string(thread % 2) + ".sav";
      for (int i = 0; i < kWritesPerThread; ++i) {
        tracker.Begin(path);
        // Exercise concurrent snapshots; the count is checked after joining.
        (void)tracker.SnapshotState();
        if (!tracker.End(path)) ++failures;
      }
    });
  }
  for (auto& writer : writers) writer.join();

  CHECK_F02(failures.load() == 0);
  CHECK_F02(tracker.SnapshotState().idle());
}

}  // namespace

#ifdef GTA4_F02_STANDALONE
int main() {
  RUN_F02(IdleStateIsClean);
  RUN_F02(PendingSaveOperationBlocksCompletionUntilTheDeadline);
  RUN_F02(OpenWriteHandlesBlockCompletionUntilTheDeadline);
  RUN_F02(SaveThatFinishesDuringTheWaitIsClean);
  RUN_F02(ZeroTimeoutNeverClaimsCompletion);
  RUN_F02(ZeroTimeoutWithNoPendingSaveIsClean);
  RUN_F02(RepeatedWaitsRecoverAfterTheSaveFinishes);
  RUN_F02(RepeatedWaitsKeepReportingPendingSaves);
  RUN_F02(TrackerCountsNestedWritesPerPath);
  RUN_F02(TrackerListsEachPendingPathOnce);
  RUN_F02(TrackerIsSafeAcrossWritingThreads);

  std::printf("%d checks, %d failed scenario(s)\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
#else
TEST_CASE("F02 save barrier idle state is clean") { RUN_F02(IdleStateIsClean); }
TEST_CASE("F02 save barrier waits for a pending save operation") {
  RUN_F02(PendingSaveOperationBlocksCompletionUntilTheDeadline);
}
TEST_CASE("F02 save barrier waits for open write handles") {
  RUN_F02(OpenWriteHandlesBlockCompletionUntilTheDeadline);
}
TEST_CASE("F02 save barrier sees a save finish during the wait") {
  RUN_F02(SaveThatFinishesDuringTheWaitIsClean);
}
TEST_CASE("F02 save barrier zero timeout never claims completion") {
  RUN_F02(ZeroTimeoutNeverClaimsCompletion);
}
TEST_CASE("F02 save barrier zero timeout with no pending save is clean") {
  RUN_F02(ZeroTimeoutWithNoPendingSaveIsClean);
}
TEST_CASE("F02 repeated save waits recover after the save finishes") {
  RUN_F02(RepeatedWaitsRecoverAfterTheSaveFinishes);
}
TEST_CASE("F02 repeated save waits keep reporting pending saves") {
  RUN_F02(RepeatedWaitsKeepReportingPendingSaves);
}
TEST_CASE("F02 write tracker counts nested writes per path") {
  RUN_F02(TrackerCountsNestedWritesPerPath);
}
TEST_CASE("F02 write tracker lists each pending path once") {
  RUN_F02(TrackerListsEachPendingPathOnce);
}
TEST_CASE("F02 write tracker is safe across writing threads") {
  RUN_F02(TrackerIsSafeAcrossWritingThreads);
}
#endif
