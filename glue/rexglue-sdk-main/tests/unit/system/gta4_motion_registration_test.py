#!/usr/bin/env python3
"""Host production-body regression with memory, runtime and sensor boundary fixtures."""

import argparse
import hashlib
import json
import os
import pathlib
import re
import shlex
import subprocess
import tempfile


DISPATCHER_SIGNATURES = (
    "FunctionDispatcher::FunctionDispatcher(",
    "FunctionDispatcher::~FunctionDispatcher()",
    "bool FunctionDispatcher::InitializeFunctionTable(",
    "FunctionDispatcher::ModuleTableInfo* FunctionDispatcher::FindModuleByAddress(",
    "uint32_t FunctionDispatcher::FindCallerModuleBase(",
    "bool FunctionDispatcher::SetFunction(",
    "::PPCFunc* FunctionDispatcher::GetFunction(",
    "uint32_t FunctionDispatcher::AllocateThunk(",
    "void FunctionDispatcher::RegisterModule(",
    "std::optional<std::pair<uint32_t, uint32_t>> FunctionDispatcher::UnregisterModule(",
)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def strip_includes(source):
    # Preserve all other bytes and line numbers, including every function body.
    return re.sub(r"(?m)^[ \t]*#[ \t]*(?:include\b[^\n]*|pragma once[^\n]*)", "", source)


def extract(source, signature):
    """Return an exact source slice; braces in strings/comments are not syntax."""
    start = source.index(signature)
    if signature == "FunctionDispatcher::~FunctionDispatcher()":
        end = source.index(";", start) + 1
    else:
        tokens = re.finditer(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|'
                             r"'(?:\\.|[^'\\])*'|[{}]", source[start:])
        depth = 0
        opened = False
        for token in tokens:
            if token.group() == "{":
                depth += 1
                opened = True
            elif token.group() == "}":
                depth -= 1
                if opened and depth == 0:
                    end = start + token.end()
                    break
        else:
            raise ValueError(f"Unterminated production method: {signature}")
    text = source[start:end]
    return text, {"signature": signature,
                  "byte_start": len(source[:start].encode()), "byte_end": len(source[:end].encode()),
                  "line_start": source.count("\n", 0, start) + 1,
                  "line_end": source.count("\n", 0, end) + 1,
                  "sha256": digest(text.encode())}


PREFIX = r'''
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
#include <rex/ppc/context.h>
#include "gta4_motion_bridge.h"
#ifdef NDEBUG
#error Motion regression requires C++ assertions enabled
#endif
#define CHECK(x) do { if (!(x)) throw std::runtime_error(#x); } while (false)
#define REXLOG_ERROR(...) ((void)0)
#define REXLOG_WARN(...) ((void)0)
#define REXLOG_INFO(...) ((void)0)
#define REX_FATAL(...) throw std::runtime_error("unexpected production REX_FATAL")

// Small byte-backed guest regions, using the real rex::be storage type. Bounds
// failures are test errors, not implicit insertion into a scalar address map.
struct GuestMemory {
  std::array<uint8_t, 0x1000> scratch{};
  std::array<uint8_t, 0x1000> names{};
  uint8_t* Ptr(uint32_t address, size_t size = 1) {
    if (address < scratch.size() && size <= scratch.size() - address)
      return scratch.data() + address;
    if (address >= 0x82037000 && address - 0x82037000 < names.size() &&
        size <= names.size() - (address - 0x82037000))
      return names.data() + (address - 0x82037000);
    throw std::runtime_error("guest memory boundary exceeded");
  }
  uint32_t Load32(uint32_t address) {
    rex::be<uint32_t> value;
    std::memcpy(&value, Ptr(address, 4), 4);
    return value.get();
  }
  void Store32(uint32_t address, uint32_t value) {
    rex::be<uint32_t> stored(value);
    std::memcpy(Ptr(address, 4), &stored, 4);
  }
  std::string_view String(uint32_t address, size_t size) {
    return {reinterpret_cast<char*>(Ptr(address, size)), size};
  }
  std::string CString(uint32_t address) {
    std::string value;
    for (size_t i = 0; i < 100; ++i) {
      char c = *Ptr(address + uint32_t(i));
      if (!c) return value;
      value += c;
    }
    throw std::runtime_error("unterminated fixture string");
  }
  uint8_t* Base() { return reinterpret_cast<uint8_t*>(this); }
};
GuestMemory& Guest(uint8_t* base) { return *reinterpret_cast<GuestMemory*>(base); }
#define REX_LOAD_U32(a) Guest(base).Load32(uint32_t(a))
#define REX_STORE_U32(a, v) Guest(base).Store32(uint32_t(a), uint32_t(v))
#define REX_LOAD_U8(a) (*Guest(base).Ptr(uint32_t(a)))
#define REX_STORE_U8(a, v) (*Guest(base).Ptr(uint32_t(a)) = uint8_t(v))
#define REX_LOAD_STRING(a, n) Guest(base).String(uint32_t(a), n)

// Only the Memory boundary is substituted, not FunctionDispatcher. Its mirror
// also permits deterministic initialization/SetFunction failure and reentrant
// external mutation before allocation returns, exercising nonpublication.
namespace rex::memory {
class Memory {
 public:
  struct Range { uint32_t base, end; };
  std::vector<Range> ranges;
  std::unordered_map<uint32_t, PPCFunc*> mirror;
  bool fail_initialize = false, fail_set = false;
  size_t set_calls = 0;
  void (*after_set)(uint32_t) = nullptr;
  bool InitializeFunctionTable(uint32_t code_base, uint32_t code_size,
                               uint32_t, uint32_t) {
    if (fail_initialize) return false;
    ranges.push_back({code_base, code_base + code_size + 0x10000});
    return true;
  }
  bool SetFunction(uint32_t address, PPCFunc* fn) {
    ++set_calls;
    if (fail_set) return false;
    bool covered = false;
    for (auto r : ranges) covered |= address >= r.base && address < r.end;
    if (!covered) return false;
    if (fn) mirror[address] = fn;
    else mirror.erase(address);
    if (after_set) {
      auto callback = std::exchange(after_set, nullptr);
      callback(address);
    }
    return true;
  }
  void DestroyFunctionTable(uint32_t code_base) {
    for (auto r : ranges) if (r.base == code_base) {
      for (auto it = mirror.begin(); it != mirror.end();) {
        if (it->first >= r.base && it->first < r.end) it = mirror.erase(it);
        else ++it;
      }
    }
    std::erase_if(ranges, [=](auto r) { return r.base == code_base; });
  }
};
}
namespace rex::thread { struct global_critical_region {}; }
'''

BOUNDARIES = r'''
namespace rex {
class Runtime {
 public:
  static inline Runtime* current = nullptr;
  runtime::FunctionDispatcher* dispatcher = nullptr;
  static Runtime* instance() { return current; }
  runtime::FunctionDispatcher* function_dispatcher() { return dispatcher; }
};
}

struct MotionFixture {
  gta4::MotionSnapshot snapshot{};
  std::array<bool, size_t(gta4::MotionPreference::kCount)> preferences{};
  std::array<bool, size_t(gta4::MotionReloadConsumer::kCount)> reload{};
  int read_count = 0, consume_count = 0, set_count = 0;
  uint32_t last_user = ~0u;
} motion;
namespace gta4 {
GTA4MotionBridge& GTA4MotionBridge::Get() { static GTA4MotionBridge bridge; return bridge; }
MotionSnapshot GTA4MotionBridge::Read(uint32_t user) {
  ++motion.read_count; motion.last_user = user; return motion.snapshot;
}
bool GTA4MotionBridge::IsPreferenceEnabled(MotionPreference preference) const {
  return IsPreferenceEnabled(uint32_t(preference));
}
bool GTA4MotionBridge::IsPreferenceEnabled(uint32_t preference) const {
  return preference < motion.preferences.size() && motion.preferences[preference];
}
void GTA4MotionBridge::SetAllPreferences(bool enabled) {
  ++motion.set_count; motion.preferences.fill(enabled);
}
bool GTA4MotionBridge::ConsumeReloadGesture(MotionReloadConsumer consumer, uint32_t user) {
  CHECK(user == 0); ++motion.consume_count;
  return std::exchange(motion.reload.at(size_t(consumer)), false);
}
}

struct Captures { PPCContext original{}, observed{}; int calls = 0, observations = 0; };
thread_local Captures captures;
std::mutex script_mutex;
std::unordered_map<std::string, uint32_t> script_natives;
namespace gta4::quicksave {
void ObserveNativeRegistration(PPCContext& ctx, uint8_t*) {
  std::memcpy(&captures.observed, &ctx, sizeof(ctx)); ++captures.observations;
}
}
extern "C" void __imp__sub_82845600(PPCContext& ctx, uint8_t* base) {
  std::memcpy(&captures.original, &ctx, sizeof(ctx)); ++captures.calls;
  std::lock_guard lock(script_mutex);
  script_natives[Guest(base).CString(ctx.r3.u32)] = ctx.r4.u32;
  ctx.r3.u64 = 0xFEED12345678ABCDull;  // original's return must remain unchanged
}
'''

TESTS = r'''
using Dispatcher = rex::runtime::FunctionDispatcher;
constexpr uint32_t kCodeBase = 0x82000000, kCodeSize = 0xA86300;
constexpr uint32_t kPool = kCodeBase + kCodeSize;
constexpr uint32_t kHistoricalPreferenceThunk = 0x82A8636C;
constexpr uint32_t kContext = 0x100, kArgs = 0x200, kReturn = 0x300;
constexpr uint32_t kOutA = 0x400, kOutB = 0x410, kSentinel = 0xA5A5A5A5;
GuestMemory guest;
rex::Runtime runtime;
void DummyFn(PPCContext&, uint8_t*) {}
void OtherFn(PPCContext&, uint8_t*) {}

struct Session {
  // Reuse the SAME storage and runtime address, but reconstruct the actual
  // production dispatcher. Never reset motion's cached thunk fields here.
  alignas(Dispatcher) std::array<std::byte, sizeof(Dispatcher)> storage{};
  rex::memory::Memory memory;
  Dispatcher* dispatcher = nullptr;
  ~Session() { Destroy(); }
  void Destroy() {
    runtime.dispatcher = nullptr;
    if (dispatcher) dispatcher->~FunctionDispatcher();
    dispatcher = nullptr;
  }
  void Construct(bool initialize = true) {
    Destroy(); memory = {};
    dispatcher = new (storage.data()) Dispatcher(&memory, nullptr);
    CHECK(static_cast<void*>(dispatcher) == static_cast<void*>(storage.data()));
    runtime.dispatcher = dispatcher; rex::Runtime::current = &runtime;
    if (initialize) Initialize();
    script_natives.clear();
  }
  void Initialize() {
    CHECK(dispatcher->InitializeFunctionTable(kCodeBase, kCodeSize, kCodeBase, 0xB00000, true));
    dispatcher->RegisterModule("motion-test", kCodeBase,
      [](rex::runtime::IModuleRegistrar* r) { CHECK(r->SetFunction(kCodeBase, DummyFn)); });
  }
  void Prefill(size_t count) {
    for (size_t i = 0; i < count; ++i)
      CHECK(dispatcher->AllocateThunk(DummyFn, 0x825D2344) == kPool + 4 * i);
  }
};

void InitializeNames() {
  guest.names.fill(0);
  for (auto& r : gta4::g_motion_native_registrations)
    std::memcpy(guest.Ptr(r.name_address, r.name.size() + 1), r.name.data(), r.name.size() + 1);
  std::memcpy(guest.Ptr(0x82037800, 6), "OTHER", 6);
}
PPCContext RegistrationContext(size_t index) {
  auto& r = gta4::g_motion_native_registrations.at(index);
  PPCContext ctx; std::memset(&ctx, 0, sizeof(ctx));
  ctx.r1.u64 = 0xABCD123400000800ull;
  ctx.r3.u64 = 0x1234567800000000ull | r.name_address;
  ctx.r4.u64 = 0x8877665500000000ull | r.original_handler;
  ctx.r5.u64 = 0x1234567890ABCDEFull;
  ctx.r12.u64 = 0xDEADBEEF;
  ctx.lr = 0x1357246800000000ull | r.return_address;
  return ctx;
}
uint32_t Register(PPCContext ctx, bool replaced) {
  PPCContext expected; std::memcpy(&expected, &ctx, sizeof(ctx));
  captures = {};
  sub_82845600(ctx, guest.Base());
  CHECK(captures.calls == 1); CHECK(captures.observations == 1);
  CHECK(std::memcmp(&captures.observed, &expected, sizeof(ctx)) == 0);
  if (replaced) expected.r4.u32 = captures.original.r4.u32;
  CHECK(std::memcmp(&captures.original, &expected, sizeof(ctx)) == 0);
  CHECK(ctx.r3.u64 == 0xFEED12345678ABCDull);
  expected.r3.u64 = ctx.r3.u64;
  CHECK(std::memcmp(&ctx, &expected, sizeof(ctx)) == 0);
  return captures.original.r4.u32;
}
uint32_t Register(size_t index) { return Register(RegistrationContext(index), true); }
uint32_t Lookup(size_t index) {
  std::lock_guard lock(script_mutex);
  return script_natives.at(std::string(gta4::g_motion_native_registrations.at(index).name));
}
void CheckResolved(Session& s, size_t index) {
  const auto& r = gta4::g_motion_native_registrations.at(index);
  uint32_t thunk = Lookup(index);
  auto* handler = s.dispatcher->GetFunction(thunk);
  if (!handler && index == 4 && thunk == kHistoricalPreferenceThunk)
    throw std::runtime_error("stale missing-handler: GET_MOTION_CONTROL_PREFERENCE thunk=0x82A8636C");
  CHECK(handler != nullptr);
  CHECK(handler == r.replacement);
  CHECK(s.memory.mirror.at(thunk) == r.replacement);
  CHECK(thunk == r.thunk_address);
}
void RegisterAll(Session& s) {
  for (size_t i = 0; i < 5; ++i) { Register(i); CheckResolved(s, i); }
}

void Prepare(bool enabled = true, bool fresh = true) {
  motion = {};
  motion.snapshot.sensor_available = true;
  motion.snapshot.controls_enabled = enabled;
  motion.snapshot.fresh = fresh;
  motion.snapshot.pitch_axis = 0.25f; motion.snapshot.roll_axis = -0.5f;
  motion.preferences.fill(true); motion.reload.fill(true);
}
void Invoke(Session& s, size_t index, std::array<uint32_t, 3> args,
            uint32_t return_storage = kReturn) {
  guest.scratch.fill(0xA5);
  guest.Store32(kContext, return_storage);
  guest.Store32(kContext + 8, kArgs);
  for (size_t i = 0; i < args.size(); ++i) guest.Store32(kArgs + 4 * uint32_t(i), args[i]);
  PPCContext ctx; std::memset(&ctx, 0, sizeof(ctx)); ctx.r3.u32 = kContext;
  PPCContext before; std::memcpy(&before, &ctx, sizeof(ctx));
  auto* handler = s.dispatcher->GetFunction(Lookup(index));
  CHECK(handler == gta4::g_motion_native_registrations[index].replacement);
  handler(ctx, guest.Base());
  CHECK(std::memcmp(&ctx, &before, sizeof(ctx)) == 0);
  CHECK(guest.Load32(0) == kSentinel);  // null output/return pointers never write
  CHECK(*guest.Ptr(kReturn - 1) == 0xA5 && *guest.Ptr(kReturn + 4) == 0xA5);
}
void CheckByte(uint32_t address, bool value) {
  CHECK(*guest.Ptr(address) == uint8_t(value));
  CHECK(*guest.Ptr(address - 1) == 0xA5);
  for (uint32_t i = 1; i <= 4; ++i) CHECK(*guest.Ptr(address + i) == 0xA5);
}
void CheckFloat(uint32_t address, float value) {
  CHECK(guest.Load32(address) == std::bit_cast<uint32_t>(value));
  CHECK(*guest.Ptr(address - 1) == 0xA5 && *guest.Ptr(address + 4) == 0xA5);
}
void NativeSemantics(Session& s) {
  for (bool enabled : {false, true}) for (bool fresh : {false, true}) {
    bool available = enabled && fresh;
    for (uint32_t user : {0u, 1u, 0xFFFFFFFFu}) {
      Prepare(enabled, fresh);
      Invoke(s, 0, {user, kOutA, kOutB});
      bool primary_available = user == 0 && available;
      CHECK(guest.Load32(kReturn) == uint32_t(primary_available));
      CheckFloat(kOutA, primary_available ? 0.25f : 0.f);
      CheckFloat(kOutB, primary_available ? -0.5f : 0.f);
      CHECK(motion.read_count == (user == 0 ? 1 : 0));
      if (user == 0) CHECK(motion.last_user == 0);
      Invoke(s, 0, {user, 0, 0}, 0);
    }
    Prepare(enabled, fresh);
    Invoke(s, 1, {kOutA, kOutB, 0});
    CheckByte(kOutA, available); CheckByte(kOutB, available);
    CHECK(guest.Load32(kReturn) == kSentinel);
    CHECK(motion.read_count == 1 && motion.last_user == 0);
    Invoke(s, 1, {0, 0, 0}, 0);
    for (uint32_t preference = 0; preference < 9; ++preference) {
      for (bool selected : {false, true}) {
        Prepare(enabled, fresh);
        if (preference < motion.preferences.size()) motion.preferences[preference] = selected;
        Invoke(s, 4, {preference, 0, 0});
        CHECK(guest.Load32(kReturn) == uint32_t(available && selected && preference < 7));
        CHECK(motion.read_count == 1 && motion.last_user == 0);
        CHECK(guest.Load32(kOutA) == kSentinel);
      }
    }
    Prepare(enabled, fresh); Invoke(s, 4, {~0u, 0, 0}); CHECK(guest.Load32(kReturn) == 0);
    Invoke(s, 4, {0, 0, 0}, 0);
    for (uint32_t user : {0u, 1u, 0xFFFFFFFFu}) for (bool reload_preference : {false, true}) {
      Prepare(enabled, fresh);
      motion.preferences[size_t(gta4::MotionPreference::kReload)] = reload_preference;
      bool permitted = user == 0 && available && reload_preference;
      Invoke(s, 2, {user, kOutA, 0});
      CHECK(guest.Load32(kReturn) == uint32_t(permitted)); CheckByte(kOutA, permitted);
      CHECK(motion.read_count == (user == 0 ? 1 : 0));
      CHECK(motion.consume_count == (permitted ? 1 : 0));
      CHECK(motion.reload[size_t(gta4::MotionReloadConsumer::kScript)] == !permitted);
      CHECK(motion.reload[size_t(gta4::MotionReloadConsumer::kGameplay)]);
      Invoke(s, 2, {user, kOutA, 0});  // successful script consumption is one-shot
      CHECK(guest.Load32(kReturn) == uint32_t(permitted)); CheckByte(kOutA, false);
      CHECK(motion.consume_count == (permitted ? 2 : 0));
      Invoke(s, 2, {user, 0, 0}, 0);
    }
    for (uint32_t value : {0u, 1u, 17u, 0xFFFFFFFFu}) {
      Prepare(enabled, fresh); motion.preferences.fill(value == 0);
      Invoke(s, 3, {value, 0, 0});
      CHECK(motion.set_count == 1); CHECK(motion.read_count == 0);
      for (bool preference : motion.preferences) CHECK(preference == (value != 0));
      CHECK(guest.Load32(kReturn) == kSentinel && guest.Load32(kOutA) == kSentinel);
      Invoke(s, 4, {uint32_t(gta4::MotionPreference::kReload), 0, 0});
      CHECK(guest.Load32(kReturn) == uint32_t(available && value != 0));
    }
  }
  // Check physical BE layout independently of Load32 (not a mutually wrong codec).
  Prepare(); Invoke(s, 0, {0, kOutA, kOutB});
  CHECK(*guest.Ptr(kOutA) == 0x3E && *guest.Ptr(kOutA + 1) == 0x80);
  CHECK(*guest.Ptr(kOutB) == 0xBF && *guest.Ptr(kOutB + 1) == 0);
  CHECK(*guest.Ptr(kReturn) == 0 && *guest.Ptr(kReturn + 3) == 1);
}

void ColdAndSameAddressRestarts() {
  Session s; s.Construct(); InitializeNames();
  for (const auto& r : gta4::g_motion_native_registrations) CHECK(r.thunk_address == 0);
  s.Prefill(23); RegisterAll(s);
  CHECK(Lookup(4) == kHistoricalPreferenceThunk);
  NativeSemantics(s);
  auto old_host = s.dispatcher;
  auto old_thunks = std::array<uint32_t, 5>{};
  for (size_t i = 0; i < 5; ++i) old_thunks[i] = Lookup(i);
  size_t writes = s.memory.set_calls;
  for (int repeat = 0; repeat < 50; ++repeat) RegisterAll(s);
  CHECK(s.memory.set_calls == writes);  // duplicate registration never allocates
  CHECK(s.dispatcher->AllocateThunk(DummyFn, 0) == kHistoricalPreferenceThunk + 4);
  std::puts("PASS cold/all-five semantics/duplicate no-leak; preference=0x82A8636C");

  s.Construct(); CHECK(s.dispatcher == old_host); s.Prefill(0);
  CHECK(s.dispatcher->GetFunction(kHistoricalPreferenceThunk) == nullptr);
  Register(4); CheckResolved(s, 4);  // unchanged C fails HERE, with exact assertion
  CHECK(Lookup(4) == kPool);
  for (size_t i = 0; i < 4; ++i) { Register(i); CheckResolved(s, i); }
  NativeSemantics(s);
  for (size_t prefill : {40u, 3u, 28u, 1u, 60u, 0u, 17u, 80u}) {
    s.Construct(); CHECK(s.dispatcher == old_host); s.Prefill(prefill);
    // All five cached addresses survive each destruction; some now miss and
    // some collide with DummyFn. Registration order deliberately changes.
    for (size_t i : {4u, 2u, 0u, 3u, 1u}) { Register(i); CheckResolved(s, i); }
    writes = s.memory.set_calls;
    RegisterAll(s); CHECK(s.memory.set_calls == writes);
    CHECK(s.dispatcher->AllocateThunk(OtherFn, 0) == kPool + uint32_t(prefill + 5) * 4);
    NativeSemantics(s);
  }
  std::puts("PASS 9 same-host-address dispatcher reconstructions, varied pools, script lookup");
}

void GuardMismatches(Session& s) {
  RegisterAll(s);
  size_t writes = s.memory.set_calls;
  for (size_t i = 0; i < 5; ++i) {
    auto& r = gta4::g_motion_native_registrations[i];
    uint32_t cached = r.thunk_address;
    auto ctx = RegistrationContext(i); ctx.lr ^= 1; Register(ctx, false);
    ctx = RegistrationContext(i); ctx.r3.u32 = 0x82037800; Register(ctx, false);
    ctx = RegistrationContext(i); ctx.r4.u32 ^= 4; Register(ctx, false);
    char first = *guest.Ptr(r.name_address);
    *guest.Ptr(r.name_address) = '?'; Register(RegistrationContext(i), false);
    *guest.Ptr(r.name_address) = first;
    *guest.Ptr(r.name_address + uint32_t(r.name.size())) = 'X';
    Register(RegistrationContext(i), false);
    *guest.Ptr(r.name_address + uint32_t(r.name.size())) = 0;
    CHECK(r.thunk_address == cached);
  }
  CHECK(s.memory.set_calls == writes);
  RegisterAll(s);
}

Dispatcher* poison_dispatcher = nullptr;
PPCFunc* poison_handler = nullptr;
void PoisonAllocation(uint32_t address) {
  CHECK(poison_dispatcher->SetFunction(address, poison_handler));
}
void CollisionsNullAndFailures() {
  Session s; s.Construct(); RegisterAll(s);
  GuardMismatches(s);
  // Same dispatcher pointer, each cached slot now holds a DIFFERENT motion
  // handler, not just a missing entry. Pointer identity cannot pass this test.
  for (size_t i = 0; i < 5; ++i) {
    auto& r = gta4::g_motion_native_registrations[i];
    uint32_t old = r.thunk_address;
    CHECK(s.dispatcher->SetFunction(old, gta4::g_motion_native_registrations[(i + 1) % 5].replacement));
    Register(i); CheckResolved(s, i); CHECK(Lookup(i) != old);
  }
  // An already-correct handler on a new dispatcher may legitimately be reused:
  // validation is handler identity, not runtime/dispatcher pointer identity.
  auto retained = std::array<uint32_t, 5>{};
  for (size_t i = 0; i < 5; ++i) retained[i] = Lookup(i);
  s.Construct();
  for (size_t i = 0; i < 5; ++i)
    CHECK(s.dispatcher->SetFunction(retained[i], gta4::g_motion_native_registrations[i].replacement));
  size_t writes = s.memory.set_calls;
  RegisterAll(s); CHECK(s.memory.set_calls == writes);
  for (size_t i = 0; i < 5; ++i) CHECK(Lookup(i) == retained[i]);

  for (bool absent_runtime : {false, true}) {
    RegisterAll(s);
    runtime.dispatcher = nullptr;
    if (absent_runtime) rex::Runtime::current = nullptr;
    for (size_t i = 0; i < 5; ++i) {
      Register(RegistrationContext(i), false);
      CHECK(gta4::g_motion_native_registrations[i].thunk_address == 0);
      CHECK(Lookup(i) == gta4::g_motion_native_registrations[i].original_handler);
    }
    runtime.dispatcher = s.dispatcher; rex::Runtime::current = &runtime;
    RegisterAll(s);
  }
  // No module / failed Memory initialization leaves no eligible caller pool.
  s.Construct(false); s.memory.fail_initialize = true;
  CHECK(!s.dispatcher->InitializeFunctionTable(kCodeBase, kCodeSize, kCodeBase, 0xB00000, true));
  for (size_t i = 0; i < 5; ++i) {
    Register(RegistrationContext(i), false);
    CHECK(gta4::g_motion_native_registrations[i].thunk_address == 0);
  }
  s.memory.fail_initialize = false; s.Initialize();
  // Failed Memory::SetFunction must roll allocation back and publish nothing.
  s.memory.fail_set = true;
  for (size_t i = 0; i < 5; ++i) {
    Register(RegistrationContext(i), false);
    CHECK(gta4::g_motion_native_registrations[i].thunk_address == 0);
    CHECK(s.dispatcher->GetFunction(kPool) == nullptr);
    CHECK(s.memory.mirror.find(kPool) == s.memory.mirror.end());
  }
  s.memory.fail_set = false; RegisterAll(s); CHECK(Lookup(0) == kPool);

  // A Memory boundary mutation AFTER SetFunction stored the host entry but
  // BEFORE AllocateThunk returns checks the independent publication guard.
  for (PPCFunc* bad : {static_cast<PPCFunc*>(nullptr), &DummyFn}) for (size_t i = 0; i < 5; ++i) {
    auto& r = gta4::g_motion_native_registrations[i];
    CHECK(s.dispatcher->SetFunction(r.thunk_address, OtherFn));
    poison_dispatcher = s.dispatcher; poison_handler = bad;
    s.memory.after_set = PoisonAllocation;
    Register(RegistrationContext(i), false);
    CHECK(Lookup(i) == r.original_handler);
    CHECK(s.dispatcher->GetFunction(r.thunk_address) == bad);
    Register(i); CheckResolved(s, i);
  }
  s.Construct(); s.Prefill(Dispatcher::kThunkReserveSize / 4);
  CHECK(s.dispatcher->AllocateThunk(DummyFn, 0) == 0);
  for (size_t i = 0; i < 5; ++i) {
    Register(RegistrationContext(i), false);
    CHECK(gta4::g_motion_native_registrations[i].thunk_address == 0);
    CHECK(Lookup(i) == gta4::g_motion_native_registrations[i].original_handler);
  }
  std::puts("PASS guard mismatches/original calls, wrong-handler collision, null, failures, exhaustion");
}

void ModuleReloadAndConcurrentDuplicates() {
  Session s; s.Construct(); s.Prefill(23); RegisterAll(s);
  auto host = s.dispatcher;
  auto cleared = s.dispatcher->UnregisterModule("motion-test");
  CHECK(cleared && cleared->first == kPool && cleared->second == kHistoricalPreferenceThunk + 4);
  CHECK(!s.dispatcher->HasAnyFunctionTable());
  CHECK(s.dispatcher->GetFunction(kHistoricalPreferenceThunk) == nullptr);
  CHECK(s.memory.ranges.empty() && s.memory.mirror.empty());
  // First exercise reinitialization directly, with nonzero stale caches intact.
  s.Initialize(); s.Prefill(40); RegisterAll(s); CHECK(s.dispatcher == host);
  NativeSemantics(s);
  cleared = s.dispatcher->UnregisterModule("motion-test"); CHECK(cleared);
  for (size_t i = 0; i < 5; ++i) Register(RegistrationContext(i), false);
  for (const auto& r : gta4::g_motion_native_registrations) CHECK(r.thunk_address == 0);
  s.Initialize(); s.Prefill(7); RegisterAll(s);

  // Stable-runtime concurrent cold registration and duplicate registration.
  // No concurrent runtime destruction; no TSan or lifecycle-race claim.
  for (auto& r : gta4::g_motion_native_registrations) {
    CHECK(s.dispatcher->SetFunction(r.thunk_address, DummyFn));
  }
  size_t before = s.memory.set_calls;
  std::atomic<int> errors{0}; std::vector<std::thread> threads;
  for (size_t thread = 0; thread < 8; ++thread) threads.emplace_back([&, thread] {
    try {
      for (size_t n = 0; n < 100; ++n) Register((n + thread) % 5);
    } catch (...) { ++errors; }
  });
  for (auto& thread : threads) thread.join();
  CHECK(errors == 0); CHECK(s.memory.set_calls == before + 5);
  RegisterAll(s); CHECK(s.memory.set_calls == before + 5);
  NativeSemantics(s);
  std::puts("PASS actual module unregister/reinitialize and 800 stable-runtime concurrent registrations");
}
int main() {
  try {
    ColdAndSameAddressRestarts();
    CollisionsNullAndFailures();
    ModuleReloadAndConcurrentDuplicates();
    std::puts("PASS motion registration regression: all assertions enabled; no TSan claim");
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "FAIL: %s\n", e.what());
    return 1;
  }
}
'''


def run(args, output):
    root = args.root.resolve()
    if not (root / "gta4-recomp").is_dir():
        root = root / "glue/rexglue-sdk-main"
    simde = (args.simde or root / "thirdparty/simde").resolve()
    if not (simde / "simde/x86/sse2.h").is_file():
        raise ValueError("SIMDe headers unavailable; provide --simde with a local dependency directory")
    motion_path = (args.original_motion or root / "gta4-recomp/src/gta4_motion_hooks.cpp").resolve()
    dispatcher_path = root / "src/system/function_dispatcher.cpp"
    header_path = root / "include/rex/system/function_dispatcher.h"
    # One immutable snapshot per input: never rewrite or patch production code.
    sources = {path: path.read_bytes() for path in (motion_path, dispatcher_path, header_path)}
    motion_source = sources[motion_path].decode()
    dispatcher_source = sources[dispatcher_path].decode()
    header_source = sources[header_path].decode()
    methods, scopes = zip(*(extract(dispatcher_source, signature) for signature in DISPATCHER_SIGNATURES))
    source = (PREFIX + strip_includes(header_source) + "\nnamespace rex::runtime {\n" +
              "\n\n".join(methods) + "\n}\n" + BOUNDARIES +
              strip_includes(motion_source) + TESTS)
    cpp = output / "fixture.cpp"
    cpp.write_text(source)
    (output / "extraction.json").write_text(json.dumps({
        "motion": {"path": str(motion_path), "scope": "complete file, includes removed only",
                   "body_sha256": digest(strip_includes(motion_source).encode())},
        "dispatcher_header": {"path": str(header_path),
                              "scope": "complete actual header including all fields; includes/pragma removed only"},
        "dispatcher_methods": {"path": str(dispatcher_path), "scope": list(scopes)},
        "not_extracted": ["dispatcher Execute/ExecuteInterrupt/indirect trap",
                          "Memory implementation", "runtime ownership/lifetime",
                          "motion filtering/sensors/cvars", "generated script native registry/lookup"],
        "boundaries": ["byte-backed guest regions with actual rex::be and actual PPCContext",
                       "Memory table mirror + deterministic failure/reentrant-mutation injection",
                       "runtime pointer", "quicksave observer", "script registration/lookup storage",
                       "actual motion bridge header + fixture method definitions"],
        "concurrency": "stable-runtime registration only; no concurrent teardown, no TSan",
    }, indent=2) + "\n")
    command = [args.cxx, "-std=c++23", "-O1", "-g", "-UNDEBUG", "-pthread", "-Wno-psabi",
               "-I", str(root / "include"), "-I", str(simde),
               "-I", str(root / "gta4-recomp/src"), "-MD", "-MF", str(output / "deps.d"),
               str(cpp), "-o", str(output / "fixture")]
    if args.sanitize:
        command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-pie", "-no-pie"]
    env = os.environ.copy()
    if args.sanitize:
        env["ASAN_OPTIONS"] = "detect_leaks=1:halt_on_error=1:abort_on_error=1"
        env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
    version = subprocess.run([args.cxx, "--version"], capture_output=True, text=True, check=True).stdout
    print("Compile:", shlex.join(command), flush=True)
    compile_result = subprocess.run(command, capture_output=True, text=True)
    (output / "compile.log").write_text(compile_result.stdout + compile_result.stderr)
    print(compile_result.stdout + compile_result.stderr, end="", flush=True)
    metadata = {"root": str(root), "simde": str(simde), "original_motion": bool(args.original_motion),
                "compiler": version, "command": command, "sanitize": args.sanitize,
                "sanitizer_environment": {k: env[k] for k in ("ASAN_OPTIONS", "UBSAN_OPTIONS") if k in env},
                "compile_returncode": compile_result.returncode,
                "sources": {str(path): digest(data) for path, data in sources.items()},
                "fixture_sha256": digest(cpp.read_bytes())}
    deps = output / "deps.d"
    if deps.exists():
        paths = shlex.split(deps.read_text().replace("\\\n", " ").split(":", 1)[1])
        metadata["dependency_sha256"] = {str(pathlib.Path(p).resolve()): digest(pathlib.Path(p).read_bytes())
                                         for p in paths if pathlib.Path(p).is_file()}
    if compile_result.returncode:
        (output / "result.json").write_text(json.dumps(metadata, indent=2) + "\n")
        return compile_result.returncode
    result = subprocess.run([str(output / "fixture")], capture_output=True, text=True, env=env)
    (output / "run.log").write_text(result.stdout + result.stderr)
    print(result.stdout + result.stderr, end="", flush=True)
    metadata["run_returncode"] = result.returncode
    metadata["inputs_unchanged"] = all(path.read_bytes() == data for path, data in sources.items())
    (output / "result.json").write_text(json.dumps(metadata, indent=2) + "\n")
    if not metadata["inputs_unchanged"]:
        raise RuntimeError("Production sources changed during test; rerun against a stable fix")
    return result.returncode if result.returncode >= 0 else 128 - result.returncode


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=pathlib.Path, default=pathlib.Path(__file__).resolve().parents[3])
    parser.add_argument("--cxx", default="clang++")
    parser.add_argument("--sanitize", action="store_true", help="enable ASan + UBSan, fail on findings")
    parser.add_argument("--out", type=pathlib.Path, help="NEW directory retaining fixture, hashes, scope, and logs")
    parser.add_argument("--original-motion", type=pathlib.Path,
                        help="compile this unchanged motion file as a negative control; failure is not inverted")
    parser.add_argument("--simde", type=pathlib.Path, help="local SIMDe dependency directory (no download)")
    args = parser.parse_args()
    if args.out:
        output = args.out.resolve()
        output.mkdir(parents=True, exist_ok=False)  # never overwrite previous evidence
        return run(args, output)
    with tempfile.TemporaryDirectory(prefix="gta4-motion-registration-") as directory:
        return run(args, pathlib.Path(directory))


if __name__ == "__main__":
    raise SystemExit(main())
