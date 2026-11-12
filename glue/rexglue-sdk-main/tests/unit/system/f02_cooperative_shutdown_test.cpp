// Host tests for the cooperative shutdown ordering and retention policy.
#include <chrono>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <rex/system/cooperative_shutdown.h>

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

struct Recorder {
  std::vector<std::string> steps;

  void Step(const char* name) { steps.emplace_back(name); }

  uint32_t Count(const char* name) const {
    uint32_t count = 0;
    for (const auto& step : steps) {
      if (step == name) ++count;
    }
    return count;
  }
};

DrainResult CleanDrain(uint32_t requested = 2) {
  DrainResult result;
  result.status = DrainStatus::kClean;
  result.requested_threads = requested;
  result.waited = milliseconds(7);
  return result;
}

DrainResult StragglerDrain(std::vector<uint32_t> survivors) {
  DrainResult result;
  result.status = DrainStatus::kStragglers;
  result.requested_threads = 3;
  result.surviving_threads = std::move(survivors);
  result.waited = milliseconds(200);
  return result;
}

SaveWaitResult CleanSaves() {
  SaveWaitResult result;
  result.clean = true;
  result.last.save_admission_closed = true;
  result.waited = milliseconds(1);
  return result;
}

SaveWaitResult DirtySaves() {
  SaveWaitResult result;
  result.clean = false;
  result.last.save_admission_closed = true;
  result.last.open_write_handles = 1;
  result.last.pending_files = {"/saves/slot1.bin"};
  result.waited = milliseconds(5000);
  return result;
}

void DefaultReportIsBlocked() {
  const ShutdownReport report;
  CHECK_F02(report.blocked());
  CHECK_F02(!report.complete());
  CHECK_F02(report.status == ShutdownStatus::kBlockedOnGuestThreads);
}

void CleanSequenceRunsInFixedOrder() {
  Recorder recorder;
  const ShutdownReport report = RunCooperativeShutdown(
      [&] {
        recorder.Step("drain");
        return CleanDrain();
      },
      [&] { recorder.Step("close"); },
      [&] {
        recorder.Step("wait");
        return CleanSaves();
      },
      [&] {
        recorder.Step("destroy");
        return true;
      });

  CHECK_F02(report.complete());
  CHECK_F02(report.status == ShutdownStatus::kComplete);
  CHECK_F02(report.guest_threads.clean());
  CHECK_F02(report.save_work.clean);
  CHECK_F02(report.guest_threads.requested_threads == 2);
  CHECK_F02((recorder.steps ==
             std::vector<std::string>{"drain", "close", "wait", "destroy"}));
}

void BlockedDrainSkipsEveryLaterStep() {
  Recorder recorder;
  const ShutdownReport report = RunCooperativeShutdown(
      [&] {
        recorder.Step("drain");
        return StragglerDrain({0x10, 0x22});
      },
      [&] { recorder.Step("close"); },
      [&] {
        recorder.Step("wait");
        return CleanSaves();
      },
      [&] {
        recorder.Step("destroy");
        return true;
      });

  CHECK_F02(report.blocked());
  CHECK_F02(report.status == ShutdownStatus::kBlockedOnGuestThreads);
  CHECK_F02(report.guest_threads.surviving_threads.size() == 2);
  CHECK_F02(report.guest_threads.surviving_threads[0] == 0x10);
  CHECK_F02(report.guest_threads.surviving_threads[1] == 0x22);
  CHECK_F02(report.guest_threads.waited == milliseconds(200));
  // A surviving guest may still issue saves, so the save root must not be
  // closed and nothing may be destroyed.
  CHECK_F02((recorder.steps == std::vector<std::string>{"drain"}));
}

void BlockedSaveWorkSkipsDestroy() {
  Recorder recorder;
  const ShutdownReport report = RunCooperativeShutdown(
      [&] {
        recorder.Step("drain");
        return CleanDrain(1);
      },
      [&] { recorder.Step("close"); },
      [&] {
        recorder.Step("wait");
        return DirtySaves();
      },
      [&] {
        recorder.Step("destroy");
        return true;
      });

  CHECK_F02(report.blocked());
  CHECK_F02(report.status == ShutdownStatus::kBlockedOnSaveWork);
  CHECK_F02(report.guest_threads.clean());
  CHECK_F02(!report.save_work.clean);
  CHECK_F02(report.save_work.waited == milliseconds(5000));
  CHECK_F02(report.save_work.last.open_write_handles == 1);
  CHECK_F02(report.save_work.last.pending_files.size() == 1);
  // The drain was clean, so closing idle content is allowed; destruction is
  // not, because a write handle is still open.
  CHECK_F02((recorder.steps == std::vector<std::string>{"drain", "close", "wait"}));
}

void RepeatedAttemptsAdvanceOnlyWhenAPhaseCompletes() {
  Recorder recorder;
  bool guest_exited = false;
  bool saves_flushed = false;

  auto attempt = [&] {
    return RunCooperativeShutdown(
        [&] {
          recorder.Step("drain");
          return guest_exited ? CleanDrain() : StragglerDrain({0x33});
        },
        [&] { recorder.Step("close"); },
        [&] {
          recorder.Step("wait");
          return saves_flushed ? CleanSaves() : DirtySaves();
        },
        [&] {
        recorder.Step("destroy");
        return true;
      });
  };

  const ShutdownReport first = attempt();
  CHECK_F02(first.status == ShutdownStatus::kBlockedOnGuestThreads);
  CHECK_F02(first.guest_threads.surviving_threads.size() == 1);

  guest_exited = true;
  const ShutdownReport second = attempt();
  CHECK_F02(second.status == ShutdownStatus::kBlockedOnSaveWork);
  CHECK_F02(second.guest_threads.clean());
  CHECK_F02(!second.save_work.clean);

  saves_flushed = true;
  const ShutdownReport third = attempt();
  CHECK_F02(third.complete());

  // Destruction happens exactly once, and only after both phases proved clean.
  CHECK_F02(recorder.Count("destroy") == 1);
  CHECK_F02((recorder.steps == std::vector<std::string>{"drain", "drain", "close", "wait",
                                                       "drain", "close", "wait", "destroy"}));
}

void ABlockedRetryDoesNotDestroyPartialState() {
  Recorder recorder;
  uint32_t attempts = 0;

  auto attempt = [&] {
    ++attempts;
    return RunCooperativeShutdown(
        [&] {
          recorder.Step("drain");
          // Still blocked on every attempt; the caller must keep every
          // resource alive rather than destroying what is already idle.
          return StragglerDrain({0x01});
        },
        [&] { recorder.Step("close"); },
        [&] {
          recorder.Step("wait");
          return CleanSaves();
        },
        [&] {
        recorder.Step("destroy");
        return true;
      });
  };

  for (int i = 0; i < 3; ++i) {
    const ShutdownReport report = attempt();
    CHECK_F02(report.status == ShutdownStatus::kBlockedOnGuestThreads);
    CHECK_F02(report.guest_threads.surviving_threads[0] == 0x01);
  }

  CHECK_F02(attempts == 3);
  CHECK_F02(recorder.Count("drain") == 3);
  CHECK_F02(recorder.Count("close") == 0);
  CHECK_F02(recorder.Count("wait") == 0);
  CHECK_F02(recorder.Count("destroy") == 0);
}

void HostExitPredicateWaitsPastRunningFalse() {
  // Models a thread that published running=false inside Exit() but whose host
  // stack has not yet released its runtime references. A drain keyed on
  // is_running() would report clean too early; the host-exit predicate must
  // keep treating it as a survivor.
  bool running = false;
  bool host_exited = false;
  milliseconds now{0};

  DrainResult weak = DrainCooperatively(
      {0x42}, [&](uint32_t) { return running; }, milliseconds(5), [&] { return now; },
      [&](milliseconds delay) { now += delay; });
  CHECK_F02(weak.clean());

  now = milliseconds(0);
  DrainResult strong = DrainCooperatively(
      {0x42}, [&](uint32_t) { return !host_exited; }, milliseconds(5), [&] { return now; },
      [&](milliseconds delay) { now += delay; });
  CHECK_F02(!strong.clean());
  CHECK_F02(strong.surviving_threads.size() == 1);
  CHECK_F02(strong.waited == milliseconds(5));

  host_exited = true;
  now = milliseconds(0);
  DrainResult after_exit = DrainCooperatively(
      {0x42}, [&](uint32_t) { return !host_exited; }, milliseconds(5), [&] { return now; },
      [&](milliseconds delay) { now += delay; });
  CHECK_F02(after_exit.clean());
  CHECK_F02(after_exit.waited == milliseconds(0));
}

void IncompleteReleaseIsReportedAsIncomplete() {
  // Retained memory ownership must prevent a replacement session.
  Recorder recorder;
  const ShutdownReport report = RunCooperativeShutdown(
      [&] {
        recorder.Step("drain");
        return CleanDrain();
      },
      [&] { recorder.Step("close"); },
      [&] {
        recorder.Step("wait");
        return CleanSaves();
      },
      [&] {
        recorder.Step("destroy");
        return false;
      });

  CHECK_F02(!report.complete());
  CHECK_F02(report.status == ShutdownStatus::kReleasedIncomplete);
  CHECK_F02(report.guest_threads.clean());
  CHECK_F02(report.save_work.clean);
  CHECK_F02((recorder.steps == std::vector<std::string>{"drain", "close", "wait", "destroy"}));
}

}  // namespace

#ifdef GTA4_F02_STANDALONE
int main() {
  RUN_F02(DefaultReportIsBlocked);
  RUN_F02(CleanSequenceRunsInFixedOrder);
  RUN_F02(BlockedDrainSkipsEveryLaterStep);
  RUN_F02(BlockedSaveWorkSkipsDestroy);
  RUN_F02(RepeatedAttemptsAdvanceOnlyWhenAPhaseCompletes);
  RUN_F02(ABlockedRetryDoesNotDestroyPartialState);
  RUN_F02(HostExitPredicateWaitsPastRunningFalse);
  RUN_F02(IncompleteReleaseIsReportedAsIncomplete);

  std::printf("%d checks, %d failed scenario(s)\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
#else
TEST_CASE("F02 shutdown report defaults to blocked") { RUN_F02(DefaultReportIsBlocked); }
TEST_CASE("F02 clean shutdown runs every step in order") {
  RUN_F02(CleanSequenceRunsInFixedOrder);
}
TEST_CASE("F02 blocked drain skips save close and destruction") {
  RUN_F02(BlockedDrainSkipsEveryLaterStep);
}
TEST_CASE("F02 blocked save work skips destruction") { RUN_F02(BlockedSaveWorkSkipsDestroy); }
TEST_CASE("F02 repeated shutdown attempts advance per phase") {
  RUN_F02(RepeatedAttemptsAdvanceOnlyWhenAPhaseCompletes);
}
TEST_CASE("F02 blocked retry never destroys partial state") {
  RUN_F02(ABlockedRetryDoesNotDestroyPartialState);
}
TEST_CASE("F02 drain waits for host exit after running turns false") {
  RUN_F02(HostExitPredicateWaitsPastRunningFalse);
}
TEST_CASE("F02 incomplete release is reported as incomplete") {
  RUN_F02(IncompleteReleaseIsReportedAsIncomplete);
}
#endif
