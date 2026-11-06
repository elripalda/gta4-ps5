// [ps5 port] Where the game runs out of room (the "Lure" rooftop hang).
//
// Players reach the rooftop in "Lure", the screen goes black and never comes
// back: the game's main thread loops for good in its allocator, asking its
// streaming code to free room for one allocation (sub_824E2D60 calls
// sub_821CFF10 until the heap's largest free block fits).
//
// Why it never ends: the port raises the streamed-resource budget above
// retail's (gta4_streaming_hooks.cpp: 221.5 MB to about 256 MB of a 272 MB
// heap). The same heap holds everything else the game allocates, so it can be
// out of room (or too broken up for one block) while the streamer is still
// under its budget, and the streamer only frees anything when over budget:
// each try answers "there is room" and frees nothing. So while an allocation
// waits, each try is run as if the budget ended at what is in use now, and the
// streamer evicts as retail would at its own budget, until the block fits;
// the budget is back as it was before the try returns. Nothing changes for an
// allocation that fits.
//
// Also logged: what waited, from where, and how the budgets stood, once a wait
// has gone on for a while; and every large allocation, with where it came from.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <utility>

#include <fmt/format.h>
#include <rex/logging.h>

#include "gta4_init.h"

namespace {

constexpr uint32_t kCodeBegin = 0x82140000, kCodeEnd = 0x82140000 + 0x94635C;
// The streaming budgets sub_824E2980 compares (used/limit pairs at +32/+36
// and +44/+48 of this block).
constexpr uint32_t kStreamingBudgets = 0x82A9AA7C;
// Streamed physical memory: the budget and what is in use (bytes).
constexpr uint32_t kPhysicalLimit = kStreamingBudgets + 44, kPhysicalUsed = kStreamingBudgets + 48;
// Tries left as the game made them before the streamer is asked to evict
// (room can come back on its own: another thread freeing, a late release).
constexpr uint64_t kTriesBeforeEvicting = 32;

struct Wait {
  uint32_t size = 0, caller = 0, stack = 0;
  uint64_t tries = 0, evicting_tries = 0;
  std::chrono::steady_clock::time_point start{};
  bool logged = false;
};
thread_local Wait t_wait;

// Guest return addresses on the stack, nearest first (a scan: some may be stale).
std::string GuestTrail(uint8_t* base, uint32_t stack) {
  std::string out;
  int found = 0;
  for (uint32_t offset = 0; offset < 1536 && found < 16; offset += 4) {
    const uint32_t word = REX_LOAD_U32(stack + offset);
    if (word >= kCodeBegin && word < kCodeEnd && !(word & 3)) {
      out += fmt::format("{}{:08X}", found ? " < " : "", word);
      ++found;
    }
  }
  return out;
}

void LogWait(uint8_t* base, const char* what) {
  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - t_wait.start).count();
  std::string budgets;
  for (uint32_t i = 0; i < 16; ++i)
    budgets += fmt::format("{}{:08X}", i ? " " : "", REX_LOAD_U32(kStreamingBudgets + i * 4));
  REXLOG_WARN("ps5 memory: {} the game is waiting for room for {} KB ({} bytes) from game code {:08X}, "
              "{} tries ({} evicting) over {:.1f} s; called from {}; streaming budgets [{}]",
              what, (t_wait.size + 1023) / 1024, t_wait.size, t_wait.caller, t_wait.tries,
              t_wait.evicting_tries, seconds, GuestTrail(base, t_wait.stack), budgets);
}

std::mutex g_large_mutex;
std::set<std::pair<uint32_t, uint32_t>> g_large_seen;

// Tries running with the budget lowered, and the budget to put back after them.
std::mutex g_evict_mutex;
int g_evicting = 0;
uint32_t g_evict_saved_limit = 0;

}  // namespace

// The allocation that waits for room (r3: bytes).
extern "C" void sub_824E2D60(PPCContext& ctx, uint8_t* base) {
  const Wait outer = t_wait;
  t_wait = Wait{ctx.r3.u32, uint32_t(ctx.lr), ctx.r1.u32, 0, 0, {}, false};
  __imp__sub_824E2D60(ctx, base);
  if (t_wait.logged || t_wait.evicting_tries) LogWait(base, "done:");
  t_wait = outer;
}

// One try at making room, while that allocation waits (r3 + r4: bytes).
extern "C" void sub_821CFF10(PPCContext& ctx, uint8_t* base) {
  if (!t_wait.size) {
    __imp__sub_821CFF10(ctx, base);
    return;
  }
  if (!t_wait.tries++) t_wait.start = std::chrono::steady_clock::now();
  if (t_wait.tries == 20000 || (t_wait.tries > 20000 && t_wait.tries % 1000000 == 0)) {
    t_wait.logged = true;
    LogWait(base, "stuck:");
  }
  // Under budget, the streamer frees nothing: this try runs with the budget
  // ending at what is in use, so it evicts (sub_824E2980 checks used > limit - bytes).
  // (Threads stuck at once share it: the last one out puts back the budget the
  // first one found. The lock is never held over game code.)
  bool evict = false;
  if (t_wait.tries > kTriesBeforeEvicting) {
    std::lock_guard lock(g_evict_mutex);
    const uint32_t limit = g_evicting ? g_evict_saved_limit : REX_LOAD_U32(kPhysicalLimit);
    const uint32_t used = REX_LOAD_U32(kPhysicalUsed);
    if (limit && used <= limit) {
      evict = true;
      if (!g_evicting++) g_evict_saved_limit = limit;
      if (!t_wait.evicting_tries++)
        REXLOG_INFO("ps5 memory: no room for {} KB under the streaming budget ({} of {} MB in use); "
                    "freeing streamed resources for it", (t_wait.size + 1023) / 1024, used >> 20, limit >> 20);
      REX_STORE_U32(kPhysicalLimit, used);
    }
  }
  __imp__sub_821CFF10(ctx, base);
  if (evict) {
    std::lock_guard lock(g_evict_mutex);
    if (!--g_evicting) REX_STORE_U32(kPhysicalLimit, g_evict_saved_limit);
  }
}

// The game's aligned allocations (r3: bytes): the large ones, once per caller
// and size, with where they came from.
extern "C" void sub_82812260(PPCContext& ctx, uint8_t* base) {
  const uint32_t size = ctx.r3.u32;
  if (size >= (2u << 20) && size != 0xFFFFFFFFu) {
    const uint32_t caller = uint32_t(ctx.lr);
    bool first = false;
    {
      std::lock_guard lock(g_large_mutex);
      if (g_large_seen.size() < 4096) first = g_large_seen.insert({caller, size}).second;
    }
    if (first)
      REXLOG_INFO("ps5 memory: the game allocates {} KB from game code {:08X}; called from {}",
                  (size + 1023) / 1024, caller, GuestTrail(base, ctx.r1.u32));
  }
  __imp__sub_82812260(ctx, base);
}
