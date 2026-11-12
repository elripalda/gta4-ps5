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
    src = root / "gta4-recomp/src"
    quick = (src / "gta4_quicksave_hooks.cpp").read_text()
    front = (src / "gta4_frontend_hooks.cpp").read_text()
    trace = (src / "gta4_font_selection_trace.cpp").read_text()
    app = (src / "gta4_app.cpp").read_text()
    ending = function(app, "void GTA4App::OnSessionEnding()")
    for reset in ("gta4::frontend_menu::ResetSession();",
                  "gta4::quicksave::ResetSession();", "GTA4_FontSelectionTraceResetSession();"):
        assert reset in ending
        assert ending.index(reset) > ending.index("OnShutdown();")
    restart = (root / "src/ui/rex_app.cpp").read_text()
    assert "OnSessionEnding();" in function(restart, "bool ReXApp::RestartSessionForHub(")
    source = r'''
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>
#include "gta4_quicksave_policy.h"
#include "gta4_frontend_menu_policy.h"
#include "ps5/lifecycle/f02_return_prompt.h"
#define REXLOG_INFO(...) ((void)0)
#define REXCVAR_GET(x) enabled
#define REX_RAW_ADDR(a) (base + (a))
#define REX_LOAD_U8(a) base[a]
union Register { uint64_t u64 = 0; uint32_t u32; };
struct PPCContext { Register r3, r4; };
std::array<uint8_t, 16384> arena{};
bool enabled = true, fail_allocation = false;
uint32_t allocation_cursor = 1024;
int allocations = 0, frees = 0;
namespace rex::memory {
struct Heap {};
struct Memory {
  Heap heap;
  Heap* LookupHeap(uint32_t address) { return address && address < arena.size() ? &heap : nullptr; }
  uint32_t SystemHeapAlloc(uint32_t size) {
    ++allocations;
    if (fail_allocation) return 0;
    const uint32_t result = allocation_cursor;
    allocation_cursor += size;
    assert(allocation_cursor < arena.size());
    return result;
  }
};
uint8_t* GuestPtr(uint8_t* base, uint32_t address) { return base + address; }
}
rex::memory::Memory memory;
struct Kernel { rex::memory::Memory* memory() { return &::memory; } } kernel;
#define REX_KERNEL_STATE() (&::kernel)
struct Dispatcher {
  uint32_t next = 0x9000;
  std::unordered_set<uint32_t> registered;
  uint32_t AllocateThunk(void (*)(PPCContext&, uint8_t*), uint32_t) {
    auto address = next; next += 4; registered.insert(address); return address;
  }
} dispatcher;
namespace rex {
struct Runtime {
  static Runtime* instance() { static Runtime value; return &value; }
  Dispatcher* function_dispatcher() { return &dispatcher; }
};
}
void __imp__sub_821B3510(PPCContext& ctx, uint8_t*) {
  ctx.r3.u64 = memory.SystemHeapAlloc(ctx.r3.u32);
}
void __imp__sub_821B3560(PPCContext&, uint8_t*) { ++frees; }
'''
    source += "\nnamespace gta4::quicksave {\n"
    source += quick[quick.index("std::mutex state_mutex"):quick.index("bool Span(")]
    source += "bool Span(uint8_t*, uint32_t address, size_t bytes) { return address && address + bytes <= arena.size(); }\n"
    source += quick[quick.index("struct NativeEntry"):quick.index("uint32_t NativeAddress(")]
    source += quick[quick.index("struct Text {"):quick.index("void Notify(")]
    source += "void NativeRequest(PPCContext&, uint8_t*) {}\nvoid NativeObserve(PPCContext&, uint8_t*) {}\n"
    source += function(quick, "bool EnsureThunks()")
    source += function(quick, "void ResetSession()")
    source += "\n}\nnamespace frontend_test {\n"
    source += function(front, "enum class TextId") + ";\n"
    source += front[front.index("struct NativePageState"):front.index("std::filesystem::path g_config_path")]
    source += "uint32_t g_menu_trace_label_calls = 0, g_menu_trace_value_calls = 0;\n"
    source += function(front, "void ResetSession()")
    source += "\n}\nnamespace trace_test {\n"
    source += trace[trace.index("struct Budget {"):trace.index("struct FontState {")]
    source += function(trace, "void GTA4_FontSelectionTraceResetSession()")
    source += r'''
}
void ReplaceArena() {
  arena.fill(0xCD);
  allocation_cursor = 4096;
  dispatcher.registered.clear();
  dispatcher.next = 0xA000;
}
int main() {
  {
    using namespace gta4::quicksave;
    assert(EnsureText(arena.data()) && EnsureThunks());
    const uint32_t old_pool = text_pool, old_thunk = request_thunk;
    ReplaceArena();
    assert(EnsureText(arena.data()) && text_pool == old_pool && arena[old_pool] == 0xCD);
    assert(EnsureThunks() && request_thunk == old_thunk && !dispatcher.registered.contains(old_thunk));
    std::puts("PASS negative control: same Memory address retains stale text and unregistered thunks");
    for (int cycle = 0; cycle < 6; ++cycle) {
      installed.fill(true); program_addresses.fill(123); natives[0].handler = 99;
      const uint64_t epoch = world_epoch.load();
      ResetSession();
      assert(!text_pool && !text_memory && !request_thunk && !observe_thunk);
      assert(!installed[0] && !program_addresses[0] && !natives[0].handler);
      assert(world_epoch.load() == epoch + 1 && transaction.phase() == Phase::kIdle);
      for (auto& text : texts) assert(!text.address && !text.key_address);
      assert(EnsureText(arena.data()) && EnsureThunks());
      for (auto& text : texts) {
        assert(std::strcmp(reinterpret_cast<char*>(arena.data() + text.address), text.value.data()) == 0);
        assert(std::strcmp(reinterpret_cast<char*>(arena.data() + text.key_address), text.key.data()) == 0);
      }
      assert(dispatcher.registered.contains(request_thunk) && dispatcher.registered.contains(observe_thunk));
      const int count = allocations;
      assert(EnsureText(arena.data()) && allocations == count);
      ReplaceArena();
    }
    assert(frees == 0);
    std::puts("PASS Quicksave: six Memory/dispatcher reuses, all text and callable owners renewed");
  }
  {
    using namespace frontend_test;
    g_game_menu.active = true;
    g_game_menu.rows = g_game_menu.allocation = g_game_menu.label_address = 123;
    g_game_menu.stock_rows = g_game_menu.confirm_key_address = g_game_menu.dialog_text_address = 456;
    g_display_menu.allocation = g_display_menu.string_pool = 789;
    g_display_menu.text_addresses.fill(123);
    g_display_menu.native_pages.push_back({});
    g_return_confirmation_pause_state = 18;
    g_return_confirmation_dialog_open = true;
    const auto before = arena;
    ResetSession();
    assert(!g_game_menu.active && !g_game_menu.rows && !g_game_menu.allocation);
    assert(!g_game_menu.stock_rows && !g_game_menu.label_address);
    assert(!g_game_menu.confirm_key_address && !g_game_menu.dialog_text_address);
    assert(!g_display_menu.allocation && !g_display_menu.string_pool && g_display_menu.native_pages.empty());
    for (auto address : g_display_menu.text_addresses) assert(!address);
    assert(!g_return_confirmation_pause_state && !g_return_confirmation_dialog_open);
    assert(arena == before && frees == 0);
    std::puts("PASS frontend: ownership discarded without guest writes or stale frees");
  }
  {
    using namespace trace_test;
    for (auto* budget : {&texts, &glyphs, &bindings, &selections}) {
      assert(budget->Take("same-address", 1, "test"));
      assert(!budget->Take("same-address", 1, "test"));
      assert(!budget->Take("over-limit", 1, "test") && budget->exhausted);
    }
    GTA4_FontSelectionTraceResetSession();
    for (auto* budget : {&texts, &glyphs, &bindings, &selections}) {
      assert(!budget->exhausted && budget->Take("same-address", 1, "test"));
    }
    std::puts("PASS font trace: all budgets rearm for same-address next session");
  }
  std::puts("F02 TEXT LIFETIME: PASS (production bodies, mocked guest allocator/dispatcher; no renderer proof)");
}
'''
    with tempfile.TemporaryDirectory(prefix="f02-text-lifetime-") as folder:
        cpp = pathlib.Path(folder) / "test.cpp"
        binary = pathlib.Path(folder) / "test"
        cpp.write_text(source)
        command = [args.cxx, "-std=c++20", "-O1", "-g", "-pthread", "-I", str(src), str(cpp), "-o", str(binary)]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
