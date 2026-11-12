#!/usr/bin/env python3
import argparse
import json
import os
import pathlib
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", required=True, type=pathlib.Path)
    parser.add_argument("--sdk-source", required=True, type=pathlib.Path)
    args = parser.parse_args()
    root = pathlib.Path(__file__).resolve().parents[2]
    rex = root / "glue/rexglue-sdk-main"
    output = args.out.resolve()
    output.mkdir(exist_ok=False)
    (output / "bin").mkdir()
    (output / "logs").mkdir()
    sdk = args.sdk_source.resolve()
    results = []
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=1:abort_on_error=1",
               UBSAN_OPTIONS="halt_on_error=1", PYTHONDONTWRITEBYTECODE="1")

    def run(name, command):
        with (output / "logs" / (name + ".log")).open("w") as log:
            log.write("COMMAND: " + repr(command) + "\n")
            log.flush()
            try:
                code = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT,
                                      env=env, timeout=240).returncode
            except subprocess.TimeoutExpired:
                code = "TIMEOUT"
        results.append({"name": name, "command": command, "returncode": code, "pass": code == 0})
        (output / "results.json").write_text(json.dumps(results, indent=2) + "\n")
        print(("PASS " if code == 0 else "FAIL ") + name, flush=True)
        return code == 0

    suites = ["return_to_hub", "cooperative_drain", "save_completion", "save_quiescence",
              "cooperative_shutdown", "return_prompt", "relaunch_policy", "session_restart",
              "game_menu", "episode_startup", "keybind_reregistration", "hsio_watch"]
    scripts = ["native_confirmation", "native_teardown", "native_graphics_cache",
               "text_lifetime", "runtime_inspection"]
    for cxx, cc in (("g++", "gcc"), ("clang++", "clang")):
        for mode in ("plain", "asan-ubsan"):
            flags = ["-O2"] if mode == "plain" else ["-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
            for suite in suites:
                name = "f02_" + suite
                tag = name + "_" + cxx + "_" + mode
                sources = [rex / "tests/unit/system" / (name + "_test.cpp")]
                includes = []
                if suite == "keybind_reregistration":
                    includes = ["-I", str(rex / "tests/unit/system/f02_keybind_stubs")]
                    sources.append(rex / "src/ui/keybinds.cpp")
                elif suite == "hsio_watch":
                    sources.append(rex / "src/kernel/xboxkrnl/vdhsio_watch.cpp")
                binary = output / "bin" / tag
                command = [cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror",
                           "-DGTA4_F02_STANDALONE", "-pthread", *includes,
                           "-I", str(rex / "include"), "-I", str(rex / "gta4-recomp/src"),
                           *flags, *map(str, sources), "-o", str(binary)]
                if run(tag + "_compile", command):
                    run(tag + "_run", [str(binary)])
            for suite in scripts:
                command = ["python3", "-B", str(rex / "tests/unit/system" / ("f02_" + suite + "_test.py")),
                           "--root", str(rex), "--cxx", cxx]
                if suite == "native_confirmation":
                    command.extend(["--simde", str(rex / "thirdparty/simde")])
                if mode != "plain":
                    command.append("--sanitize")
                run("f02_" + suite + "_" + cxx + "_" + mode, command)
            motion = ["python3", "-B", str(rex / "tests/unit/system/gta4_motion_registration_test.py"),
                      "--root", str(rex), "--cxx", cxx, "--simde", str(rex / "thirdparty/simde")]
            if mode != "plain":
                motion.append("--sanitize")
            run("motion_registration_" + cxx + "_" + mode, motion)
            binary = output / "bin" / ("sdk_ownership_" + cc + "_" + mode)
            command = [cc, "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-pthread", *flags,
                       "-I", str(sdk / "platform/include"), "-I", str(sdk / "platform/src"),
                       "-I", str(sdk / "platform/tests"), str(sdk / "platform/src/shm.c"),
                       str(sdk / "platform/src/placement.c"), str(sdk / "platform/tests/host_kernel.c"),
                       str(root / "ps5/tests/f02_sdk_ownership_test.c"),
                       "-Wl,--wrap=sceKernelReleaseDirectMemory", "-Wl,--wrap=sceKernelMunmap",
                       "-Wl,--wrap=sceKernelReserveVirtualRange", "-o", str(binary)]
            tag = "sdk_ownership_" + cc + "_" + mode
            if run(tag + "_compile", command):
                run(tag + "_run", [str(binary)])
    run("watcher_lifetime_audit", ["python3", "-B", str(rex / "tests/unit/system/f02_watcher_lifetime_audit_test.py"), "--root", str(rex)])
    failures = sum(not result["pass"] for result in results)
    print(f"TOTAL {len(results)} gates, {failures} failed", flush=True)
    return bool(failures)


if __name__ == "__main__":
    raise SystemExit(main())
