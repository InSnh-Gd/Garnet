#!/usr/bin/env python3
"""Run selected production limit/binding functions against boundary stubs."""
import importlib.util
import os
from pathlib import Path
import shlex
import subprocess
import sys

if len(sys.argv) != 3:
    raise SystemExit("usage: test-large-buffer-api.py PATCHED_MESA OUTPUT_DIR")
here = Path(__file__).resolve().parent
module = importlib.util.spec_from_file_location("composite_test", here / "test-composite-bo.py")
extract = importlib.util.module_from_spec(module)
module.loader.exec_module(extract)
mesa, out = map(Path, sys.argv[1:])
out.mkdir(parents=True, exist_ok=True)
cc = shlex.split(os.environ.get("CC", "clang"))
functions = {
    "src/amd/vulkan/radv_physical_device.c": ["radv_get_max_allocation_size"],
    "src/amd/vulkan/radv_cmd_buffer.c": ["radv_get_vbo_info", "radv_CmdBindIndexBuffer3KHR"],
    "src/amd/vulkan/winsys/amdgpu/radv_darwin_scanout.c": ["buffer_handle"],
}
parts = []
for source, names in functions.items():
    text = extract.preprocess(cc, mesa / source, "-DHAVE_AMDGPU_DARWIN=1")
    for name in names:
        parts.append(extract.extract_function(text, name)[2])
(out / "large_api_actual.inc").write_text("\n\n".join(parts))
exe = out / "large-buffer-api-test"
subprocess.run(cc + ["-std=c11", "-Wall", "-Wextra", "-Werror", "-O1", "-g",
    "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-I", str(out),
    str(here / "large_buffer_api_test.c"), "-o", str(exe)], check=True)
subprocess.run([str(exe.resolve())], check=True)
