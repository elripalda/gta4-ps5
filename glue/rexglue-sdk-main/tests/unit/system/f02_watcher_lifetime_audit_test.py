#!/usr/bin/env python3
"""Source audit for the VdHSIOCalibrationLock watcher lifetime.

The unit tests exercise the worker itself. This audit proves the production
wiring around it: XboxkrnlModule owns one watcher, stops and joins it from its
destructor during kernel teardown, and Runtime destroys the kernel before it
releases guest memory.
"""
import argparse
import pathlib
import sys


def body(text, signature):
    start = text.index(signature)
    brace = text.index("{", start)
    depth = 0
    for index in range(brace, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[start:index + 1]
    raise RuntimeError(f"unterminated body for {signature!r}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=pathlib.Path,
                        default=pathlib.Path(__file__).resolve().parents[3])
    args = parser.parse_args()
    root = args.root.resolve()

    failures = 0

    def check(condition, message):
        nonlocal failures
        if condition:
            print(f"PASS {message}")
        else:
            print(f"FAIL {message}")
            failures += 1

    watch_header = (root / "include/rex/kernel/xboxkrnl/vdhsio_watch.h").read_text()
    watch_source = (root / "src/kernel/xboxkrnl/vdhsio_watch.cpp").read_text()
    module_header = (root / "include/rex/kernel/xboxkrnl/module.h").read_text()
    module_source = (root / "src/kernel/xboxkrnl/xboxkrnl_module.cpp").read_text()
    video_source = (root / "src/kernel/xboxkrnl/xboxkrnl_video.cpp").read_text()
    video_header = (root / "include/rex/kernel/xboxkrnl/video.h").read_text()
    runtime_source = (root / "src/system/runtime.cpp").read_text()
    kernel_cmake = (root / "src/kernel/CMakeLists.txt").read_text()

    check("class VdHsioLockWatcher" in watch_header, "watcher class is declared")
    check("std::unique_ptr<VdHsioLockWatcher> hsio_watch_" in module_header,
          "XboxkrnlModule owns the watcher")
    check("hsio_watch_(std::make_unique<VdHsioLockWatcher>())" in module_source,
          "watcher is created with the module")
    check("RegisterVideoExports(export_resolver_, kernel_state_, hsio_watch_.get())"
          in module_source, "module passes its owned watcher to the video exports")
    check("hsio_watch_(std::make_unique" not in video_source,
          "video hardware setup does not create a second watcher")

    destructor = body(module_source, "XboxkrnlModule::~XboxkrnlModule()")
    check("hsio_watch_->Stop()" in destructor, "module destructor stops the watcher")

    stop_body = body(watch_source, "void VdHsioLockWatcher::StopLocked()")
    check("stop_requested_.store" in stop_body, "stop sets the stop request")
    check("worker_.join()" in stop_body, "stop joins the worker")
    check(stop_body.index("stop_requested_.store") < stop_body.index("worker_.join()"),
          "stop requests exit before joining")

    run_body = body(watch_source, "void VdHsioLockWatcher::Run(")
    stop_check = run_body.index("stop_requested_.load")
    access = run_body.index("header[0]")
    check(stop_check < access, "worker checks the stop request before guest access")
    check(".detach()" not in watch_source, "worker is never detached")
    check(".detach()" not in module_source, "module never detaches a reader")
    check(".detach()" not in video_source, "video setup never detaches a reader")

    start_call = video_source.index("hsio_watch->Start(")
    ps5_guard = video_source.rindex("#if REX_PLATFORM_PS5", 0, start_call)
    ps5_end = video_source.index("#endif", start_call)
    check(ps5_guard < start_call < ps5_end, "watcher start stays PS5-only")
    check("VdHsioLockWatcher* hsio_watch" in video_header,
          "video export signature takes the owned watcher")

    shutdown = body(runtime_source, "void Runtime::Shutdown()")
    check(shutdown.index("kernel_state_.reset()") < shutdown.index("memory_.reset()"),
          "runtime destroys the kernel (and joins) before releasing memory")
    check("vdhsio_watch.cpp" in kernel_cmake, "watcher source is part of rexruntime")

    if failures:
        print(f"F02 WATCHER LIFETIME AUDIT: FAIL ({failures})")
        return 1
    print("F02 WATCHER LIFETIME AUDIT: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
