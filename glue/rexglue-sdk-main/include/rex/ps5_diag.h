#pragma once
/**
 * [ps5 port] Progress counters for bringing the game up on the PS5: a
 * monitor thread logs them every 10 s (windowed_app_main_sdl.cpp), so a
 * black screen shows which stage stopped moving. Cheap relaxed atomics.
 */
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <sched.h>

namespace rex::ps5diag {
enum Counter : int {
  kAccessViolations,    // faults reaching the MMIO handler
  kMmioAccesses,        // of those, GPU register reads / writes emulated
  kWatchFaults,         // of those, write watches / other handlers
  kUnhandledFaults,     // of those, not handled (a real crash)
  kVblanks,             // GPU vblanks
  kInterrupts,          // guest GPU interrupt callbacks run
  kWritePointer,        // ring buffer write pointer updates by the game
  kPrimaryBuffers,      // ring buffer batches executed
  kCount
};
extern std::atomic<uint64_t> counters[kCount];
// The reserved guest address space (all views live inside it).
extern uintptr_t guest_arena_lo, guest_arena_hi;
inline void Add(Counter c) { counters[c].fetch_add(1, std::memory_order_relaxed); }

// Guest threads blocked in a kernel wait: one slot per host thread, filled on
// entering XObject::Wait / WaitMultiple / SignalAndWait and cleared on leaving.
struct WaitSlot {
  std::atomic<uint64_t> since_ms{0};  // 0: not waiting
  uint32_t thread_id = 0;             // guest thread id
  uint32_t object_type = 0;           // XObject::Type (first object)
  uint32_t handle = 0;                // its handle
  uint32_t count = 0;                 // objects waited on
  uint32_t guest_lr = 0;              // the game code that waits
  uint32_t guest_sp = 0;              // its stack pointer, and its stack [lo, hi): the
  uint32_t guest_stack_lo = 0, guest_stack_hi = 0;  // monitor walks the callers from them
  uint32_t timeout_ms = 0;            // 0xFFFFFFFF: infinite
  const void* critical_section = nullptr;
  uint32_t critical_section_guest = 0;
  uint32_t critical_section_lr = 0;  // the game code that entered it  // host address of a contended X_RTL_CRITICAL_SECTION
};
constexpr int kWaitSlots = 256;
extern WaitSlot wait_slots[kWaitSlots];
WaitSlot* CurrentWaitSlot();  // (nullptr when out of slots)
uint64_t NowMs();
const char* ObjectTypeName(uint32_t type);

// Never scan beyond the interrupted stack pointer's mapped page.
inline uintptr_t StackScanEnd(uintptr_t rsp, size_t bytes) {
  constexpr uintptr_t page_size = 4096;
  const uintptr_t remaining = page_size - (rsp & (page_size - 1));
  return rsp + (bytes < remaining ? bytes : remaining);
}

struct StackSampleGate {
  std::atomic<bool> suspended{false};
  std::atomic<int> readers{0};
  bool Enter() {
    readers.fetch_add(1, std::memory_order_seq_cst);
    if (suspended.load(std::memory_order_seq_cst)) {
      readers.fetch_sub(1, std::memory_order_seq_cst);
      return false;
    }
    return true;
  }
  void Leave() { readers.fetch_sub(1, std::memory_order_seq_cst); }
  void WaitForReaders() {
    while (readers.load(std::memory_order_seq_cst)) sched_yield();
  }
};
extern StackSampleGate stack_sample_gate;

class RuntimeInspection {
 public:
  RuntimeInspection() : entered_(stack_sample_gate.Enter()) {}
  ~RuntimeInspection() { if (entered_) stack_sample_gate.Leave(); }
  RuntimeInspection(const RuntimeInspection&) = delete;
  RuntimeInspection& operator=(const RuntimeInspection&) = delete;
  explicit operator bool() const { return entered_; }
 private:
  bool entered_;
};

void SuspendStackSampling();
void ResumeStackSampling();

// Host stack samples: every rex thread registers itself; SampleAllThreads()
// signals each one (SIGPROF), records its rip and the return-address-looking
// words on its stack, and logs them relative to Anchor() so they can be
// symbolized against the linked ELF offline.
void RegisterThread(const char* what);
// Before the calling thread exits: out of the samples and the CPU moves.
void UnregisterThread();
// [ps5 port] Keeps every registered thread except the renderer's (the
// "worker" and "recorder", and `title_thread`) on `mask`, and the game's main
// thread on `main_mask` when one is given.
void KeepOtherThreadsOn(uint64_t mask, uintptr_t title_thread, uintptr_t main_thread = 0,
                        uint64_t main_mask = 0);
void SampleAllThreads();
// A sampling CPU profiler on the same signal: ProfileTick() signals every
// registered thread once, each records where it was; ProfileReport() logs,
// per busy thread, the code addresses seen most (relative to Anchor(), like
// the stack samples). Cheap enough to leave running.
void ProfileTick();
void ProfileReport();
// A host fault no handler took (a crash in some thread): the first one is
// written to the end of the log file with the thread's return addresses
// (without the heap); the thread then re-faults, and that calls crash_hook.
void UnhandledHostFault(uintptr_t rip, uintptr_t address, uintptr_t rsp);
// Called (once) when a thread keeps faulting: tells the player and ends the
// game rather than leave it hanging. Set by main. It runs in the fault
// handler: no heap (no logging) before it flushes the log.
extern void (*crash_hook)(const char* what);
// When the first unhandled fault was reported (NowMs() | 1; 0: none): a
// watchdog ends the game if the crash hook doesn't.
extern std::atomic<uint64_t> crashed_at_ms;
// The log file, for the crash report (written without the logger).
void SetLogPath(const char* path);
// The game asked to quit (to the dashboard): the app's answer (restart into
// its own menu). Set: it doesn't return when it succeeds.
extern void (*title_exit_hook)();
uintptr_t Anchor();

// Critical sections the game initialized (guest address -> game code).
void NoteCriticalSectionInit(uint32_t guest, uint32_t lr);
uint32_t CriticalSectionInitBy(uint32_t guest);  // 0: never seen
}  // namespace rex::ps5diag
