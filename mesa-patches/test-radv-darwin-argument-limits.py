#!/usr/bin/env python3
"""Compile and run the pure RADV Darwin argument-limit export tests."""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import subprocess


SOURCE_FILES = (
    "include/darwin/radv_darwin_argument_limits.h",
    "src/amd/vulkan/radv_darwin_argument_limits.c",
    "src/amd/vulkan/radv_darwin_argument_limits_core.h",
    "src/amd/vulkan/radv_constants.h",
)
HASH_FILES = SOURCE_FILES + ("src/amd/vulkan/radv_physical_device.c",)

SHARED_SOURCE_REQUIRED = (
    "radv_darwin_max_descriptor_set_size()",
    "radv_darwin_max_allocation_size_from_native_vram(",
    "maxStorageBufferRange = RADV_DARWIN_MAX_STORAGE_BUFFER_RANGE",
    "maxSamplerAllocationCount = RADV_DARWIN_MAX_SAMPLER_ALLOCATIONS",
    "maxUpdateAfterBindDescriptorsInAllPools = RADV_DARWIN_MAX_UAB_DESCRIPTORS_ALL_POOLS",
)


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    print("+", " ".join(command), flush=True)
    result = subprocess.run(
        command,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    if result.stdout:
        print(result.stdout, end="", flush=True)
    if result.returncode:
        raise SystemExit(f"command failed ({result.returncode}): {' '.join(command)}")
    return result


def check_shared_source(mesa: Path) -> None:
    text = (mesa / "src/amd/vulkan/radv_physical_device.c").read_text(
        encoding="utf-8"
    )
    missing = [needle for needle in SHARED_SOURCE_REQUIRED if needle not in text]
    if "radv_max_descriptor_set_size()" in text:
        missing.append("removed radv_max_descriptor_set_size()")
    if missing:
        raise SystemExit(
            "radv_physical_device.c does not use the shared Darwin limits: "
            + ", ".join(missing)
        )
    print("SOURCE_SHARED_HELPERS_PASS physical_device.c", flush=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("mesa_source", type=Path)
    parser.add_argument("output_dir", nargs="?", type=Path, default=Path("argument-limits-test"))
    args = parser.parse_args()

    mesa = args.mesa_source.resolve()
    out = args.output_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    stub = out / "include-stub"
    stub.mkdir(exist_ok=True)
    (stub / "git_sha1.h").write_text(
        '#define MESA_GIT_SHA1 " (git-test000000)"\n', encoding="ascii"
    )

    for name in SOURCE_FILES:
        path = mesa / name
        if not path.is_file():
            raise SystemExit(f"missing patched Mesa file: {path}")
    check_shared_source(mesa)

    repo = Path(__file__).resolve().parent
    test_source = repo / "radv_darwin_argument_limits_test.c"
    production_source = mesa / "src/amd/vulkan/radv_darwin_argument_limits.c"
    common = [
        os.environ.get("CC", "cc"),
        "-std=c11",
        "-g",
        "-Wall",
        "-Wextra",
        "-Werror",
        '-DPACKAGE_VERSION="26.3.0-devel"',
        "-I",
        str(stub),
        "-I",
        str(mesa / "include"),
        "-I",
        str(mesa / "include/darwin"),
        "-I",
        str(mesa / "src/amd/vulkan"),
        str(test_source),
        str(production_source),
    ]

    builds = (
        ("optimized", ["-O2"]),
        ("sanitize", ["-O1", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]),
    )
    logs: list[str] = []
    for name, flags in builds:
        binary = out / f"test-radv-darwin-argument-limits-{name}"
        compile_result = run(common[:1] + flags + common[1:] + ["-o", str(binary)])
        run_result = run([str(binary)])
        logs.append(
            f"=== {name} compile ===\n{compile_result.stdout}"
            f"=== {name} run ===\n{run_result.stdout}"
        )

    hashes = []
    for name in HASH_FILES:
        data = (mesa / name).read_bytes()
        hashes.append(f"{hashlib.sha256(data).hexdigest()}  {name}")
    summary = "\n".join(logs) + "\n=== source hashes ===\n" + "\n".join(hashes) + "\n"
    (out / "result.log").write_text(summary, encoding="utf-8")
    print(summary, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
