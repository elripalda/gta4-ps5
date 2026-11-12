#!/usr/bin/env python3
import argparse
import pathlib
import re
import subprocess
import tempfile


def function(source, signature):
    start = source.index(signature)
    body = source.index("{", start)
    depth = 1
    end = body + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=pathlib.Path, default=pathlib.Path(__file__).resolve().parents[3])
    parser.add_argument("--cxx", default="clang++")
    parser.add_argument("--simde", type=pathlib.Path)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    root = args.root.resolve()
    hooks = (root / "gta4-recomp/src/gta4_frontend_hooks.cpp").read_text()
    generated = (root / "gta4-recomp/generated/gta4_recomp.0.cpp").read_text()
    retail = function(generated, "DEFINE_REX_FUNC(sub_8214B640)")
    pause = function(hooks, 'extern "C" void sub_8214C8C8(')
    reset = function(hooks, "void ResetReturnConfirmation(")
    callees = sorted(set(re.findall(r"\b(sub_[0-9A-F]+)\(ctx, base\)", retail)))
    prefix = r'''
#include <bit>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <unordered_map>
#include <rex/ppc/context.h>
#include "ps5/lifecycle/f02_return_prompt.h"
#undef REX_FUNC_PROLOGUE
#define REX_FUNC_PROLOGUE()
#define DEFINE_REX_FUNC(name) void name(PPCContext& ctx, uint8_t* base)
#undef __builtin_rotateleft64
#define __builtin_rotateleft64(x, y) std::rotl<uint64_t>(x, y)
#define REXLOG_INFO(...) ((void)0)
std::unordered_map<uint32_t, uint64_t> guest;
#define REX_LOAD_U8(a) uint8_t(guest[uint32_t(a)])
#define REX_LOAD_U16(a) uint16_t(guest[uint32_t(a)])
#define REX_LOAD_U32(a) uint32_t(guest[uint32_t(a)])
#define REX_LOAD_U64(a) uint64_t(guest[uint32_t(a)])
#define REX_STORE_U8(a, v) (guest[uint32_t(a)] = uint8_t(v))
#define REX_STORE_U16(a, v) (guest[uint32_t(a)] = uint16_t(v))
#define REX_STORE_U32(a, v) (guest[uint32_t(a)] = uint32_t(v))
#define REX_STORE_U64(a, v) (guest[uint32_t(a)] = uint64_t(v))
using namespace gta4::ps5::lifecycle::f02;
namespace f02_game_menu { constexpr uint32_t kGameScreen = 9; }
constexpr uint32_t kStoredPauseStateAddress = 0x82BFA13C;
constexpr uint32_t kPauseModeAddress = 0x82A97CF4;
constexpr uint32_t kCurrentScreenAddress = 0x82BFA124;
constexpr uint32_t kPauseModeInGameMenu = 1, kPauseStateConfirm = 18;
ReturnConfirmation g_return_confirmation;
uint32_t g_return_confirmation_pause_state = 0;
bool g_return_confirmation_dialog_open = false;
uint64_t g_return_confirmation_sequence = 1;
uint16_t buttons = 0;
int requests = 0, retail_calls = 0, retail_action = 0, retail_answer = 0;
uint32_t dialog_key = 0;
bool reset_requested = false;
namespace gta4::ps5::lifecycle::f02 {
bool ConsumeConfirmationReset() { return std::exchange(reset_requested, false); }
void RequestReturnFromMenu() { ++requests; }
}
uint16_t ReadUiPadButtons() { return buttons; }
void OpenReturnDialog(PPCContext&, uint8_t*) {
  dialog_key = 0xF02; g_return_confirmation_dialog_open = true;
}
void CloseReturnDialog(PPCContext&, uint8_t*) {
  dialog_key = 0; g_return_confirmation_dialog_open = false;
}
'''
    stubs = []
    for name in callees:
        action = "ctx.r3.u64 = 0;"
        if name == "sub_82254FE0":
            action = "dialog_key = ctx.r4.u32;"
        elif name == "sub_8224FFC8":
            action = "ctx.r3.u64 = (ctx.r3.u32 == uint32_t(retail_answer));"
        elif name == "sub_8224EFE8":
            action = "retail_action = ctx.r3.u32; guest[kStoredPauseStateAddress] = retail_action == 9 ? 0 : retail_action;"
        stubs.append(f"void {name}(PPCContext& ctx, uint8_t*) {{ {action} }}")
    adapter = r'''
void __imp__sub_8214C8C8(PPCContext& ctx, uint8_t* base) {
  ++retail_calls;
  sub_8214B640(ctx, base);
}
'''
    tests = r'''
#define CHECK(x) do { if (!(x)) throw std::runtime_error(#x); } while (false)
PPCContext ctx{};
void frame(uint16_t input) { buttons = input; sub_8214C8C8(ctx, nullptr); }
void begin() {
  guest.clear(); ctx = {}; ctx.r1.u32 = 0x100000;
  guest[kPauseModeAddress] = 1; guest[kCurrentScreenAddress] = 9;
  requests = retail_calls = retail_action = retail_answer = 0;
  dialog_key = 0; reset_requested = false;
  g_return_confirmation.Reset(); g_return_confirmation_pause_state = 0;
  CHECK(g_return_confirmation.Begin(kConfirmButton));
  guest[kStoredPauseStateAddress] = 18;
  frame(kConfirmButton); CHECK(requests == 0); frame(0);
}
void cancel() {
  begin(); frame(kCancelButton);
  CHECK(guest[kStoredPauseStateAddress] == 0);
  frame(0);
  frame(0);
  std::printf("cancel: restored=%llu key=%08x action=%d\n",
              static_cast<unsigned long long>(guest[kStoredPauseStateAddress]), dialog_key, retail_action);
  CHECK(guest[kStoredPauseStateAddress] == 0);
  CHECK(dialog_key == 0); CHECK(retail_action == 0); CHECK(requests == 0);
  CHECK(retail_calls == 1);
}
void held_cancel() {
  begin(); frame(kCancelButton);
  for (int i = 0; i < 1200; ++i) frame(kCancelButton | kConfirmButton);
  CHECK(g_return_confirmation.active()); CHECK(retail_calls == 0);
  CHECK(guest[kStoredPauseStateAddress] == 0); CHECK(dialog_key == 0);
  frame(0); frame(0); CHECK(retail_calls == 1); CHECK(requests == 0);
}
void confirm_once() {
  begin(); frame(kConfirmButton);
  for (int i = 0; i < 20; ++i) { frame(0); frame(kConfirmButton); frame(kCancelButton); }
  CHECK(requests == 1); CHECK(retail_calls == 0); CHECK(retail_action == 0);
}
void cancel_then_new_game() {
  begin(); frame(kCancelButton); frame(0); frame(0);
  CHECK(retail_calls == 1); CHECK(requests == 0);
  guest[kStoredPauseStateAddress] = 18; retail_answer = 8; retail_calls = 0;
  frame(0);
  CHECK(retail_calls == 1); CHECK(dialog_key == 0x820B9FC0); CHECK(retail_action == 21);
  CHECK(requests == 0);
}
void retail_new_game() {
  for (int answer : {0, 8, 11}) {
    begin(); g_return_confirmation.Reset(); retail_calls = 0;
    guest[kStoredPauseStateAddress] = 18; retail_answer = answer;
    frame(0);
    CHECK(retail_calls == 1); CHECK(dialog_key == 0x820B9FC0);
    CHECK(retail_action == (answer == 8 ? 21 : answer == 11 ? 9 : 0));
    CHECK(requests == 0);
  }
}
void lost_pause_and_reset() {
  begin(); guest[kCurrentScreenAddress] = 8; frame(0);
  CHECK(!g_return_confirmation.active()); CHECK(retail_calls == 1);
  CHECK(guest[kStoredPauseStateAddress] == 0);
  begin(); reset_requested = true; frame(0);
  CHECK(!g_return_confirmation.active()); CHECK(retail_calls == 1);
  CHECK(guest[kStoredPauseStateAddress] == 0);
}
int main() {
  int failures = 0;
  for (auto test : {cancel, held_cancel, confirm_once, cancel_then_new_game, retail_new_game,
                    lost_pause_and_reset}) {
    try { test(); } catch (const std::exception& e) { ++failures; std::printf("FAIL: %s\n", e.what()); }
  }
  std::printf("native hook + generated retail state machine: %d failures\n", failures);
  return failures ? 1 : 0;
}
'''
    source = "#include <utility>\n" + prefix + "\n".join(stubs) + retail + adapter + reset + pause + tests
    with tempfile.TemporaryDirectory(prefix="f02-native-confirm-") as directory:
        path = pathlib.Path(directory)
        (path / "test.cpp").write_text(source)
        command = [args.cxx, "-std=c++23", "-O1", "-g", "-Wno-psabi",
                   "-I", str(root / "include"), "-I", str(args.simde or root / "thirdparty/simde"),
                   "-I", str(root / "gta4-recomp/src"), str(path / "test.cpp"), "-o", str(path / "test")]
        if args.sanitize:
            command.extend(["-fsanitize=address,undefined", "-fno-omit-frame-pointer"])
        subprocess.run(command, check=True)
        subprocess.run([str(path / "test")], check=True)


if __name__ == "__main__":
    main()
