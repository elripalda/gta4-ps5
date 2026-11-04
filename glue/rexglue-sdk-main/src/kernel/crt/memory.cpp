/**
 * @file        kernel/crt/memory.cpp
 *
 * @brief       Native memory operation hooks -- replaces recompiled PPC
 *              implementations of memcpy, memmove, memset, etc.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 * @license     BSD 3-Clause License
 */
#include <cstring>

#include <rex/hook.h>
#if REX_PLATFORM_PS5
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(ps5_memory_census, true, "PS5",
                    "Every 10 s, log which game code cleared and copied the most memory (memset, memcpy and "
                    "their Xbox variants), by caller");

namespace rex::kernel::crt::census {
// [ps5 port] 1.0.3 Test 1's profile had one of the game's threads spend about
// 60% of its time in memset. Which code clears (and copies) that much, how
// often and in what sizes: the guest return address of each call (passed by
// the hooks below), counted in a fixed table. No thread_local: on the PS5
// each one is a lookup through the system, on the game's hottest call.
struct Slot {
  std::atomic<uint32_t> caller{0};
  std::atomic<uint64_t> calls{0}, bytes{0}, largest{0};
};
constexpr uint32_t kSlots = 1024;
std::array<Slot, kSlots> g_set, g_copy;
std::atomic<uint64_t> g_set_bytes{0}, g_copy_bytes{0};
std::atomic<int64_t> g_reported_ns{0};
std::mutex g_report_mutex;

int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// (returns the slot's call count before this one)
uint64_t Count(std::array<Slot, kSlots>& table, uint32_t caller, uint64_t n) {
  uint32_t i = (caller * 2654435761u) >> 22;  // (10 bits)
  for (uint32_t probe = 0; probe < 16; ++probe, i = (i + 1) % kSlots) {
    Slot& slot = table[i];
    uint32_t key = slot.caller.load(std::memory_order_relaxed);
    if (key == 0 && slot.caller.compare_exchange_strong(key, caller, std::memory_order_relaxed)) key = caller;
    if (key != caller) continue;
    const uint64_t calls = slot.calls.fetch_add(1, std::memory_order_relaxed);
    slot.bytes.fetch_add(n, std::memory_order_relaxed);
    uint64_t largest = slot.largest.load(std::memory_order_relaxed);
    while (n > largest && !slot.largest.compare_exchange_weak(largest, n, std::memory_order_relaxed)) {}
    return calls;
  }
  return 0;
}

void Report(const char* what, std::array<Slot, kSlots>& table, uint64_t total, double seconds) {
  struct Row { uint32_t caller; uint64_t calls, bytes, largest; };
  std::vector<Row> rows;
  for (Slot& slot : table) {
    const uint32_t caller = slot.caller.load(std::memory_order_relaxed);
    if (!caller) continue;
    const Row row{caller, slot.calls.exchange(0, std::memory_order_relaxed),
                  slot.bytes.exchange(0, std::memory_order_relaxed),
                  slot.largest.exchange(0, std::memory_order_relaxed)};
    if (row.calls) rows.push_back(row);
  }
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.bytes > b.bytes; });
  std::string line;
  for (size_t i = 0; i < rows.size() && i < 12; ++i) {
    const Row& r = rows[i];
    line += fmt::format(" {:08X}: {} calls {:.1f} MB (avg {} B, largest {} KB);", r.caller, r.calls,
                        double(r.bytes) / 1048576.0, r.bytes / r.calls, r.largest / 1024);
  }
  REXLOG_INFO("ps5 {}: {:.0f} MB a second over {:.0f} s; by caller:{}", what,
              double(total) / 1048576.0 / seconds, seconds, line);
}

void Note(bool copy, uint64_t n, uint32_t caller) {
  if (!REXCVAR_GET(ps5_memory_census)) return;
  const uint64_t calls = Count(copy ? g_copy : g_set, caller, n);
  (copy ? g_copy_bytes : g_set_bytes).fetch_add(n, std::memory_order_relaxed);
  // every 10 s, from whichever thread gets here first (the clock read only
  // on every 256th call from one caller: a system call on each would cost
  // more than the count)
  if ((calls & 255) != 255) return;
  const int64_t now = NowNs();
  int64_t last = g_reported_ns.load(std::memory_order_relaxed);
  if (!last) {
    g_reported_ns.compare_exchange_strong(last, now, std::memory_order_relaxed);
    return;
  }
  if (now - last < 10'000'000'000ll) return;
  if (!g_reported_ns.compare_exchange_strong(last, now, std::memory_order_relaxed)) return;
  std::lock_guard lock(g_report_mutex);
  const double seconds = double(now - last) / 1e9;
  Report("memset", g_set, g_set_bytes.exchange(0, std::memory_order_relaxed), seconds);
  Report("memcpy", g_copy, g_copy_bytes.exchange(0, std::memory_order_relaxed), seconds);
}
}  // namespace rex::kernel::crt::census
#endif

namespace rex::kernel::crt {

// ---------------------------------------------------------------------------
// Standard memory operations
// ---------------------------------------------------------------------------

static void* native_memcpy(void* dst, const void* src, size_t n) {
  return std::memcpy(dst, src, n);
}

static void* native_memmove(void* dst, const void* src, size_t n) {
  return std::memmove(dst, src, n);
}

static void* native_memset(void* dst, int val, size_t n) {
  return std::memset(dst, val, n);
}

static void* native_memchr(const void* ptr, int val, size_t n) {
  return const_cast<void*>(std::memchr(ptr, val, n));
}

// ---------------------------------------------------------------------------
// Xbox/VMX-optimized variants (same semantics, native speed)
// ---------------------------------------------------------------------------

static void* native_XMemCpy(void* dst, const void* src, size_t n) {
  return std::memcpy(dst, src, n);
}

static void* native_XMemSet(void* dst, int val, size_t n) {
  return std::memset(dst, val, n);
}

static void* native_XMemSet128(void* dst, int val, size_t n) {
  return std::memset(dst, val, n);
}

static void* native_memset_vmx(void* dst, int val, size_t n) {
  return std::memset(dst, val, n);
}

// ---------------------------------------------------------------------------
// Secure variants (return errno_t)
// ---------------------------------------------------------------------------

static int native_memcpy_s(void* dst, size_t dstsz, const void* src, size_t count) {
  if (!dst || !src || count > dstsz)
    return 22;  // EINVAL
  std::memcpy(dst, src, count);
  return 0;
}

static int native_memmove_s(void* dst, size_t dstsz, const void* src, size_t count) {
  if (!dst || !src || count > dstsz)
    return 22;  // EINVAL
  std::memmove(dst, src, count);
  return 0;
}

}  // namespace rex::kernel::crt

#if REX_PLATFORM_PS5
REX_HOOK_RAW(rexcrt_memcpy) {
  rex::kernel::crt::census::Note(true, ctx.r5.u32, uint32_t(ctx.lr));
  rex::ppc::HostToGuestFunction<rex::kernel::crt::native_memcpy>(ctx, base);
}
#else
REX_HOOK(rexcrt_memcpy, rex::kernel::crt::native_memcpy)
#endif
#if REX_PLATFORM_PS5
REX_HOOK_RAW(rexcrt_memmove) {
  rex::kernel::crt::census::Note(true, ctx.r5.u32, uint32_t(ctx.lr));
  rex::ppc::HostToGuestFunction<rex::kernel::crt::native_memmove>(ctx, base);
}
#else
REX_HOOK(rexcrt_memmove, rex::kernel::crt::native_memmove)
#endif
#if REX_PLATFORM_PS5
REX_HOOK_RAW(rexcrt_memset) {
  rex::kernel::crt::census::Note(false, ctx.r5.u32, uint32_t(ctx.lr));
  rex::ppc::HostToGuestFunction<rex::kernel::crt::native_memset>(ctx, base);
}
#else
REX_HOOK(rexcrt_memset, rex::kernel::crt::native_memset)
#endif
REX_HOOK(rexcrt_memchr, rex::kernel::crt::native_memchr)
#if REX_PLATFORM_PS5
REX_HOOK_RAW(rexcrt_XMemCpy) {
  rex::kernel::crt::census::Note(true, ctx.r5.u32, uint32_t(ctx.lr));
  rex::ppc::HostToGuestFunction<rex::kernel::crt::native_XMemCpy>(ctx, base);
}
#else
REX_HOOK(rexcrt_XMemCpy, rex::kernel::crt::native_XMemCpy)
#endif
#if REX_PLATFORM_PS5
REX_HOOK_RAW(rexcrt_XMemSet) {
  rex::kernel::crt::census::Note(false, ctx.r5.u32, uint32_t(ctx.lr));
  rex::ppc::HostToGuestFunction<rex::kernel::crt::native_XMemSet>(ctx, base);
}
#else
REX_HOOK(rexcrt_XMemSet, rex::kernel::crt::native_XMemSet)
#endif
#if REX_PLATFORM_PS5
REX_HOOK_RAW(rexcrt_XMemSet128) {
  rex::kernel::crt::census::Note(false, ctx.r5.u32, uint32_t(ctx.lr));
  rex::ppc::HostToGuestFunction<rex::kernel::crt::native_XMemSet128>(ctx, base);
}
#else
REX_HOOK(rexcrt_XMemSet128, rex::kernel::crt::native_XMemSet128)
#endif
#if REX_PLATFORM_PS5
REX_HOOK_RAW(rexcrt_memset_vmx) {
  rex::kernel::crt::census::Note(false, ctx.r5.u32, uint32_t(ctx.lr));
  rex::ppc::HostToGuestFunction<rex::kernel::crt::native_memset_vmx>(ctx, base);
}
#else
REX_HOOK(rexcrt_memset_vmx, rex::kernel::crt::native_memset_vmx)
#endif
REX_HOOK(rexcrt_memcpy_s, rex::kernel::crt::native_memcpy_s)
REX_HOOK(rexcrt_memmove_s, rex::kernel::crt::native_memmove_s)
