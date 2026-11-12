// Host tests for the cooperative guest-thread drain policy.
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include <rex/system/cooperative_drain.h>

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

// Deterministic clock: only the drain's injected sleep advances it.
struct FakeClock {
  milliseconds now{0};

  milliseconds Now() const { return now; }

  void Sleep(milliseconds delay) { now += delay; }
};

std::vector<uint32_t> Ids(std::initializer_list<uint32_t> ids) { return std::vector<uint32_t>(ids); }

// is_running reads the clock, so a thread can be made to leave at a chosen time.
auto RunningUntil(FakeClock& clock, std::vector<std::pair<uint32_t, milliseconds>> exit_times) {
  return [&clock, exit_times](uint32_t id) {
    for (const auto& [thread_id, exit_at] : exit_times) {
      if (thread_id == id) return clock.now < exit_at;
    }
    return false;
  };
}

void EmptySetIsClean() {
  FakeClock clock;
  const DrainResult result =
      DrainCooperatively({}, [](uint32_t) { return true; }, milliseconds(10), [&] { return clock.Now(); },
                         [&](milliseconds delay) { clock.Sleep(delay); });
  CHECK_F02(result.clean());
  CHECK_F02(result.requested_threads == 0);
  CHECK_F02(result.surviving_threads.empty());
  CHECK_F02(result.waited == milliseconds(0));
}

void AlreadyExitedSetIsClean() {
  FakeClock clock;
  const DrainResult result =
      DrainCooperatively(Ids({1, 2, 3}), [](uint32_t) { return false; }, milliseconds(10),
                         [&] { return clock.Now(); }, [&](milliseconds delay) { clock.Sleep(delay); });
  CHECK_F02(result.clean());
  CHECK_F02(result.requested_threads == 3);
  CHECK_F02(result.surviving_threads.empty());
  CHECK_F02(result.waited == milliseconds(0));
}

void ThreadsThatLeaveDuringTheDrainAreClean() {
  FakeClock clock;
  const auto is_running = RunningUntil(clock, {{1, milliseconds(2)}, {2, milliseconds(0)}});
  const DrainResult result =
      DrainCooperatively(Ids({1, 2}), is_running, milliseconds(10), [&] { return clock.Now(); },
                         [&](milliseconds delay) { clock.Sleep(delay); });
  CHECK_F02(result.clean());
  CHECK_F02(result.requested_threads == 2);
  CHECK_F02(result.surviving_threads.empty());
  CHECK_F02(result.waited == milliseconds(2));
}

void SurvivorsAreReportedAtTheDeadline() {
  FakeClock clock;
  const auto is_running = RunningUntil(clock, {{1, milliseconds(2)}, {2, milliseconds(100)}});
  const DrainResult result =
      DrainCooperatively(Ids({1, 2}), is_running, milliseconds(5), [&] { return clock.Now(); },
                         [&](milliseconds delay) { clock.Sleep(delay); });
  CHECK_F02(!result.clean());
  CHECK_F02(result.status == DrainStatus::kStragglers);
  CHECK_F02(result.requested_threads == 2);
  CHECK_F02(result.surviving_threads == Ids({2}));
  CHECK_F02(result.waited == milliseconds(5));
}

void ZeroTimeoutChecksOnceAndDoesNotClaimSuccess() {
  FakeClock clock;
  const DrainResult result =
      DrainCooperatively(Ids({7}), [](uint32_t) { return true; }, milliseconds(0),
                         [&] { return clock.Now(); }, [&](milliseconds delay) { clock.Sleep(delay); });
  CHECK_F02(!result.clean());
  CHECK_F02(result.surviving_threads == Ids({7}));
  CHECK_F02(result.waited == milliseconds(0));
}

void SurvivorOrderFollowsTheRequest() {
  FakeClock clock;
  const auto is_running = [](uint32_t id) { return id == 9 || id == 4; };
  const DrainResult result =
      DrainCooperatively(Ids({9, 3, 4}), is_running, milliseconds(3), [&] { return clock.Now(); },
                         [&](milliseconds delay) { clock.Sleep(delay); });
  CHECK_F02(result.surviving_threads == Ids({9, 4}));
}

void RepeatedDrainsCanRecover() {
  FakeClock clock;
  bool straggler_running = true;
  const auto is_running = [&straggler_running](uint32_t) { return straggler_running; };

  const DrainResult first =
      DrainCooperatively(Ids({5}), is_running, milliseconds(3), [&] { return clock.Now(); },
                         [&](milliseconds delay) { clock.Sleep(delay); });
  CHECK_F02(!first.clean());
  CHECK_F02(first.surviving_threads == Ids({5}));

  // The straggler leaves later; a repeated shutdown observes the recovery.
  straggler_running = false;
  const DrainResult second =
      DrainCooperatively(Ids({5}), is_running, milliseconds(3), [&] { return clock.Now(); },
                         [&](milliseconds delay) { clock.Sleep(delay); });
  CHECK_F02(second.clean());
  CHECK_F02(second.requested_threads == 1);
  CHECK_F02(second.surviving_threads.empty());
  CHECK_F02(second.waited == milliseconds(0));
}

void RepeatedDrainsKeepReportingStragglers() {
  FakeClock clock;
  const auto is_running = [](uint32_t) { return true; };
  for (int attempt = 0; attempt < 3; ++attempt) {
    const DrainResult result =
        DrainCooperatively(Ids({8, 9}), is_running, milliseconds(2), [&] { return clock.Now(); },
                           [&](milliseconds delay) { clock.Sleep(delay); });
    CHECK_F02(!result.clean());
    CHECK_F02(result.surviving_threads == Ids({8, 9}));
    CHECK_F02(result.waited == milliseconds(2));
  }
}

}  // namespace

#ifdef GTA4_F02_STANDALONE
int main() {
  RUN_F02(EmptySetIsClean);
  RUN_F02(AlreadyExitedSetIsClean);
  RUN_F02(ThreadsThatLeaveDuringTheDrainAreClean);
  RUN_F02(SurvivorsAreReportedAtTheDeadline);
  RUN_F02(ZeroTimeoutChecksOnceAndDoesNotClaimSuccess);
  RUN_F02(SurvivorOrderFollowsTheRequest);
  RUN_F02(RepeatedDrainsCanRecover);
  RUN_F02(RepeatedDrainsKeepReportingStragglers);

  std::printf("%d checks, %d failed scenario(s)\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
#else
TEST_CASE("F02 drain empty set is clean") { RUN_F02(EmptySetIsClean); }
TEST_CASE("F02 drain already-exited set is clean") { RUN_F02(AlreadyExitedSetIsClean); }
TEST_CASE("F02 drain threads that leave during the drain are clean") {
  RUN_F02(ThreadsThatLeaveDuringTheDrainAreClean);
}
TEST_CASE("F02 drain survivors are reported at the deadline") {
  RUN_F02(SurvivorsAreReportedAtTheDeadline);
}
TEST_CASE("F02 drain with zero timeout never claims success") {
  RUN_F02(ZeroTimeoutChecksOnceAndDoesNotClaimSuccess);
}
TEST_CASE("F02 drain survivor order follows the request") {
  RUN_F02(SurvivorOrderFollowsTheRequest);
}
TEST_CASE("F02 repeated drains recover after a straggler exits") {
  RUN_F02(RepeatedDrainsCanRecover);
}
TEST_CASE("F02 repeated drains keep reporting stragglers") {
  RUN_F02(RepeatedDrainsKeepReportingStragglers);
}
#endif
