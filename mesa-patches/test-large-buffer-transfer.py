#!/usr/bin/env python3
import importlib.util
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys

if len(sys.argv) != 3:
    raise SystemExit("usage: test-large-buffer-transfer.py PATCHED_MESA OUTPUT_DIR")

here = Path(__file__).resolve().parent
module = importlib.util.spec_from_file_location("composite_test", here / "test-composite-bo.py")
extract = importlib.util.module_from_spec(module)
module.loader.exec_module(extract)

mesa, out = map(Path, sys.argv[1:])
out.mkdir(parents=True, exist_ok=True)
cc = shlex.split(os.environ.get("CC", "clang"))

source = mesa / "src/amd/vulkan/meta/radv_meta_buffer.c"
text = extract.preprocess(cc, source, "-DHAVE_AMDGPU_DARWIN=1")
limits = re.findall(r"^#define RADV_DARWIN_(?:TRANSFER|BYTE_COPY)_CHUNK_SIZE\s+.+$",
                    source.read_text(), re.MULTILINE)
if len(limits) != 2:
    raise SystemExit("missing production transfer limits")
names = [
    "radv_is_copy_memory_4B_aligned",
    "radv_compute_fill_memory",
    "radv_compute_copy_memory",
    "radv_prefer_compute_or_cp_dma",
    "radv_is_compute_required",
    "radv_fill_memory_chunk",
    "radv_fill_memory_internal",
    "radv_fill_memory",
    "radv_copy_memory_chunk",
    "radv_copy_memory",
]

parts = limits
for name in names:
    parts.append(extract.extract_function(text, name)[2])

(out / "large_buffer_transfer_actual.inc").write_text("\n\n".join(parts) + "\n")

exe = out / "large-buffer-transfer-test"
cmd = cc + [
    "-std=c11",
    "-Wall",
    "-Wextra",
    "-Werror",
    "-O1",
    "-g",
    "-fsanitize=address,undefined",
    "-fno-sanitize-recover=all",
    "-fno-omit-frame-pointer",
    "-I",
    str(out),
    str(here / "large_buffer_transfer_test.c"),
    "-o",
    str(exe),
]
subprocess.run(cmd, check=True)

env = dict(os.environ)
env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
env["ASAN_OPTIONS"] = "halt_on_error=1:detect_leaks=0" if sys.platform == "darwin" else "halt_on_error=1:detect_leaks=1"
subprocess.run([str(exe.resolve())], check=True, env=env)
