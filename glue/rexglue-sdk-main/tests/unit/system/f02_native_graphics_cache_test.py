#!/usr/bin/env python3
"""Regression test for the per-session native graphics-system cache.

Returning to the hub destroys the graphics system and installs a new one for
the next title in the same process. The native command path caches the
graphics-system pointer to avoid a virtual call per draw, so the cache must be
invalidated when that system is replaced. This test compiles the production
GetNativeGraphicsSystem() against a fake runtime that models the generation
stamp and proves the cache cannot hand out a pointer from an older session.
"""
import argparse
import pathlib
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
    parser.add_argument("--root", type=pathlib.Path,
                        default=pathlib.Path(__file__).resolve().parents[3])
    parser.add_argument("--cxx", default="clang++")
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    root = args.root.resolve()

    hooks = (root / "gta4-recomp/src/gta4_native_hooks.cpp").read_text()
    runtime_source = (root / "src/system/runtime.cpp").read_text()
    runtime_header = (root / "include/rex/runtime.h").read_text()
    cache = function(hooks, "rex::system::IGraphicsSystem* GetNativeGraphicsSystem()")
    setup = function(runtime_source, "X_STATUS Runtime::Setup(RuntimeConfig config)")
    shutdown = function(runtime_source, "void Runtime::Shutdown()")

    failures = 0

    def check(condition, message):
        nonlocal failures
        if condition:
            print(f"PASS {message}")
        else:
            print(f"FAIL {message}")
            failures += 1

    check("uint64_t graphics_generation()" in runtime_header,
          "runtime declares the graphics generation stamp")
    check("g_graphics_generation.load(std::memory_order_acquire)" in runtime_source,
          "runtime exposes the current generation")
    installed = "graphics_system_ = std::move(config.graphics);"
    destroyed = "graphics_system_.reset();"
    bump = "g_graphics_generation.fetch_add(1, std::memory_order_release);"
    check(bump in setup and setup.index(installed) < setup.index(bump),
          "installing a graphics system advances the generation")
    check(bump in shutdown and shutdown.index(destroyed) < shutdown.index(bump),
          "destroying a graphics system advances the generation")
    check("rex::Runtime::graphics_generation()" in cache,
          "native hooks key the cache to the generation")
    check("cached_generation.load(std::memory_order_acquire)" in cache,
          "native hooks compare the cached generation before reusing a pointer")

    program = r'''
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#define REX_PLATFORM_PS5 1

namespace rex {
namespace system {
struct IGraphicsSystem {
  int abi = 0x21;
  mutable int abi_queries = 0;
  int GetTitleCommandAbi(uint32_t) const { ++abi_queries; return abi; }
};
}  // namespace system
class Runtime {
 public:
  static Runtime* instance_;
  static uint64_t generation_;
  static Runtime* instance() { return instance_; }
  static uint64_t graphics_generation() { return generation_; }
  system::IGraphicsSystem* graphics_system() const { return graphics_; }
  system::IGraphicsSystem* graphics_ = nullptr;
};
Runtime* Runtime::instance_ = nullptr;
uint64_t Runtime::generation_ = 0;
}  // namespace rex

static constexpr uint32_t kTitleId = 0x545407F2;
static constexpr uint32_t kTitleCommandAbi = 0x21;

''' + cache + r'''

static int checks = 0;
static int failures = 0;
static void check(bool condition, const char* what) {
  ++checks;
  if (condition) {
    std::printf("PASS %s\n", what);
  } else {
    std::printf("FAIL %s\n", what);
    ++failures;
  }
}

int main() {
  using rex::Runtime;
  using rex::system::IGraphicsSystem;
  IGraphicsSystem first;
  IGraphicsSystem second;

  check(GetNativeGraphicsSystem() == nullptr, "no runtime resolves to no graphics");

  Runtime runtime;
  Runtime::instance_ = &runtime;
  runtime.graphics_ = &first;
  Runtime::generation_ = 0;
  check(GetNativeGraphicsSystem() == &first, "first session resolves its graphics system");
  check(first.abi_queries == 1, "resolution validates the title command ABI");
  check(GetNativeGraphicsSystem() == &first, "repeat draw reuses the same session's pointer");
  check(first.abi_queries == 1, "cache hit skips the ABI virtual call");

  // The system is destroyed for the hub: the generation changes before the
  // next session installs its own system.
  runtime.graphics_ = nullptr;
  Runtime::generation_ = 1;
  check(GetNativeGraphicsSystem() == nullptr,
        "destroyed system is never returned from the old session cache");

  // The next session installs a new backend.
  runtime.graphics_ = &second;
  check(GetNativeGraphicsSystem() == &second, "next session resolves its own graphics system");
  check(second.abi_queries == 1, "next session validates the new system");

  // Address reuse: even if a new system lands at the previous address, a
  // generation change forces revalidation instead of a stale hit.
  Runtime::generation_ = 2;
  second.abi = 0x99;
  check(GetNativeGraphicsSystem() == nullptr,
        "generation change revalidates instead of trusting the cached address");
  check(second.abi_queries == 2, "revalidation queried the live system");
  second.abi = kTitleCommandAbi;
  check(GetNativeGraphicsSystem() == &second, "matching ABI after revalidation is accepted");
  check(GetNativeGraphicsSystem() == &second, "accepted pointer is cached for the session");
  check(second.abi_queries == 3, "accepted pointer is not revalidated per draw");

  Runtime::instance_ = nullptr;
  check(GetNativeGraphicsSystem() == nullptr, "shutdown runtime never yields a cached pointer");

  if (failures) {
    std::printf("%d/%d checks failed\n", failures, checks);
    return 1;
  }
  std::printf("%d checks passed\n", checks);
  return 0;
}
'''

    with tempfile.TemporaryDirectory() as temp:
        temp = pathlib.Path(temp)
        source = temp / "f02_native_graphics_cache.cpp"
        binary = temp / "f02_native_graphics_cache"
        source.write_text(program)
        command = [args.cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror",
                   str(source), "-o", str(binary)]
        if args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
        build = subprocess.run(command, text=True, capture_output=True)
        if build.returncode != 0:
            print(build.stdout)
            print(build.stderr)
            return 1
        run = subprocess.run([str(binary)], text=True, capture_output=True)
        print(run.stdout, end="")
        if run.returncode != 0:
            print(run.stderr, end="")
            return 1

    if failures:
        print(f"{failures} static checks failed")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
