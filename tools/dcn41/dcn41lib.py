#!/usr/bin/env python3
"""dcn41lib.py - shared paths and guarded child runner for the DCN 4.1 display register layer (tools/dcn41/).

Every compiler or test child runs sequentially through run() below with a memory gate and timeout guard.
Build outputs never land in the repo: BUILD defaults to $TMPDIR/dcn41-build (override with DCN41_BUILD).
"""
import os
import pathlib
import re
import shutil
import subprocess
import sys

sys.dont_write_bytecode = True

HERE = pathlib.Path(__file__).resolve().parent
TOOLS = HERE.parent
ROOT = TOOLS.parent
SRC = ROOT / "src/dcn41"
AMD = ROOT / "re/linux-dc/drivers/gpu/drm/amd"          # optional Linux clone, commit 238650ef6c7c
DISPLAY = AMD / "display"
DC = DISPLAY / "dc"
ASIC_DCN = AMD / "include/asic_reg/dcn"
OFFSET_H = ASIC_DCN / "dcn_4_1_0_offset.h"
SHMASK_H = ASIC_DCN / "dcn_4_1_0_sh_mask.h"
BUILD = pathlib.Path(os.environ.get("DCN41_BUILD", pathlib.Path(os.environ.get("TMPDIR", "/tmp")) / "dcn41-build"))
LINUX_COMMIT = "238650ef6c7c"

CLANG = os.environ.get("CC") or shutil.which("clang") or "/usr/bin/clang"


class GuardKilled(RuntimeError):
    pass


def require_memory(what):
    """Ensure sufficient free memory before launching a compiler or test child."""
    meminfo = pathlib.Path("/proc/meminfo")
    if meminfo.exists():
        total_kb = avail_kb = 0
        for line in meminfo.read_text(encoding="utf-8", errors="replace").splitlines():
            if line.startswith("MemTotal:"):
                total_kb = int(line.split()[1])
            elif line.startswith("MemAvailable:"):
                avail_kb = int(line.split()[1])
        if avail_kb < 512 * 1024:
            raise GuardKilled(f"{what}: low memory ({avail_kb // 1024} MiB available < 512 MiB)")
        return int((avail_kb * 100) // total_kb) if total_kb else 100

    mp = pathlib.Path("/usr/bin/memory_pressure")
    if mp.exists():
        r = subprocess.run([str(mp), "-Q"], capture_output=True, text=True)
        m = re.search(r"(\d+)%", r.stdout)
        pct = int(m.group(1)) if m else 100
        if pct < 40:
            raise GuardKilled(f"{what}: memory_pressure free {pct}% < 40%")
        return pct
    return 100


def guarded_run(cmd, **kw):
    try:
        return subprocess.run(cmd, timeout=90, **kw)
    except subprocess.TimeoutExpired as exc:
        raise GuardKilled(f"command timed out after 90s: {' '.join(str(c) for c in cmd)}") from exc


def run(cmd, what, log=None, **kw):
    """One guarded child. stdout+stderr go to `log` when given."""
    pct = require_memory(what)
    if log is None:
        r = guarded_run(cmd, capture_output=True, text=True, **kw)
    else:
        log = pathlib.Path(log)
        log.parent.mkdir(parents=True, exist_ok=True)
        r = guarded_run(cmd, capture_output=True, text=True, **kw)
        log.write_text(
            f"$ {' '.join(str(c) for c in cmd)}\n# memory free before: {pct}%\n"
            f"# rc={r.returncode}\n--- stdout\n{r.stdout}\n--- stderr\n{r.stderr}\n"
        )
    r.mem_free_before = pct
    return r


def check(r, what):
    if r.returncode != 0:
        sys.stderr.write(f"{what}: rc={r.returncode}\n{r.stdout[-4000:]}\n{r.stderr[-4000:]}\n")
        raise SystemExit(f"{what}: FAILED")
    return r


def need_linux():
    if not OFFSET_H.exists():
        raise SystemExit(f"missing {OFFSET_H}: the gitignored Linux clone re/linux-dc (commit {LINUX_COMMIT}) is needed")
