#!/usr/bin/env python3
"""Verify primary IOFramebuffer 350-slot vtable layout and 25G332 KC symbol exports.

Usage: check-fb-vtable.py [--class CLASSNAME] <linked_kext_binary>
"""
import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", ".."))

DEFAULT_WORK = os.path.join(REPO, "build-offline", "vm-25G332")
DEFAULT_LAYOUT_OUT = os.path.join(DEFAULT_WORK, "layout-25G332")

# Set defaults before importing exports/kcmod because kcmod binds env vars at import time.
os.environ.setdefault("C64_WORK", DEFAULT_WORK)
os.environ.setdefault("C64_LAYOUT_OUT", DEFAULT_LAYOUT_OUT)
os.environ.setdefault("C64_NM", "llvm-nm")

GATES_DIR = os.path.join(REPO, "src", "garnet-accel", "gates")
LAYOUT_DIR = os.path.join(REPO, "tools", "ioaccel-layout")
sys.path.insert(0, GATES_DIR)
sys.path.insert(0, LAYOUT_DIR)

import kextimg  # noqa: E402
import exports  # noqa: E402

WANT_OVR = {
    0, 1, 7, 18, 183, 184, 185,
    305, 309, 310, 311, 312, 313, 314, 315, 316, 317, 318, 319,
    322, 325, 326, 327, 330, 331, 332, 344, 345, 346, 347, 348,
}


def check_vtable(k, cls, c_fb):
    problems = []
    if c_fb["nslots"] != 350 or c_fb["size"] != 464:
        problems.append(
            f"IOFramebuffer model mismatch: nslots={c_fb['nslots']} size={c_fb['size']}"
        )
        return problems, 0, set()

    pure_slots = {r["i"] for r in c_fb["rows"] if r["pure"]}
    missing_pure = sorted(pure_slots - WANT_OVR)
    if missing_pure:
        problems.append(f"pure slots not in WANT_OVR: {missing_pure}")

    ztv = f"__ZTV{len(cls)}{cls}"
    sym = k.by_name.get(ztv)
    if not sym:
        problems.append(f"{cls}: vtable symbol {ztv} not defined in linked kext")
        return problems, 0, set()

    ext, loc = k.build_reloc_maps(0)
    text_segs = [s for s in k.segs if s["name"] == "__TEXT"]
    if not text_segs:
        problems.append("__TEXT segment missing from linked kext")
        return problems, 0, set()
    text = text_segs[0]

    va = sym["value"]
    nslots = c_fb["nslots"]
    got_ovr = set()

    for i in range(nslots):
        a = va + 16 + 8 * i
        row = c_fb["rows"][i]
        if a in ext:
            if i in WANT_OVR:
                problems.append(
                    f"{cls} slot {i}: expected override, found external ref to {ext[a]}"
                )
            elif ext[a] not in row["mangled"]:
                problems.append(
                    f"{cls} slot {i}: references {ext[a]}, IOFramebuffer holds {' | '.join(row['mangled'])}"
                )
        elif a in loc or k.u64(a):
            got_ovr.add(i)
            tgt = k.u64(a)
            if tgt is None or not (text["vmaddr"] <= tgt < text["vmaddr"] + text["vmsize"]):
                problems.append(
                    f"{cls} slot {i}: defined slot points outside __TEXT ({tgt:#x})"
                )
            if i not in WANT_OVR:
                problems.append(
                    f"{cls} slot {i}: UNEXPECTED override ({k.name_at(tgt)})"
                )
        else:
            problems.append(f"{cls} slot {i}: null slot inside vtable")

    for i in sorted(WANT_OVR - got_ovr):
        problems.append(f"{cls} slot {i}: expected override missing")

    term = va + 16 + 8 * nslots
    if term in ext or term in loc or k.u64(term):
        problems.append(
            f"{cls}: vtable missing zero terminator at slot {nslots}"
        )

    return problems, nslots, got_ovr


def check_exports(k):
    ex = exports.load()
    if len(ex) < 10000:
        return [], [], f"exports.load() returned too few symbols ({len(ex)}); ensure C64_NM=llvm-nm"
    und = [
        s["name"]
        for s in k.syms
        if s["sect"] == 0 and (s["type"] & 0xEE) == 0 and s["value"] == 0 and s["name"]
    ]
    missing = sorted(u for u in und if u not in ex)
    return und, missing, None


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--class", dest="cls", default="AMDGarnetPrimaryFramebuffer",
                    help="IOFramebuffer subclass symbol name to verify")
    ap.add_argument("kext", help="Path to linked Mach-O x86_64 kext executable")
    args = ap.parse_args()

    layout_out = os.environ["C64_LAYOUT_OUT"]
    with open(os.path.join(layout_out, "model.json"), "r", encoding="utf-8") as fp:
        c_fb = json.load(fp)["classes"]["IOFramebuffer"]

    k = kextimg.Kext(args.kext)
    pa, nslots, got_ovr = check_vtable(k, args.cls, c_fb)
    und, missing, ex_err = check_exports(k)

    bad = list(pa)
    if ex_err:
        bad.append(ex_err)
    bad.extend(f"import not exported: {m}" for m in missing)

    print(
        f"Gate A ({args.cls} vs IOFramebuffer): {nslots} slots checked "
        f"({len(got_ovr)} own, {nslots - len(got_ovr)} inherited)"
    )
    print(f"Gate B: {len(und)} undefined symbols, {len(missing)} missing from KC exports")

    if bad:
        print(f"FAIL: {len(bad)} problem(s):")
        for item in bad[:20]:
            print(f"  {item}")
        return 1

    print(
        f"PASS: {nslots} vtable slots checked ({len(got_ovr)} own, {nslots - len(got_ovr)} inherited), "
        f"{len(und)} imports resolved (0 missing KC exports)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())

