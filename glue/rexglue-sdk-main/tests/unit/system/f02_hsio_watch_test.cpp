// Host tests for the VdHSIOCalibrationLock watcher lifetime.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <rex/kernel/xboxkrnl/vdhsio_watch.h>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#define F02_HAVE_MMAP 1
#else
#define F02_HAVE_MMAP 0
#endif

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

using namespace std::chrono_literals;
using rex::kernel::xboxkrnl::VdHsioLockWatcher;

constexpr std::chrono::milliseconds kPollInterval{1};

void IgnoreChange(uint32_t, uint8_t, uint8_t, uint8_t, uint8_t, const uint8_t*) {}

template <typename Predicate>
bool WaitFor(Predicate predicate, std::chrono::milliseconds timeout = 2000ms) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(1ms);
  }
  return predicate();
}

struct GuestHeader {
  std::vector<uint8_t> bytes = std::vector<uint8_t>(64, 0);

  explicit GuestHeader(uint8_t type = 1, uint8_t spin = 40) {
    bytes[0] = type;
    bytes[1] = spin;
  }

  volatile uint8_t* header() { return bytes.data(); }
};

#if F02_HAVE_MMAP
void* MapGuestPage() {
  const size_t page_size = 4096;
  void* base = mmap(nullptr, page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK_F02(base != MAP_FAILED);
  std::memset(base, 0, page_size);
  auto* bytes = static_cast<uint8_t*>(base);
  bytes[0] = 1;
  bytes[1] = 40;
  return base;
}

void UnmapGuestPage(void* base) {
  CHECK_F02(munmap(base, 4096) == 0);
}
#endif

void StartsAndSamples() {
  GuestHeader header;
  VdHsioLockWatcher watcher;
  CHECK_F02(watcher.Start(0x1000, header.header(), IgnoreChange, kPollInterval));
  CHECK_F02(watcher.running());
  CHECK_F02(WaitFor([&] { return watcher.guest_accesses() > 0; }));
  watcher.Stop();
  CHECK_F02(!watcher.running());

  const uint64_t frozen = watcher.guest_accesses();
  std::this_thread::sleep_for(20ms);
  CHECK_F02(watcher.guest_accesses() == frozen);
}

void StopWaitsForInFlightSample() {
  GuestHeader header(0, 0);
  std::mutex mutex;
  std::condition_variable condition;
  bool entered = false;
  bool release = false;

  VdHsioLockWatcher watcher;
  CHECK_F02(watcher.Start(
      0x1100, header.header(),
      [&](uint32_t, uint8_t, uint8_t, uint8_t, uint8_t, const uint8_t*) {
        std::unique_lock<std::mutex> lock(mutex);
        entered = true;
        condition.notify_all();
        condition.wait(lock, [&] { return release; });
      },
      kPollInterval));
  CHECK_F02(WaitFor([&] {
    std::lock_guard<std::mutex> lock(mutex);
    return entered;
  }));

  std::atomic<bool> stop_returned{false};
  std::thread stopper([&] {
    watcher.Stop();
    stop_returned.store(true, std::memory_order_release);
  });
  std::this_thread::sleep_for(30ms);
  CHECK_F02(!stop_returned.load(std::memory_order_acquire));
  {
    std::lock_guard<std::mutex> lock(mutex);
    release = true;
  }
  condition.notify_all();
  stopper.join();
  CHECK_F02(stop_returned.load(std::memory_order_acquire));
  CHECK_F02(!watcher.running());
}

void StopPrecedesMappingRelease() {
#if F02_HAVE_MMAP
  void* page = MapGuestPage();
  VdHsioLockWatcher watcher;
  CHECK_F02(watcher.Start(0x1200, static_cast<const volatile uint8_t*>(page), IgnoreChange,
                          kPollInterval));
  CHECK_F02(WaitFor([&] { return watcher.guest_accesses() > 0; }));
  watcher.Stop();
  const uint64_t frozen = watcher.guest_accesses();
  CHECK_F02(!watcher.running());

  // This is the teardown order the session uses: join first, release after.
  UnmapGuestPage(page);
  std::this_thread::sleep_for(30ms);
  CHECK_F02(watcher.guest_accesses() == frozen);
#else
  GuestHeader header;
  VdHsioLockWatcher watcher;
  CHECK_F02(watcher.Start(0x1200, header.header(), IgnoreChange, kPollInterval));
  CHECK_F02(WaitFor([&] { return watcher.guest_accesses() > 0; }));
  watcher.Stop();
  const uint64_t frozen = watcher.guest_accesses();
  std::this_thread::sleep_for(30ms);
  CHECK_F02(watcher.guest_accesses() == frozen);
#endif
}

void RepeatedSessionsDoNotAccumulate() {
  constexpr int kSessions = 32;
#if F02_HAVE_MMAP
  std::vector<void*> pages;
  pages.reserve(kSessions);
  for (int session = 0; session < kSessions; ++session) {
    void* page = MapGuestPage();
    pages.push_back(page);
    VdHsioLockWatcher watcher;
    CHECK_F02(watcher.Start(0x1300 + session, static_cast<const volatile uint8_t*>(page),
                            IgnoreChange, kPollInterval));
    CHECK_F02(WaitFor([&] { return watcher.guest_accesses() > 0; }));
    watcher.Stop();
    CHECK_F02(!watcher.running());
  }
  // A watcher left over from any session would still read its old page and
  // fault once every page is made inaccessible.
  for (void* page : pages) {
    CHECK_F02(mprotect(page, 4096, PROT_NONE) == 0);
  }
  std::this_thread::sleep_for(40ms);
  for (void* page : pages) {
    UnmapGuestPage(page);
  }
#else
  std::vector<std::unique_ptr<GuestHeader>> headers;
  for (int session = 0; session < kSessions; ++session) {
    headers.push_back(std::make_unique<GuestHeader>());
    VdHsioLockWatcher watcher;
    CHECK_F02(watcher.Start(0x1300 + session, headers.back()->header(), IgnoreChange,
                            kPollInterval));
    CHECK_F02(WaitFor([&] { return watcher.guest_accesses() > 0; }));
    watcher.Stop();
    CHECK_F02(!watcher.running());
  }
#endif
}

void RestartStopsPreviousWorker() {
#if F02_HAVE_MMAP
  void* first_page = MapGuestPage();
  GuestHeader second_header;
  VdHsioLockWatcher watcher;
  CHECK_F02(watcher.Start(0x1400, static_cast<const volatile uint8_t*>(first_page), IgnoreChange,
                          kPollInterval));
  CHECK_F02(WaitFor([&] { return watcher.guest_accesses() > 0; }));
  const uint64_t first_accesses = watcher.guest_accesses();

  CHECK_F02(watcher.Start(0x1500, second_header.header(), IgnoreChange, kPollInterval));
  CHECK_F02(watcher.running());
  CHECK_F02(WaitFor([&] { return watcher.guest_accesses() > first_accesses + 1; }));

  // The replaced worker must be gone before the first mapping is released.
  CHECK_F02(mprotect(first_page, 4096, PROT_NONE) == 0);
  std::this_thread::sleep_for(30ms);
  watcher.Stop();
  CHECK_F02(!watcher.running());
  UnmapGuestPage(first_page);
#else
  GuestHeader first_header;
  GuestHeader second_header;
  VdHsioLockWatcher watcher;
  CHECK_F02(watcher.Start(0x1400, first_header.header(), IgnoreChange, kPollInterval));
  CHECK_F02(WaitFor([&] { return watcher.guest_accesses() > 0; }));
  const uint64_t first_accesses = watcher.guest_accesses();
  CHECK_F02(watcher.Start(0x1500, second_header.header(), IgnoreChange, kPollInterval));
  CHECK_F02(WaitFor([&] { return watcher.guest_accesses() > first_accesses + 1; }));
  watcher.Stop();
  CHECK_F02(!watcher.running());
#endif
}

void InvalidStartLeavesNoReader() {
  GuestHeader header;
  VdHsioLockWatcher watcher;
  CHECK_F02(!watcher.Start(0x1600, nullptr, IgnoreChange, kPollInterval));
  CHECK_F02(!watcher.running());
  CHECK_F02(!watcher.Start(0x1600, header.header(), VdHsioLockWatcher::ChangeHandler{},
                           kPollInterval));
  CHECK_F02(!watcher.running());
  CHECK_F02(watcher.guest_accesses() == 0);
  watcher.Stop();
  CHECK_F02(!watcher.running());
}

void DestructorStopsWithoutExplicitStop() {
#if F02_HAVE_MMAP
  void* page = MapGuestPage();
  {
    VdHsioLockWatcher watcher;
    CHECK_F02(watcher.Start(0x1700, static_cast<const volatile uint8_t*>(page), IgnoreChange,
                            kPollInterval));
    CHECK_F02(WaitFor([&] { return watcher.guest_accesses() > 0; }));
  }
  UnmapGuestPage(page);
  std::this_thread::sleep_for(30ms);
#else
  GuestHeader header;
  {
    VdHsioLockWatcher watcher;
    CHECK_F02(watcher.Start(0x1700, header.header(), IgnoreChange, kPollInterval));
    CHECK_F02(WaitFor([&] { return watcher.guest_accesses() > 0; }));
  }
  std::this_thread::sleep_for(30ms);
#endif
}

void SelfExitIsJoinableWithoutHang() {
  GuestHeader header(0, 0);
  std::atomic<int> changes{0};
  VdHsioLockWatcher watcher;
  CHECK_F02(watcher.Start(
      0x1800, header.header(),
      [&](uint32_t, uint8_t, uint8_t, uint8_t, uint8_t, const uint8_t*) {
        changes.fetch_add(1, std::memory_order_acq_rel);
        header.bytes[0] ^= 0x02;
      },
      kPollInterval));
  CHECK_F02(WaitFor([&] { return !watcher.running(); }));
  CHECK_F02(changes.load() >= 8);
  watcher.Stop();
  CHECK_F02(!watcher.running());
}

void ConcurrentStopDoesNotDeadlock() {
  GuestHeader header;
  VdHsioLockWatcher watcher;
  CHECK_F02(watcher.Start(0x1900, header.header(), IgnoreChange, kPollInterval));
  CHECK_F02(WaitFor([&] { return watcher.guest_accesses() > 0; }));

  const auto started = std::chrono::steady_clock::now();
  std::vector<std::thread> stoppers;
  for (int index = 0; index < 4; ++index) {
    stoppers.emplace_back([&] { watcher.Stop(); });
  }
  for (auto& stopper : stoppers) {
    stopper.join();
  }
  const auto elapsed = std::chrono::steady_clock::now() - started;
  CHECK_F02(elapsed < 2000ms);
  CHECK_F02(!watcher.running());
}

}  // namespace

#ifdef GTA4_F02_STANDALONE
int main() {
  RUN_F02(StartsAndSamples);
  RUN_F02(StopWaitsForInFlightSample);
  RUN_F02(StopPrecedesMappingRelease);
  RUN_F02(RepeatedSessionsDoNotAccumulate);
  RUN_F02(RestartStopsPreviousWorker);
  RUN_F02(InvalidStartLeavesNoReader);
  RUN_F02(DestructorStopsWithoutExplicitStop);
  RUN_F02(SelfExitIsJoinableWithoutHang);
  RUN_F02(ConcurrentStopDoesNotDeadlock);
  std::printf("f02_hsio_watch: %d checks, %d failed\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
#else
TEST_CASE("F02 HSIO watcher samples while running") { RUN_F02(StartsAndSamples); }
TEST_CASE("F02 HSIO watcher stop waits for an in-flight sample") {
  RUN_F02(StopWaitsForInFlightSample);
}
TEST_CASE("F02 HSIO watcher stops before the guest mapping is released") {
  RUN_F02(StopPrecedesMappingRelease);
}
TEST_CASE("F02 HSIO watcher sessions do not accumulate") {
  RUN_F02(RepeatedSessionsDoNotAccumulate);
}
TEST_CASE("F02 HSIO watcher restart stops the previous worker") {
  RUN_F02(RestartStopsPreviousWorker);
}
TEST_CASE("F02 HSIO watcher invalid start leaves no reader") {
  RUN_F02(InvalidStartLeavesNoReader);
}
TEST_CASE("F02 HSIO watcher destructor stops the worker") {
  RUN_F02(DestructorStopsWithoutExplicitStop);
}
TEST_CASE("F02 HSIO watcher self-exit remains joinable") {
  RUN_F02(SelfExitIsJoinableWithoutHang);
}
TEST_CASE("F02 HSIO watcher concurrent stop does not deadlock") {
  RUN_F02(ConcurrentStopDoesNotDeadlock);
}
#endif
