#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Host test driver: extracts REAL production composite functions from the
# patched Mesa tree and runs them under ASan/UBSan with a stub BAR/VA layer.
# Usage: python3 mesa-patches/test-composite-bo.py PATCHED_MESA OUTPUT_DIR
"""Extract-then-run host test for radv composite BO (Darwin contiguous VA).

PATCHED_MESA is the Mesa tree carrying 0001..0016 plus the composite change.
OUTPUT_DIR receives all generated/compiled outputs (composite_production.inc
and the test executable). The test result goes to stdout; the caller redirects
it to a log file. No log/scratch paths are hardcoded here.

Conditionals are resolved with the actual compiler preprocessor
(CC -E -P -x c -DHAVE_AMDGPU_DARWIN=1), matching the host-test build, so
brace matching sees exactly the active configuration. #include lines are
stripped before preprocessing (headers are stubbed by the test); unknown
external macros stay raw for the test's stubs, local #defines expand, and
compiler predefs are correct. Only "function found" is validated here;
interface drift is caught by the -Werror compile below.
"""
import os
import re
import shlex
import subprocess
import sys
from pathlib import Path

BO_TARGETS = [
    "radv_amdgpu_bo_va_size",
    "radv_amdgpu_bo_va_op",
    "radv_amdgpu_composite_bo_create",
    "radv_amdgpu_winsys_bo_create_internal",
    "radv_amdgpu_winsys_bo_destroy",
    "radv_amdgpu_winsys_bo_create",
]
# Small pure helpers used by the metadata path; extracted verbatim so the
# metadata tests run production code, not a re-implementation.
BO_HELPERS = [
    "eg_tile_split",
    "radv_eg_tile_split_rev",
]
# Composite-relevant entry points exercised behaviorally (fanout / rejects).
BO_EXTRA = [
    "radv_amdgpu_winsys_bo_map",
    "radv_amdgpu_winsys_bo_unmap",
    "radv_amdgpu_winsys_get_fd",
    "radv_amdgpu_winsys_bo_set_metadata",
    "radv_amdgpu_winsys_bo_get_metadata",
]
CS_TARGETS = [
    "radv_amdgpu_cs_find_buffer",
    "radv_amdgpu_cs_add_buffer_internal",
    "radv_amdgpu_cs_add_buffer",
]


def preprocess(cc, path, extra_define):
    """Run the real preprocessor over a source with #includes stripped."""
    text = path.read_text()
    stripped = "\n".join(
        line for line in text.split("\n")
        if not re.match(r"\s*#\s*include\b", line)
    )
    cmd = cc + ["-E", "-P", "-x", "c", extra_define, "-"]
    try:
        proc = subprocess.run(cmd, input=stripped, capture_output=True,
                              text=True)
    except OSError as e:
        print(f"cannot run preprocessor {' '.join(cmd)}: {e}")
        sys.exit(3)
    if proc.returncode != 0:
        print(f"preprocessing {path} failed:\n{proc.stderr}")
        sys.exit(3)
    return proc.stdout


def find_balanced(text, open_idx):
    """Return index one past the brace matching text[open_idx]=='{', skipping
    strings, char literals, line/block comments and preprocessor lines."""
    assert text[open_idx] == "{"
    i = open_idx
    depth = 0
    n = len(text)
    state = "code"
    while i < n:
        if state == "code":
            if text.startswith("//", i):
                state = "line"
                i += 2
                continue
            if text.startswith("/*", i):
                state = "block"
                i += 2
                continue
            c = text[i]
            if c == '"':
                state = "str"
            elif c == "'":
                state = "chr"
            elif c == "{":
                depth += 1
            elif c == "}":
                depth -= 1
                if depth == 0:
                    return i + 1
            i += 1
        elif state == "line":
            if text[i] == "\n":
                state = "code"
            i += 1
        elif state == "block":
            if text.startswith("*/", i):
                state = "code"
                i += 2
            else:
                i += 1
        elif state == "str":
            if text[i] == "\\":
                i += 2
                continue
            if text[i] == '"':
                state = "code"
            i += 1
        elif state == "chr":
            if text[i] == "\\":
                i += 2
                continue
            if text[i] == "'":
                state = "code"
            i += 1
    raise ValueError("unbalanced braces")


def extract_function(text, name):
    """Extract the full definition `... name(...) { ... }` with signature."""
    pat = re.compile(r"(?:^|\n)[^\n{};]*\b" + re.escape(name) + r"\s*\(", re.MULTILINE)
    for m in pat.finditer(text):
        paren = m.group(0).rfind("(")
        pidx = m.start(0) + paren
        depth = 0
        i = pidx
        in_str = in_chr = False
        esc = False
        sig_end = -1
        while i < len(text):
            c = text[i]
            if esc:
                esc = False
            elif in_str:
                if c == "\\":
                    esc = True
                elif c == '"':
                    in_str = False
            elif in_chr:
                if c == "\\":
                    esc = True
                elif c == "'":
                    in_chr = False
            else:
                if c == '"':
                    in_str = True
                elif c == "'":
                    in_chr = True
                elif c == "(":
                    depth += 1
                elif c == ")":
                    depth -= 1
                    if depth == 0:
                        sig_end = i
                        break
                elif c == ";" and depth == 0:
                    break
            i += 1
        if sig_end < 0:
            continue
        j = sig_end + 1
        while j < len(text) and text[j] in " \t\n\r":
            j += 1
        while text.startswith("__attribute__", j):
            k = text.find("(", j)
            if k < 0:
                break
            d = 1
            k += 1
            while k < len(text) and d:
                if text[k] == "(":
                    d += 1
                elif text[k] == ")":
                    d -= 1
                k += 1
            j = k
            while j < len(text) and text[j] in " \t\n\r":
                j += 1
        if j >= len(text) or text[j] != "{":
            continue  # forward declaration, not a definition
        line_start = text.rfind("\n", 0, m.start(0)) + 1
        body_end = find_balanced(text, j)
        return line_start, body_end, text[line_start:body_end]
    raise ValueError(f"definition not found: {name}")


def main():
    if len(sys.argv) != 3:
        print("usage: python3 mesa-patches/test-composite-bo.py PATCHED_MESA OUTPUT_DIR")
        return 2
    mesa = Path(sys.argv[1]).resolve()
    out = Path(sys.argv[2]).resolve()
    out.mkdir(parents=True, exist_ok=True)

    bo_c = mesa / "src/amd/vulkan/winsys/amdgpu/radv_amdgpu_bo.c"
    cs_c = mesa / "src/amd/vulkan/winsys/amdgpu/radv_amdgpu_cs.c"
    for p in (bo_c, cs_c):
        if not p.is_file():
            print(f"missing production source: {p}")
            return 2

    cc = shlex.split(os.environ.get("CC", "clang"))
    define = "-DHAVE_AMDGPU_DARWIN=1"
    bo_text = preprocess(cc, bo_c, define)
    cs_text = preprocess(cc, cs_c, define)

    parts = []
    parts.append("/* Auto-generated by test-composite-bo.py: verbatim production bodies")
    parts.append(" * (preprocessor conditionals resolved by the real CC -E with")
    parts.append(" * -DHAVE_AMDGPU_DARWIN=1, matching the host-test build). */")
    parts.append(f"/* BO source: {bo_c} ({len(bo_text)} bytes preprocessed) */")
    parts.append(f"/* CS source: {cs_c} ({len(cs_text)} bytes preprocessed) */")
    order = [(bo_text, str(bo_c), n) for n in BO_HELPERS]
    order += [(bo_text, str(bo_c), n) for n in BO_TARGETS]
    order += [(bo_text, str(bo_c), n) for n in BO_EXTRA]
    order += [(cs_text, str(cs_c), n) for n in CS_TARGETS]
    for text, src, name in order:
        try:
            start, end, body = extract_function(text, name)
        except ValueError as e:
            print(f"extraction failed: {e}; source may have changed, refusing stale test")
            return 3
        lineno = text.count("\n", 0, start) + 1
        parts.append(f"\n/* ---- {name} from {src} (preprocessed line {lineno}, {len(body)} bytes) ---- */")
        parts.append(body)
    inc = out / "composite_production.inc"
    inc.write_text("\n".join(parts) + "\n")
    print(f"extracted {len(order)} production functions -> {inc}")

    test_c = Path(__file__).resolve().parent / "composite_bo_test.c"
    exe = out / "composite-bo-test"
    cmd = cc + ["-std=gnu11", "-g", "-O1", "-fsanitize=address,undefined",
                "-fno-omit-frame-pointer", define,
                "-I", str(out), "-Wall", "-Wextra", "-Werror",
                "-Wno-unused-parameter", "-Wno-unused-variable",
                "-Wno-unused-but-set-variable",
                str(test_c), "-o", str(exe)]
    print("+ " + " ".join(cmd), flush=True)
    subprocess.run(cmd, check=True)
    env = dict(os.environ)
    # Sanitizer failures must fail the test run, never pass silently.
    env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
    if sys.platform == "darwin":
        env["ASAN_OPTIONS"] = "detect_leaks=0:halt_on_error=1"
    else:
        env["ASAN_OPTIONS"] = "detect_leaks=1:halt_on_error=1"
    print(f"+ {exe}", flush=True)
    proc = subprocess.run([str(exe)], env=env)
    if proc.returncode != 0:
        print(f"composite BO host test FAILED (exit {proc.returncode})")
        return 1
    print("composite BO host test PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
