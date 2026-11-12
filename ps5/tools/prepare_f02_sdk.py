#!/usr/bin/env python3
import argparse
import hashlib
import io
import json
import os
import pathlib
import shutil
import subprocess
import tarfile

PIN = "95c08f27386fc698f6bbe21dde3030140a41d10b"
ROOT = pathlib.Path(__file__).resolve().parents[2]
PATCH = ROOT / "glue/rexglue-sdk-main/patches/ps5platform-f02-lifecycle.patch"
FILES = (
    "_f02_source/platform/src/shm.c",
    "_f02_source/platform/include/ps5platform/shm.h",
    "target/include/ps5platform/shm.h",
    "target/lib/libps5platform.a",
)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def verify(sdk):
    receipt = json.loads((sdk / "f02-sdk-receipt.json").read_text())
    if receipt["pin"] != PIN or receipt["patch_sha256"] != digest(PATCH):
        raise ValueError("SDK pin or lifecycle patch differs from this contribution")
    for name in FILES:
        if digest(sdk / name) != receipt["sha256"][name]:
            raise ValueError(f"SDK content differs from its receipt: {name}")
    if digest(sdk / FILES[1]) != digest(sdk / FILES[2]):
        raise ValueError("Installed SDK header differs from the rebuilt source")
    print("F02 SDK receipt verified: " + PIN)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--verify", type=pathlib.Path)
    parser.add_argument("--source", type=pathlib.Path)
    parser.add_argument("--base-sdk", type=pathlib.Path)
    parser.add_argument("--output", type=pathlib.Path)
    parser.add_argument("--cc", default="clang-19")
    parser.add_argument("--ar", default="llvm-ar-19")
    parser.add_argument("--nm", default="llvm-nm-19")
    parser.add_argument("--llvm-config", default="llvm-config-19")
    args = parser.parse_args()
    if args.verify:
        verify(args.verify.resolve())
        return
    if not all((args.source, args.base_sdk, args.output)):
        parser.error("--source, --base-sdk and --output are required for preparation")
    source, base, output = (p.resolve() for p in (args.source, args.base_sdk, args.output))
    if output.exists() or not output.parent.is_dir():
        raise ValueError("Output must be a fresh directory beneath an existing parent")
    if subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip() != PIN:
        raise ValueError("SDK source is not at the pinned revision")
    if subprocess.check_output(["git", "-C", str(source), "status", "--porcelain"], text=True):
        raise ValueError("SDK source must be pristine")
    if not (base / "toolchain/prospero.cmake").is_file():
        raise ValueError("Base SDK is missing the supported Prospero toolchain")
    tools = {}
    for name in ("cc", "ar", "nm", "llvm_config"):
        executable = shutil.which(getattr(args, name))
        if not executable:
            raise ValueError(f"Missing tool: {getattr(args, name)}")
        tools[name] = executable
    archive = subprocess.check_output(["git", "-C", str(source), "archive", PIN])
    output.mkdir()
    for name in ("bin", "ldscripts", "toolchain", "target"):
        shutil.copytree(base / name, output / name, symlinks=True)
    tree = output / "_f02_source"
    tree.mkdir()
    with tarfile.open(fileobj=io.BytesIO(archive)) as tar:
        tar.extractall(tree, filter="data")
    patch_env = dict(os.environ, GIT_CEILING_DIRECTORIES=str(output))
    subprocess.run(["git", "apply", "--check", str(PATCH)], cwd=tree, env=patch_env, check=True)
    subprocess.run(["git", "apply", str(PATCH)], cwd=tree, env=patch_env, check=True)
    subprocess.run([
        "make", "-C", str(tree / "platform"), "-j2", "install",
        "DESTDIR=" + str(output), "CC=" + tools["cc"], "AR=" + tools["ar"],
        "LLVM_CONFIG=" + tools["llvm_config"],
    ], check=True)
    symbols = subprocess.check_output([
        tools["nm"], "--defined-only", str(output / "target/lib/libps5platform.a")
    ], text=True)
    for symbol in ("ps5_shm_destroy_reported", "ps5_shm_unmap_reported"):
        if not any(line.split()[-1:] == [symbol] for line in symbols.splitlines()):
            raise ValueError("Rebuilt SDK archive lacks " + symbol)
    receipt = {
        "pin": PIN,
        "patch_sha256": digest(PATCH),
        "compiler": subprocess.check_output([tools["cc"], "--version"], text=True),
        "sha256": {name: digest(output / name) for name in FILES},
    }
    (output / "f02-sdk-receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
    verify(output)


if __name__ == "__main__":
    main()
