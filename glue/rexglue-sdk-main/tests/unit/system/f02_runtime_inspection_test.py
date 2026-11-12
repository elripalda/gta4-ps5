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
    diagnostic = (root / "src/core/ps5_diag.cpp").read_text()
    monitor = (root / "src/ui/windowed_app_main_sdl.cpp").read_text()
    runtime = (root / "src/system/runtime.cpp").read_text()
    loop = monitor[monitor.index("for (WaitSlot& w : wait_slots)"):monitor.index("SampleAllThreads();", monitor.index("for (WaitSlot& w : wait_slots)"))]
    assert loop.index("RuntimeInspection inspection") < loop.index("kernel_state()")
    assert "if (!inspection || !stuck_ticks) break;" in loop
    assert loop.index("RuntimeInspection inspection") < loop.index("w.critical_section)")
    shutdown = function(runtime, "void Runtime::Shutdown()")
    cooperative = function(runtime, "system::ShutdownReport Runtime::ShutdownCooperatively(")
    assert shutdown.index("} sampling_pause;") < shutdown.index("kernel_state_.reset()")
    assert cooperative.index("} sampling_pause;") < cooperative.index("report = system::RunCooperativeShutdown(")
    handler = function(diagnostic, "void SampleHandler(")
    assert "stack_sample_gate.Enter()" in handler and handler.count("stack_sample_gate.Leave()") == 2
    assert "stack_top" not in diagnostic
    source = r'''
#include <rex/ps5_diag.h>
#include <cassert>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>
#include <sys/mman.h>
namespace rex::ps5diag {
StackSampleGate stack_sample_gate;
std::mutex sample_pass_mutex;
unsigned sample_pause_depth = 0;
'''
    source += function(diagnostic, "void SuspendStackSampling()") + "\n"
    source += function(diagnostic, "void ResumeStackSampling()") + "\n}\n"
    source += r'''
using namespace rex::ps5diag;
int main() {
  void* pages = mmap(nullptr, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  assert(pages != MAP_FAILED);
  assert(mprotect(static_cast<char*>(pages) + 4096, 4096, PROT_NONE) == 0);
  auto* last = reinterpret_cast<uintptr_t*>(static_cast<char*>(pages) + 4096 - 8);
  *last = 123;
  const uintptr_t rsp = reinterpret_cast<uintptr_t>(last);
  assert(StackScanEnd(rsp, 65536) == rsp + 8);
  for (uintptr_t p = rsp; p + 8 <= StackScanEnd(rsp, 65536); p += 8)
    assert(*reinterpret_cast<const uintptr_t*>(p) == 123);
  std::atomic<bool> entered{false}, release{false}, suspended{false};
  std::thread reader([&] {
    RuntimeInspection inspection;
    assert(inspection);
    entered.store(true);
    while (!release.load()) std::this_thread::yield();
    assert(*last == 123);
  });
  while (!entered.load()) std::this_thread::yield();
  std::thread drain([&] { SuspendStackSampling(); suspended.store(true); });
  while (!stack_sample_gate.suspended.load()) std::this_thread::yield();
  assert(!suspended.load());
  { RuntimeInspection inspection; assert(!inspection); }
  release.store(true);
  reader.join(); drain.join();
  assert(suspended.load() && stack_sample_gate.readers.load() == 0);
  assert(mprotect(pages, 4096, PROT_NONE) == 0);
  { RuntimeInspection inspection; assert(!inspection); }
  assert(mprotect(pages, 4096, PROT_READ | PROT_WRITE) == 0);
  SuspendStackSampling();
  ResumeStackSampling();
  { RuntimeInspection inspection; assert(!inspection); }
  ResumeStackSampling();
  { RuntimeInspection inspection; assert(inspection); }
  std::atomic<bool> stop{false};
  std::vector<std::thread> readers;
  for (int i = 0; i < 4; ++i) readers.emplace_back([&] {
    while (!stop.load()) {
      RuntimeInspection inspection;
      if (inspection) assert(*last == 123);
    }
  });
  for (int cycle = 0; cycle < 200; ++cycle) {
    SuspendStackSampling();
    assert(mprotect(pages, 4096, PROT_NONE) == 0);
    { RuntimeInspection inspection; assert(!inspection); }
    assert(mprotect(pages, 4096, PROT_READ | PROT_WRITE) == 0);
    ResumeStackSampling();
  }
  stop.store(true);
  for (auto& thread : readers) thread.join();
  assert(stack_sample_gate.readers.load() == 0);
  assert(munmap(pages, 8192) == 0);
  std::puts("runtime inspection: production gate, guard page, 200 drains and monitor wiring passed");
}
'''
    with tempfile.TemporaryDirectory(prefix="f02-runtime-inspection-") as directory:
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
