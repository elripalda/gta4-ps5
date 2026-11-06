// [ps5 port] see rex/ps5_diag.h
#include <rex/ps5_diag.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <fcntl.h>
#include <pthread.h>
#include <pthread_np.h>
#include <sched.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

#include <rex/chrono/clock.h>
#include <rex/frame_stats.h>
#include <rex/logging.h>

namespace rex::ps5diag {
std::atomic<uint64_t> counters[kCount];
WaitSlot wait_slots[kWaitSlots];
uintptr_t guest_arena_lo = 0, guest_arena_hi = 0;
void (*crash_hook)(const char* what) = nullptr;
void (*title_exit_hook)() = nullptr;

namespace {
std::atomic<int> next_slot{0};
}

WaitSlot* CurrentWaitSlot() {
  thread_local int index = -1;
  if (index < 0) index = next_slot.fetch_add(1);
  return index < kWaitSlots ? &wait_slots[index] : nullptr;
}

uint64_t NowMs() {
  // The TSC: every guest kernel wait stamps its start with this, and a
  // steady_clock read is a system call on the PS5.
  const uint64_t per_ms = rex::chrono::Clock::QueryHostTickFrequency() / 1000;
  return per_ms ? rex::chrono::Clock::QueryHostTickCount() / per_ms : 0;
}

const char* ObjectTypeName(uint32_t type) {
  static const char* const kNames[] = {"Undefined", "Enumerator", "Event",     "File",
                                       "IOCompletion", "Module",  "Mutant",    "NotifyListener",
                                       "Semaphore", "Session",    "Socket",    "SymbolicLink",
                                       "Thread",    "Timer"};
  return type < sizeof(kNames) / sizeof(kNames[0]) ? kNames[type] : "?";
}

namespace {
struct ThreadEntry {
  std::atomic<bool> used{false};
  pthread_t pthread{};
  int tid = 0;
  uintptr_t stack_top = 0;
  char what[24] = {};
  // filled by the signal handler
  std::atomic<int> done{0};
  uintptr_t rip = 0, rsp = 0;
  uintptr_t words[64] = {};
  int nwords = 0;
  // the profiler's samples (rip relative to the anchor, 0: outside the game)
  std::atomic<bool> want_stack{false};
  static constexpr int kProfileSamples = 8192;
  uint32_t profile[kProfileSamples] = {};
  // For a sample in a library: the next return address into the game past
  // the one it is charged to (who called the wrapper: std::mutex::lock's
  // caller, not std::mutex::lock).
  uint32_t profile_parent[kProfileSamples] = {};
  std::atomic<uint32_t> profile_count{0};
  uint32_t profile_reported = 0;
  // [ps5 port] A library sample's own address (where in libkernel, libc or
  // the GPU driver the thread was), for "library time by place".
  static constexpr int kLibrarySamples = 2048;
  uint64_t library_rip[kLibrarySamples] = {};
  std::atomic<uint32_t> library_count{0};
  uint32_t library_reported = 0;
  // The CPUs KeepOtherThreadsOn last gave it (0: none yet).
  uint64_t applied_mask = 0;
};
constexpr int kThreads = 160;  // (the game runs about 40)
ThreadEntry threads[kThreads];
std::atomic<int> next_thread{0};
thread_local ThreadEntry* my_entry = nullptr;

constexpr uintptr_t kNear = 256ull << 20;  // the eboot text is ~100 MB

// The allocator's entry points, which only pass a call on: a sample in
// malloc is charged past them, to whoever asked for the memory.
}  // namespace
}  // namespace rex::ps5diag
extern "C" {
__attribute__((weak)) void* __wrap_malloc(size_t);
__attribute__((weak)) void __wrap_free(void*);
__attribute__((weak)) void* __wrap_calloc(size_t, size_t);
__attribute__((weak)) void* __wrap_realloc(void*, size_t);
__attribute__((weak)) int __wrap_posix_memalign(void**, size_t, size_t);
__attribute__((weak)) void* ps5_heap_malloc(size_t);
__attribute__((weak)) void* ps5_heap_calloc(size_t, size_t);
__attribute__((weak)) void* ps5_heap_memalign(size_t, size_t);
__attribute__((weak)) void* ps5_heap_realloc(void*, size_t);
__attribute__((weak)) void ps5_heap_free(void*);
}
namespace rex::ps5diag {
namespace {
struct PassThrough {
  uint32_t begin = 0, end = 0;  // anchor-relative words, like the samples
};
PassThrough pass_through[16];
int pass_through_count = 0;
void NotePassThrough(const void* function, uint32_t bytes) {
  if (!function || pass_through_count >= 16) return;
  const uint32_t word = uint32_t(reinterpret_cast<uintptr_t>(function) - Anchor() + 0x10000000);
  pass_through[pass_through_count++] = {word, word + bytes};
}
bool IsPassThrough(uint32_t word) {
  for (int i = 0; i < pass_through_count; ++i)
    if (word >= pass_through[i].begin && word < pass_through[i].end) return true;
  return false;
}
void FindPassThroughs() {
  NotePassThrough(reinterpret_cast<const void*>(&__wrap_malloc), 256);
  NotePassThrough(reinterpret_cast<const void*>(&__wrap_free), 256);
  NotePassThrough(reinterpret_cast<const void*>(&__wrap_calloc), 256);
  NotePassThrough(reinterpret_cast<const void*>(&__wrap_realloc), 512);
  NotePassThrough(reinterpret_cast<const void*>(&__wrap_posix_memalign), 256);
  NotePassThrough(reinterpret_cast<const void*>(&ps5_heap_malloc), 1024);
  NotePassThrough(reinterpret_cast<const void*>(&ps5_heap_calloc), 1024);
  NotePassThrough(reinterpret_cast<const void*>(&ps5_heap_memalign), 1024);
  NotePassThrough(reinterpret_cast<const void*>(&ps5_heap_realloc), 1024);
  NotePassThrough(reinterpret_cast<const void*>(&ps5_heap_free), 1024);
  void* (*new_one)(size_t) = &::operator new;
  void* (*new_array)(size_t) = &::operator new[];
  void (*delete_one)(void*) noexcept = &::operator delete;
  void (*delete_array)(void*) noexcept = &::operator delete[];
  void (*delete_sized)(void*, size_t) noexcept = &::operator delete;
  NotePassThrough(reinterpret_cast<const void*>(new_one), 256);
  NotePassThrough(reinterpret_cast<const void*>(new_array), 256);
  NotePassThrough(reinterpret_cast<const void*>(delete_one), 128);
  NotePassThrough(reinterpret_cast<const void*>(delete_array), 128);
  NotePassThrough(reinterpret_cast<const void*>(delete_sized), 128);
}

void SampleHandler(int, siginfo_t*, void* uc) {
  ThreadEntry* e = my_entry;
  if (!e) return;
  auto* gregs = reinterpret_cast<uint64_t*>(reinterpret_cast<uint8_t*>(uc) + 64);
  if (!e->want_stack.load(std::memory_order_acquire)) {
    // a profiler sample: where the thread is, nothing else
    // (The eboot loads low on the PS5, below kNear: anchor - kNear would
    // wrap and drop every sample.)
    const uintptr_t rip = gregs[20], anchor = Anchor();
    const uintptr_t lo = anchor > kNear ? anchor - kNear : 0x10000;
    uint32_t rel = rip > lo && rip < anchor + kNear ? uint32_t(rip - anchor + 0x10000000) : 0;
    uint32_t parent = 0;
    if (rel) {
      // In the game: note the nearest return address into it in another
      // block, most often its caller (malloc's, a shared_ptr release's).
      const uintptr_t rsp = gregs[23] & ~uintptr_t(7);
      uintptr_t end = rsp + 24 * 8;
      if (e->stack_top && e->stack_top < end) end = e->stack_top;
      for (uintptr_t p = rsp; p + 8 <= end; p += 8) {
        const uintptr_t v = *reinterpret_cast<const uintptr_t*>(p);
        if (v > lo && v < anchor + kNear) {
          const uint32_t word = uint32_t(v - anchor + 0x10000000);
          if ((word & ~63u) != (rel & ~63u) && !IsPassThrough(word)) {
            parent = word;
            break;
          }
        }
      }
    } else {
      {
        const uint32_t k = e->library_count.load(std::memory_order_relaxed);
        e->library_rip[k % ThreadEntry::kLibrarySamples] = rip;
        e->library_count.store(k + 1, std::memory_order_release);
      }
      // In a library (libc's memcpy, a kernel wait...): charge the game code
      // that called it, the nearest return address into the eboot on the
      // stack, marked with the top bit; and note the next one in another
      // block, its likely caller.
      const uintptr_t rsp = gregs[23] & ~uintptr_t(7);
      uintptr_t end = rsp + 40 * 8;
      if (e->stack_top && e->stack_top < end) end = e->stack_top;
      for (uintptr_t p = rsp; p + 8 <= end; p += 8) {
        const uintptr_t v = *reinterpret_cast<const uintptr_t*>(p);
        if (v > lo && v < anchor + kNear) {
          const uint32_t word = uint32_t(v - anchor + 0x10000000);
          if (!rel) {
            if (p >= rsp + 24 * 8) break;
            rel = word | 0x80000000u;
          } else if ((word & ~63u) != (rel & 0x7FFFFFC0u) && !IsPassThrough(word)) {
            parent = word;
            break;
          }
        }
      }
    }
    const uint32_t n = e->profile_count.load(std::memory_order_relaxed);
    e->profile_parent[n % ThreadEntry::kProfileSamples] = parent;
    e->profile[n % ThreadEntry::kProfileSamples] = rel;
    e->profile_count.store(n + 1, std::memory_order_release);
    return;
  }
  e->want_stack.store(false, std::memory_order_release);
  e->rip = gregs[20];
  e->rsp = gregs[23];
  uintptr_t anchor = Anchor();
  uintptr_t end = e->rsp + 64 * 1024;
  if (e->stack_top && e->stack_top < end) end = e->stack_top;
  int n = 0;
  for (uintptr_t p = e->rsp & ~uintptr_t(7); p + 8 <= end && n < 64; p += 8) {
    uintptr_t v = *reinterpret_cast<uintptr_t*>(p);
    if (v > (anchor > kNear ? anchor - kNear : 0x10000) && v < anchor + kNear) e->words[n++] = v;
  }
  e->nwords = n;
  e->done.store(1, std::memory_order_release);
}
}  // namespace

namespace {
std::mutex cs_mutex;
std::unordered_map<uint32_t, uint32_t>& CsInits() {
  static auto* m = new std::unordered_map<uint32_t, uint32_t>();
  return *m;
}
}  // namespace
void NoteCriticalSectionInit(uint32_t guest, uint32_t lr) {
  std::lock_guard<std::mutex> lock(cs_mutex);
  CsInits()[guest] = lr ? lr : 1;
}
uint32_t CriticalSectionInitBy(uint32_t guest) {
  std::lock_guard<std::mutex> lock(cs_mutex);
  auto it = CsInits().find(guest);
  return it == CsInits().end() ? 0 : it->second;
}

namespace {
char log_path[512];
// Appends to a fixed buffer without the heap (the report below runs in a
// fault handler, possibly inside the allocator).
struct RawLine {
  char text[2048];
  size_t size = 0;
  void Add(const char* s) {
    while (*s && size + 1 < sizeof(text)) text[size++] = *s++;
  }
  void Hex(uint64_t v) {
    char digits[17];
    int n = 0;
    do {
      digits[n++] = "0123456789ABCDEF"[v & 15];
      v >>= 4;
    } while (v);
    while (n && size + 1 < sizeof(text)) text[size++] = digits[--n];
  }
  void Dec(uint64_t v) {
    char digits[21];
    int n = 0;
    do {
      digits[n++] = char('0' + v % 10);
      v /= 10;
    } while (v);
    while (n && size + 1 < sizeof(text)) text[size++] = digits[--n];
  }
};
}  // namespace

std::atomic<uint64_t> crashed_at_ms{0};

void SetLogPath(const char* path) {
  if (!path) return;
  std::strncpy(log_path, path, sizeof(log_path) - 1);
}

// [ps5 port] No heap and no logger here: the fault can be inside the
// allocator with its lock held (a heap corruption), where the old report's
// first string append waited on that lock forever and the game froze behind
// its loading screen. The line goes straight to the end of the log file.
void UnhandledHostFault(uintptr_t rip, uintptr_t address, uintptr_t rsp) {
  static std::atomic<uint64_t> count{0};
  const uint64_t n = ++count;
  if (n > 1) {
    // Again (it retries the instruction) or another thread: the first report
    // ends the game.
    if (n == 2 && crash_hook) crash_hook("a thread crashed");  // (doesn't return)
    return;
  }
  uint64_t expected = 0;
  crashed_at_ms.compare_exchange_strong(expected, NowMs() | 1);
  const uintptr_t anchor = Anchor();
  static RawLine line;
  line.Add("\n[ps5 crash] thread ");
  line.Dec(uint64_t(my_entry ? my_entry->tid : pthread_getthreadid_np()));
  line.Add(" (");
  line.Add(my_entry ? my_entry->what : "unregistered");
  line.Add(") faulted at ");
  line.Hex(address);
  line.Add(" (rip ");
  line.Hex(rip);
  line.Add(", anchor-relative ");
  line.Hex(rip - anchor + 0x10000000);
  line.Add(") | stack:");
  if (ThreadEntry* e = my_entry) {
    uintptr_t end = rsp + 64 * 1024;
    if (e->stack_top && e->stack_top < end) end = e->stack_top;
    int k = 0;
    for (uintptr_t p = rsp & ~uintptr_t(7); p + 8 <= end && k < 48; p += 8) {
      uintptr_t v = *reinterpret_cast<uintptr_t*>(p);
      if (v > (anchor > kNear ? anchor - kNear : 0x10000) && v < anchor + kNear) {
        line.Add(" ");
        line.Hex(v - anchor + 0x10000000);
        ++k;
      }
    }
  }
  line.Add("\n");
  const char* path = log_path[0] ? log_path : "/app0/logs/crash.log";
  const int fd = open(path, O_WRONLY | O_APPEND | O_CREAT, 0666);
  if (fd >= 0) {
    (void)write(fd, line.text, line.size);
    close(fd);
  }
}

uintptr_t Anchor() { return reinterpret_cast<uintptr_t>(&NowMs); }

namespace {
void InstallSampleHandler() {
  static std::once_flag once;
  std::call_once(once, [] {
    FindPassThroughs();
    struct sigaction sa = {};
    sa.sa_sigaction = SampleHandler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGPROF, &sa, nullptr);
  });
}
}  // namespace

void ProfileTick() {
  InstallSampleHandler();
  const int count = std::min(next_thread.load(), kThreads);
  for (int i = 0; i < count; ++i) {
    ThreadEntry& e = threads[i];
    if (!e.used.load(std::memory_order_acquire) || e.want_stack.load()) continue;
    pthread_kill(e.pthread, SIGPROF);
  }
}

void ProfileReport() {
  struct Busy {
    ThreadEntry* e;
    uint32_t total, inside, via;
    std::unordered_map<uint32_t, uint32_t> hits, callers;
    std::unordered_map<uint64_t, uint32_t> pairs;  // caller block << 32 | its caller's block
    std::unordered_map<uint64_t, uint32_t> code_pairs;  // block << 32 | its caller's block
    std::unordered_map<uint64_t, uint32_t> places;      // library samples by 16-byte address
  };
  std::vector<Busy> busy;
  const int count = std::min(next_thread.load(), kThreads);
  // [ps5 port] The game's render thread (the one the frame limiter runs on)
  // by its own name, not "rex" like every other game thread.
  const uintptr_t title = rex::frame_stats::g_title_thread.load(std::memory_order_relaxed);
  for (int i = 0; i < count; ++i) {
    ThreadEntry& e = threads[i];
    if (!e.used.load(std::memory_order_acquire)) continue;
    if (title && reinterpret_cast<uintptr_t>(e.pthread) == title && std::strcmp(e.what, "title") != 0)
      std::strncpy(e.what, "title", sizeof(e.what) - 1);
    const uint32_t end = e.profile_count.load(std::memory_order_acquire);
    uint32_t begin = e.profile_reported;
    if (end - begin > uint32_t(ThreadEntry::kProfileSamples))
      begin = end - ThreadEntry::kProfileSamples;
    e.profile_reported = end;
    Busy b{&e, end - begin, 0, 0, {}, {}, {}, {}, {}};
    {
      const uint32_t lib_end = e.library_count.load(std::memory_order_acquire);
      uint32_t lib_begin = e.library_reported;
      if (lib_end - lib_begin > uint32_t(ThreadEntry::kLibrarySamples))
        lib_begin = lib_end - ThreadEntry::kLibrarySamples;
      e.library_reported = lib_end;
      for (uint32_t k = lib_begin; k != lib_end; ++k)
        if (const uint64_t rip = e.library_rip[k % ThreadEntry::kLibrarySamples]) ++b.places[rip & ~uint64_t(15)];
    }
    for (uint32_t k = begin; k != end; ++k) {
      const uint32_t pc = e.profile[k % ThreadEntry::kProfileSamples];
      if (!pc) continue;
      // by 64-byte block, so one function's instructions mostly add up
      if (pc & 0x80000000u) {
        ++b.callers[pc & 0x7FFFFFC0u], ++b.via;
        const uint32_t parent = e.profile_parent[k % ThreadEntry::kProfileSamples];
        if (parent) ++b.pairs[uint64_t(pc & 0x7FFFFFC0u) << 32 | (parent & ~63u)];
      } else {
        ++b.hits[pc & ~63u], ++b.inside;
        const uint32_t parent = e.profile_parent[k % ThreadEntry::kProfileSamples];
        if (parent) ++b.code_pairs[uint64_t(pc & ~63u) << 32 | (parent & ~63u)];
      }
    }
    // Threads that ran game code; ones only ever blocked (idle job threads)
    // would otherwise crowd the busy ones out of the report.
    if (b.inside * 20 >= b.total) busy.push_back(std::move(b));
  }
  std::sort(busy.begin(), busy.end(), [](const Busy& a, const Busy& b) { return a.inside > b.inside; });

  REXLOG_INFO("ps5 prof: anchor {:X}, {} threads ran game code", Anchor(), busy.size());
  {
    // Where some library functions are, to name the places below by: a place
    // just past one of these is most likely in it.
    static bool landmarks_logged = false;
    if (!landmarks_logged) {
      landmarks_logged = true;
      void* (*mcpy)(void*, const void*, size_t) = &memcpy;
      void* (*mmove)(void*, const void*, size_t) = &memmove;
      void* (*mset)(void*, int, size_t) = &memset;
      int (*lock)(pthread_mutex_t*) = &pthread_mutex_lock;
      int (*unlock)(pthread_mutex_t*) = &pthread_mutex_unlock;
      int (*cwait)(pthread_cond_t*, pthread_mutex_t*) = &pthread_cond_wait;
      int (*csignal)(pthread_cond_t*) = &pthread_cond_signal;
      int (*sleep)(const timespec*, timespec*) = &nanosleep;
      int (*yield)() = &sched_yield;
      void* (*get)(pthread_key_t) = &pthread_getspecific;
      int (*clock)(clockid_t, timespec*) = &clock_gettime;
      REXLOG_INFO("ps5 prof: landmarks memcpy {:X} memmove {:X} memset {:X} mutex_lock {:X} mutex_unlock {:X} "
                  "cond_wait {:X} cond_signal {:X} nanosleep {:X} sched_yield {:X} getspecific {:X} "
                  "clock_gettime {:X}",
                  uintptr_t(mcpy), uintptr_t(mmove), uintptr_t(mset), uintptr_t(lock), uintptr_t(unlock),
                  uintptr_t(cwait), uintptr_t(csignal), uintptr_t(sleep), uintptr_t(yield), uintptr_t(get),
                  uintptr_t(clock));
    }
  }
  // Every block seen twice or more, 120 to a line: the whole picture, not
  // just the top (a frame's work is spread over many functions).
  auto emit = [](const Busy& b, const char* kind, const std::unordered_map<uint32_t, uint32_t>& map,
                 uint32_t n) {
    std::vector<std::pair<uint32_t, uint32_t>> all(map.begin(), map.end());
    std::sort(all.begin(), all.end(), [](const auto& x, const auto& y) { return x.second > y.second; });
    std::string line;
    int entries = 0, part = 0;
    for (const auto& [pc, hits] : all) {
      if (hits < 2) break;
      char buf[32];
      snprintf(buf, sizeof(buf), " %X:%u", pc, hits);
      line += buf;
      if (++entries == 120) {
        REXLOG_INFO("ps5 prof: tid {} {}: {} samples, {} {} part {}:{}", b.e->tid, b.e->what, b.total,
                    n, kind, ++part, line);
        line.clear();
        entries = 0;
      }
    }
    if (entries || !part)
      REXLOG_INFO("ps5 prof: tid {} {}: {} samples, {} {} part {}:{}", b.e->tid, b.e->what, b.total, n,
                  kind, ++part, line);
  };
  // Library time by caller and that caller's caller ("A/B"), the busiest 120.
  auto emit_pairs = [](const Busy& b, const std::unordered_map<uint64_t, uint32_t>& pairs,
                       const char* kind, uint32_t n) {
    std::vector<std::pair<uint64_t, uint32_t>> all(pairs.begin(), pairs.end());
    std::sort(all.begin(), all.end(), [](const auto& x, const auto& y) { return x.second > y.second; });
    std::string line;
    int entries = 0;
    for (const auto& [key, hits] : all) {
      if (hits < 3 || entries == 120) break;
      char buf[40];
      snprintf(buf, sizeof(buf), " %X/%X:%u", uint32_t(key >> 32), uint32_t(key), hits);
      line += buf;
      ++entries;
    }
    if (entries)
      REXLOG_INFO("ps5 prof: tid {} {}: {} samples, {} {} part 1:{}", b.e->tid, b.e->what, b.total, n,
                  kind, line);
  };
  for (size_t t = 0; t < busy.size() && t < 10; ++t) {
    emit(busy[t], "in game code", busy[t].hits, busy[t].inside);
    emit(busy[t], "in libraries, by caller", busy[t].callers, busy[t].via);
    emit_pairs(busy[t], busy[t].pairs, "in libraries, by caller/parent", busy[t].via);
    emit_pairs(busy[t], busy[t].code_pairs, "in game code, by block/parent", busy[t].inside);
    if (t < 5 && !busy[t].places.empty()) {
      std::vector<std::pair<uint64_t, uint32_t>> all(busy[t].places.begin(), busy[t].places.end());
      std::sort(all.begin(), all.end(), [](const auto& x, const auto& y) { return x.second > y.second; });
      std::string line;
      for (size_t i = 0; i < all.size() && i < 60 && all[i].second >= 2; ++i) {
        char buf[40];
        snprintf(buf, sizeof(buf), " %llX:%u", static_cast<unsigned long long>(all[i].first), all[i].second);
        line += buf;
      }
      REXLOG_INFO("ps5 prof: tid {} {}: library time by place:{}", busy[t].e->tid, busy[t].e->what, line);
    }
  }
}

void RegisterThread(const char* what) {
  if (my_entry) return;
  int i = next_thread.fetch_add(1);
  if (i >= kThreads) return;
  ThreadEntry& e = threads[i];
  e.pthread = pthread_self();
  e.tid = pthread_getthreadid_np();
  int marker = 0;
  e.stack_top = reinterpret_cast<uintptr_t>(&marker) + 256;  // (a little above: the caller's frame)
  std::strncpy(e.what, what ? what : "", sizeof(e.what) - 1);
  my_entry = &e;
  e.used.store(true, std::memory_order_release);
}

void UnregisterThread() {
  ThreadEntry* e = my_entry;
  if (!e) return;
  // No more samples or CPU moves for it: its pthread_t can be another
  // thread's soon, and a sample landing after its thread-specific data is
  // gone would take a lookup that allocates in a signal handler.
  sigset_t prof;
  sigemptyset(&prof);
  sigaddset(&prof, SIGPROF);
  pthread_sigmask(SIG_BLOCK, &prof, nullptr);
  e->used.store(false, std::memory_order_release);
  my_entry = nullptr;
}

extern "C" int32_t scePthreadSetaffinity(pthread_t thread, uint64_t mask);
void KeepOtherThreadsOn(uint64_t mask, uintptr_t title_thread, uintptr_t main_thread,
                        uint64_t main_mask) {
  int count = next_thread.load();
  if (count > kThreads) count = kThreads;
  for (int i = 0; i < count; ++i) {
    ThreadEntry& e = threads[i];
    if (!e.used.load(std::memory_order_acquire)) continue;
    if (!std::strcmp(e.what, "worker") || !std::strcmp(e.what, "recorder") ||
        !std::strcmp(e.what, "capture"))
      continue;
    if (reinterpret_cast<uintptr_t>(e.pthread) == title_thread) continue;
    const bool main = main_thread && reinterpret_cast<uintptr_t>(e.pthread) == main_thread;
    const uint64_t want = main && main_mask ? main_mask : mask;
    // Only when it changes: moving waiting threads every 10 s is the one thing
    // 0.6b2's frozen sessions had in common.
    if (e.applied_mask == want) continue;
    e.applied_mask = want;
    scePthreadSetaffinity(e.pthread, want);
  }
}

void SampleAllThreads() {
  static std::mutex sampling;  // the stall watcher and the hang monitor
  std::lock_guard<std::mutex> one_at_a_time(sampling);
  InstallSampleHandler();
  REXLOG_INFO("ps5 stack: anchor {:X}", Anchor());
  int count = next_thread.load();
  if (count > kThreads) count = kThreads;
  for (int i = 0; i < count; ++i) {
    ThreadEntry& e = threads[i];
    if (!e.used.load(std::memory_order_acquire)) continue;
    e.done.store(0);
    e.want_stack.store(true, std::memory_order_release);
    if (pthread_kill(e.pthread, SIGPROF) != 0) continue;
    for (int ms = 0; ms < 200 && !e.done.load(std::memory_order_acquire); ++ms) usleep(1000);
    if (!e.done.load()) {
      REXLOG_INFO("ps5 stack: tid {} {}: no answer", e.tid, e.what);
      continue;
    }
    std::string line;
    for (int k = 0; k < e.nwords; ++k) {
      char buf[24];
      snprintf(buf, sizeof(buf), " %lX", (unsigned long)(e.words[k] - Anchor() + 0x10000000));
      line += buf;
    }
    REXLOG_INFO("ps5 stack: tid {} {}: rip {:X} |{}", e.tid, e.what,
                e.rip - Anchor() + 0x10000000, line);
  }
}
}  // namespace rex::ps5diag
