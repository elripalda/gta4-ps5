#!/usr/bin/env python3
import argparse
import pathlib
import subprocess
import tempfile

from f02_native_confirmation_test import function


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=pathlib.Path, default=pathlib.Path(__file__).resolve().parents[3])
    parser.add_argument("--cxx", default="clang++")
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    root = args.root.resolve()
    memory = (root / "src/system/xmemory.cpp").read_text()
    kernel = (root / "src/system/kernel_state.cpp").read_text()
    posix = (root / "src/core/threading_posix.cpp").read_text()
    runtime = (root / "src/system/runtime.cpp").read_text()
    mapping = memory[memory.index("static const struct {"):memory.index("namespace {", memory.index("} map_info[]"))]
    source = r'''
#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <cstring>
#include <memory>
#include <mutex>
#include <semaphore.h>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>
#include <rex/system/cooperative_drain.h>
#include <rex/system/cooperative_shutdown.h>
#include <functional>
#include <cstdlib>
#define REX_PLATFORM_PS5 1
#define REXSYS_INFO(...) ((void)0)
#define REXSYS_ERROR(...) ((void)0)
#define assert_true(x) assert(x)
#define assert_always() assert(false)
namespace rex {
void FlushLogging() {}
template<class T, size_t N> constexpr size_t countof(T (&)[N]) { return N; }
namespace ps5diag {
uintptr_t guest_arena_lo = 0, guest_arena_hi = 0;
const char* LogPath() { return ""; }
unsigned sample_pause_depth = 0;
void SuspendStackSampling() { ++sample_pause_depth; }
void ResumeStackSampling() { assert(sample_pause_depth); --sample_pause_depth; }
}
namespace memory { constexpr int kFileMappingHandleInvalid = -1; void CloseFileMappingHandle(int fd, int) { close(fd); } }
[[noreturn]] void FatalError(const char*) { std::abort(); }
namespace diagnostics::gta4_transition { void Shutdown() {} }
namespace perf::Profiler { void Shutdown() {} }
}
struct ps5_shm { int64_t direct_start = 0; size_t bytes = 0; };
struct ps5_shm_unmap_report {
  int unmap_result = 0;
  int reserve_result = 0;
  void* reserve_address = nullptr;
  unsigned reserved = 0;
};
struct ps5_shm_stats {
  uint64_t views = 0, view_bytes = 0, objects = 0, object_bytes = 0, ranges = 0, range_bytes = 0;
};
constexpr int PS5_SHM_READ = 1, PS5_SHM_WRITE = 2, PS5_SHM_FIXED = 1, PS5_SHM_KEEP_RESERVED = 2;
constexpr size_t kGuestMappingLength = 0x120000000ull;
struct View { size_t bytes; int fd; };
std::unordered_map<void*, View> mapped_views;
std::unordered_map<void*, int> unmap_calls;
std::atomic<bool> g_ps5_last_teardown_complete{true};
int objects = 0, ranges = 0, create_calls = 0, fail_create = -1, map_calls = 0, fail_map = -1;
int unmap_total = 0, fail_unmap_call = -1;
bool fail_whole = false, fail_reserve = false, fail_unmap = false;
void ps5_shm_live(ps5_shm_stats* stats) {
  *stats = {};
  stats->objects = objects;
  stats->ranges = ranges;
}
bool fail_reserve_renew = false, fail_release = false, fail_range_release = false;
int ps5_vrange_reserve(size_t bytes, void*, size_t, void** out) {
  if (fail_reserve) return -1;
  *out = mmap(nullptr, bytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  assert(*out != MAP_FAILED); ++ranges; return 0;
}
int ps5_vrange_release(void* base, size_t bytes) {
  if (fail_range_release) return -1;
  assert(mapped_views.empty()); --ranges; return munmap(base, bytes);
}
int ps5_shm_create(size_t bytes, ps5_shm* out) {
  const int call = create_calls++;
  if ((fail_whole && bytes > kGuestMappingLength) || call == fail_create) return -1;
  int fd = memfd_create("f02-test", 0); assert(fd >= 0);
  assert(ftruncate(fd, bytes) == 0); *out = {fd, bytes}; ++objects; return 0;
}
int ps5_shm_map(const ps5_shm* shm, size_t offset, size_t bytes, void* at, int, unsigned, void** out) {
  if (map_calls++ == fail_map) return -1;
  assert(offset + bytes <= shm->bytes);
  *out = mmap(at, bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, shm->direct_start, offset);
  assert(*out == at); mapped_views[at] = {bytes, int(shm->direct_start)}; return 0;
}
int ps5_shm_unmap_reported(void* view, size_t bytes, unsigned flags, ps5_shm_unmap_report* report) {
  if (report) *report = ps5_shm_unmap_report{};
  ++unmap_calls[view];
  if (fail_unmap || unmap_total++ == fail_unmap_call) {
    if (report) report->unmap_result = -1;
    return -1;
  }
  assert(mapped_views.at(view).bytes == bytes);
  assert(munmap(view, bytes) == 0); mapped_views.erase(view);
  if (flags & PS5_SHM_KEEP_RESERVED) {
    if (report) report->reserved = 1;
    if (fail_reserve_renew) {
      if (report) { report->reserve_result = 0x80ff0101; report->reserve_address = view; }
      return 0x80ff0101;
    }
    assert(mmap(view, bytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == view);
    if (report) report->reserve_address = view;
  }
  return 0;
}
int ps5_shm_destroy_reported(ps5_shm* shm) {
  if (!shm->bytes) return 0;
  for (const auto& [address, view] : mapped_views) assert(view.fd != shm->direct_start);
  if (fail_release) return -1;
  assert(close(shm->direct_start) == 0); *shm = {}; --objects;
  return 0;
}
bool LastPs5TeardownReleasedEverything() { return g_ps5_last_teardown_complete.load(); }
struct Memory;
Memory* active_memory_ = nullptr;
struct Memory {
  struct Handler { void reset() {} } mmio_handler_;
  struct Heap { void Dispose() { assert(false); } };
  struct { Heap v00000000, v40000000, v80000000, v90000000, vA0000000, vC0000000, vE0000000, physical; } heaps_;
  std::vector<int*> physical_memory_invalidation_callbacks_;
  uint32_t system_allocation_granularity_ = 16384;
  int mapping_ = -1, file_name_ = 0;
  uint8_t *mapping_base_ = nullptr, *virtual_membase_ = nullptr, *physical_membase_ = nullptr;
  struct { uint8_t* all_views[9]{}; } views_;
  enum class Ps5ViewState : uint8_t { kNone, kLive, kReservedHole, kUncertain };
  ps5_shm ps5_backing_[4]{};
  Ps5ViewState ps5_view_state_[9]{};
  void* ps5_arena_ = nullptr;
  bool ps5_reserved_range_ = false;
  Memory() { assert(!active_memory_); active_memory_ = this; }
  ~Memory();
  int MapViewsPs5();
  bool ReleasePs5Backing();
  bool ReleaseGuestMappings();
  void UnmapViews();
};
'''
    source += mapping
    for signature in ("int Memory::MapViewsPs5()", "bool Memory::ReleasePs5Backing()", "void Memory::UnmapViews()", "bool Memory::ReleaseGuestMappings()", "Memory::~Memory()"):
        source += function(memory, signature) + "\n"
    source += r'''
namespace diagnostics = rex::diagnostics;
struct RuntimeConfig {};
using X_STATUS = int;
constexpr X_STATUS X_STATUS_UNSUCCESSFUL = -1, X_STATUS_SUCCESS = 0;
std::atomic<uint64_t> g_graphics_generation{0};
struct Runtime {
  struct Subsystem { void Shutdown() {} };
  inline static Runtime* instance_ = nullptr;
  bool setup_complete_ = true, last_shutdown_released_everything_ = true;
  std::unique_ptr<Memory> memory_;
  std::unique_ptr<Subsystem> graphics_system_, audio_system_, input_system_, kernel_state_,
      function_dispatcher_, export_resolver_, file_system_;
  void Shutdown();
  X_STATUS Setup(RuntimeConfig);
  rex::system::ShutdownReport ShutdownCooperatively(uint32_t, std::chrono::milliseconds,
                                               std::function<void()>);
};
'''
    source += function(runtime, "void Runtime::Shutdown()") + "\n"
    admission = function(runtime, "X_STATUS Runtime::Setup(RuntimeConfig config)")
    source += admission[:admission.index("  auto fail =")] + "  return X_STATUS_SUCCESS;\n}\n"
    repeated = function(runtime, "system::ShutdownReport Runtime::ShutdownCooperatively(")
    source += repeated[:repeated.index("  // Destroying the runtime")].replace("system::", "rex::system::") + "  assert(false);\n  return report;\n}\n"
    source += r'''
void runtime_failure_case() {
  assert(objects == 0 && ranges == 0 && mapped_views.empty());
  Runtime runtime;
  runtime.memory_ = std::make_unique<Memory>();
  assert(runtime.memory_->MapViewsPs5() == 0);
  Runtime::instance_ = &runtime;
  Memory* const owner = runtime.memory_.get();
  const ps5_shm backing = owner->ps5_backing_[0];
  fail_release = true;
  for (int attempt = 0; attempt < 3; ++attempt) {
    runtime.Shutdown();
    assert(runtime.memory_.get() == owner && active_memory_ == owner);
    assert(Runtime::instance_ == &runtime);
    assert(!runtime.last_shutdown_released_everything_);
    assert(objects == 1 && ranges == 1 && mapped_views.empty());
    assert(owner->ps5_backing_[0].direct_start == backing.direct_start);
    assert(owner->ps5_backing_[0].bytes == backing.bytes);
    assert(fcntl(backing.direct_start, F_GETFD) >= 0);
    Runtime next;
    assert(next.Setup({}) == X_STATUS_UNSUCCESSFUL);
    assert(runtime.Setup({}) == X_STATUS_UNSUCCESSFUL);
    bool callback = false;
    const auto report = runtime.ShutdownCooperatively(0, std::chrono::milliseconds(0),
                                                     [&] { callback = true; });
    assert(report.status == rex::system::ShutdownStatus::kReleasedIncomplete);
    assert(!callback && runtime.memory_.get() == owner);
  }
  for (const auto& entry : unmap_calls) assert(entry.second == 1);
  fail_release = false;
  assert(runtime.ShutdownCooperatively(0, std::chrono::milliseconds(0), {}).complete());
  assert(!runtime.memory_ && !Runtime::instance_ && !active_memory_);
  assert(objects == 0 && ranges == 0 && mapped_views.empty());
  assert(runtime.ShutdownCooperatively(0, std::chrono::milliseconds(0), {}).complete());
  Runtime next;
  assert(next.Setup({}) == X_STATUS_SUCCESS);
  Runtime::instance_ = nullptr;
}
struct JoinHandle {
  pthread_t thread_{};
  std::mutex join_mutex_;
  bool joined_ = false;
  sem_t suspend_sem_{};
  int ps5_stack_ = 0;
  void Ps5StackFree(int) {}
'''
    source += function(posix[posix.index("  // The kernel's id for the thread"):], "inline bool post_execution() override").replace(" override", "")
    source += r'''
};
namespace rex::thread {
enum class WaitResult { kSuccess, kTimeout, kFailed };
WaitResult Wait(JoinHandle* handle, bool, std::chrono::milliseconds) {
  return handle->post_execution() ? WaitResult::kSuccess : WaitResult::kFailed;
}
}
struct XThread {
  std::atomic<bool> token{false}, cleanup_done{false}, release_cleanup{false};
  JoinHandle handle;
  uint32_t thread_id() const { return 1; }
  bool host_exited() const { return token.load(); }
  JoinHandle* thread() { return &handle; }
};
template<class T> using object_ref = std::shared_ptr<T>;
using namespace rex::system;
struct KernelState {
  DrainResult WaitForThreadsToExit(const std::vector<object_ref<XThread>>&, uint32_t);
};
'''
    source += function(kernel, "DrainResult KernelState::WaitForThreadsToExit(")
    source += r'''
void reset_fixture() {
  assert(objects == 0 && ranges == 0 && mapped_views.empty());
  assert(g_ps5_last_teardown_complete.load());
  unmap_calls.clear();
  create_calls = map_calls = unmap_total = 0;
  fail_create = fail_map = fail_unmap_call = -1;
  fail_whole = fail_reserve = fail_unmap = false;
  fail_reserve_renew = fail_release = fail_range_release = false;
  rex::ps5diag::guest_arena_lo = rex::ps5diag::guest_arena_hi = 0;
}
void memory_case(bool three, int allocation_failure, int view_failure, bool anonymous) {
  reset_fixture();
  fail_whole = three;
  fail_create = allocation_failure; fail_map = view_failure; fail_reserve = anonymous;
  {
    Memory memory;
    int rc = memory.MapViewsPs5();
    if (allocation_failure >= 0 || view_failure >= 0) assert(rc != 0);
    else {
      assert(rc == 0); assert(objects == (three ? 3 : 1)); assert(mapped_views.size() == 9);
      assert(memory.views_.all_views[7] && memory.views_.all_views[8]);
      for (auto state : memory.ps5_view_state_) assert(state == Memory::Ps5ViewState::kLive);
    }
  }
  // Every view the run touched is unmapped exactly once, and cleanup completed.
  const size_t expected_views = allocation_failure >= 0 ? 0 : view_failure >= 0 ? view_failure : 9;
  assert(unmap_calls.size() == expected_views);
  for (const auto& entry : unmap_calls) assert(entry.second == 1);
  assert(objects == 0 && ranges == 0 && mapped_views.empty());
  assert(rex::ps5diag::guest_arena_lo == 0 && rex::ps5diag::guest_arena_hi == 0);
  assert(g_ps5_last_teardown_complete.load());
}
void reserve_renewal_failure_case() {
  reset_fixture();
  {
    Memory memory;
    assert(memory.MapViewsPs5() == 0);
    void* const first_view = memory.views_.all_views[0];
    fail_reserve_renew = true;
    // Unmap succeeded, fixed re-reservation failed: ownership is unknown and the
    // backing cannot be released while views 1..8 still alias it.
    assert(!memory.ReleasePs5Backing());
    assert(unmap_calls[first_view] == 1);
    assert(memory.views_.all_views[0] == nullptr);
    assert(memory.ps5_view_state_[0] == Memory::Ps5ViewState::kUncertain);
    assert(mapped_views.size() == 8);
    assert(objects == 1);
    // A retry must not unmap the already-unmapped view a second time.
    assert(!memory.ReleasePs5Backing());
    assert(unmap_calls[first_view] == 1);
    // Fixture cleanup restores the hole; production must not do this blindly.
    assert(mmap(first_view, 0x40000000, PROT_NONE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == first_view);
    memory.ps5_view_state_[0] = Memory::Ps5ViewState::kReservedHole;
    fail_reserve_renew = false;
  }
  assert(objects == 0 && ranges == 0 && mapped_views.empty());
  assert(unmap_calls.size() == 9);
  for (const auto& entry : unmap_calls) assert(entry.second == 1);
  assert(g_ps5_last_teardown_complete.load());
}
void unmap_failure_case() {
  reset_fixture();
  void* first_view = nullptr;
  {
    Memory memory;
    assert(memory.MapViewsPs5() == 0);
    first_view = memory.views_.all_views[0];
    fail_unmap = true;
    // A true unmap failure leaves the alias live: the view and backing stay.
    assert(!memory.ReleasePs5Backing());
    assert(unmap_calls[first_view] == 1);
    assert(memory.views_.all_views[0] == first_view);
    assert(memory.ps5_view_state_[0] == Memory::Ps5ViewState::kLive);
    assert(mapped_views.size() == 9 && objects == 1);
    fail_unmap = false;
  }
  assert(objects == 0 && ranges == 0 && mapped_views.empty());
  assert(unmap_calls.size() == 9);
  assert(unmap_calls[first_view] == 2);  // one failed attempt, one success
  for (const auto& entry : unmap_calls) {
    if (entry.first != first_view) assert(entry.second == 1);
  }
  assert(g_ps5_last_teardown_complete.load());
}
void unmap_failure_at_view_case(int view_index) {
  reset_fixture();
  void* addresses[9] = {};
  fail_unmap_call = view_index;
  {
    Memory memory;
    assert(memory.MapViewsPs5() == 0);
    for (int n = 0; n < 9; ++n) addresses[n] = memory.views_.all_views[n];
    assert(!memory.ReleasePs5Backing());
    for (int n = 0; n < view_index; ++n) {
      assert(memory.views_.all_views[n] == nullptr);
      assert(memory.ps5_view_state_[n] == Memory::Ps5ViewState::kReservedHole);
    }
    for (int n = view_index; n < 9; ++n) {
      assert(memory.views_.all_views[n] == addresses[n]);
      assert(memory.ps5_view_state_[n] == Memory::Ps5ViewState::kLive);
    }
    assert(mapped_views.size() == static_cast<size_t>(9 - view_index));
    assert(objects == 1 && ranges == 1);
    fail_unmap_call = -1;
  }
  // The destructor retries the failed view and the later ones without ever
  // unmapping a view twice.
  assert(objects == 0 && ranges == 0 && mapped_views.empty());
  assert(unmap_calls.size() == 9);
  for (int n = 0; n < 9; ++n) {
    assert(unmap_calls[addresses[n]] == (n == view_index ? 2 : 1));
  }
  assert(g_ps5_last_teardown_complete.load());
}
void release_failure_case() {
  reset_fixture();
  Memory memory;
  assert(memory.MapViewsPs5() == 0);
  fail_release = true;
  const ps5_shm backing = memory.ps5_backing_[0];
  memory.UnmapViews();
  assert(!g_ps5_last_teardown_complete.load());
  assert(objects == 1 && ranges == 1 && mapped_views.empty());
  assert(memory.ps5_backing_[0].bytes == backing.bytes);
  assert(memory.ps5_backing_[0].direct_start == backing.direct_start);
  assert(fcntl(backing.direct_start, F_GETFD) >= 0);
  assert(memory.ps5_arena_ != nullptr);
  fail_release = false;
  memory.UnmapViews();
  assert(g_ps5_last_teardown_complete.load());
  assert(objects == 0 && ranges == 0 && mapped_views.empty());
  assert(memory.ps5_arena_ == nullptr);
}
void arena_failure_case() {
  reset_fixture();
  Memory memory;
  assert(memory.MapViewsPs5() == 0);
  fail_range_release = true;
  memory.UnmapViews();
  assert(!g_ps5_last_teardown_complete.load());
  assert(objects == 0 && ranges == 1 && mapped_views.empty());
  assert(memory.ps5_arena_ != nullptr);
  fail_range_release = false;
  memory.UnmapViews();
  assert(g_ps5_last_teardown_complete.load());
  assert(objects == 0 && ranges == 0 && mapped_views.empty());
  assert(memory.ps5_arena_ == nullptr);
}
void join_case() {
  auto thread = std::make_shared<XThread>();
  sem_init(&thread->handle.suspend_sem_, 0, 0);
  assert(pthread_create(&thread->handle.thread_, nullptr, [](void* ptr) -> void* {
    auto& thread = *static_cast<XThread*>(ptr);
    thread.token.store(true);
    while (!thread.release_cleanup.load()) std::this_thread::yield();
    thread.cleanup_done.store(true); return nullptr;
  }, thread.get()) == 0);
  while (!thread->token.load()) std::this_thread::yield();
  std::atomic<bool> clean{false};
  std::thread drainer([&] {
    KernelState kernel;
    auto result = kernel.WaitForThreadsToExit({thread}, 1000);
    assert(result.clean()); assert(thread->cleanup_done.load()); clean = true;
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  assert(!clean.load());
  thread->release_cleanup = true;
  drainer.join();
  thread->handle.post_execution();
  assert(thread->handle.joined_);
}
void join_failure_case() {
  auto thread = std::make_shared<XThread>();
  thread->token.store(true);
  // Joining the calling thread fails with EDEADLK and must surface as kFailed,
  // never as an exited thread or a clean drain.
  thread->handle.thread_ = pthread_self();
  KernelState kernel;
  auto result = kernel.WaitForThreadsToExit({thread}, 5);
  assert(!result.clean());
  assert(result.surviving_threads.size() == 1 && result.surviving_threads[0] == 1);
  assert(!thread->handle.joined_);
}
int main() {
  for (int cycle = 0; cycle < 3; ++cycle) {
    memory_case(false, -1, -1, false);
    memory_case(true, -1, -1, false);
    memory_case(false, -1, -1, true);
  }
  for (int view = 0; view < 9; ++view) {
    memory_case(false, -1, view, false);
    memory_case(true, -1, view, false);
  }
  for (int allocation = 1; allocation <= 3; ++allocation) memory_case(true, allocation, -1, false);
  reserve_renewal_failure_case();
  unmap_failure_case();
  unmap_failure_at_view_case(7);
  unmap_failure_at_view_case(8);
  release_failure_case();
  arena_failure_case();
  reset_fixture();
  runtime_failure_case();
  join_case();
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define F02_ASAN 1
#endif
#endif
#if !defined(__SANITIZE_ADDRESS__) && !defined(F02_ASAN)
  // ASan aborts on any handle it considers already joined, including a
  // self/invalid handle, so the injected join failure only runs unsanitized.
  join_failure_case();
#endif
  std::puts("actual PS5 mapping/destructor + cooperative drain/join routines: PASS (31 memory cases, view 7/8 boundaries, uncertain reservation, true unmap failure, partial-release reporting, failed join)");
}
'''
    with tempfile.TemporaryDirectory(prefix="f02-native-teardown-") as directory:
        path = pathlib.Path(directory)
        (path / "test.cpp").write_text(source)
        command = [args.cxx, "-std=c++20", "-O1", "-g", "-pthread", "-I", str(root / "include"),
                   str(path / "test.cpp"), "-o", str(path / "test")]
        if args.sanitize:
            command.extend(["-fsanitize=address,undefined", "-fno-omit-frame-pointer"])
        subprocess.run(command, check=True)
        subprocess.run([str(path / "test")], check=True)


if __name__ == "__main__":
    main()
