#!/usr/bin/env python3
"""Export canonical zero-Apple-binary layout fingerprint JSON for GarnetAccel.

Consumes generated C64_LAYOUT_OUT/{model.json,forwarded.json} and
src/garnet-accel/gates/{common.py,leaves.py} to emit:
  - layout-fingerprint.json (schema: garnet-layout-fingerprint-v1)
  - optional variants/index.json (schema: garnet-accel-variants-v1)

Zero Apple Mach-O sections, raw bytes, or local paths are included in outputs.
"""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import sys


SCHEMA_FINGERPRINT = "garnet-layout-fingerprint-v1"
SCHEMA_VARIANTS = "garnet-accel-variants-v1"


def load_leaves_spec(gates_dir: Path):
    """Load LEAVES/OSDEFINE_SLOTS/TRAMP/EXPECT_* from leaves.py cleanly."""
    leaves_path = gates_dir / "leaves.py"
    if not leaves_path.is_file():
        raise ValueError(f"missing leaves.py under {gates_dir}")
    # Provide stub common module constants if needed; leaves.py is standalone.
    spec = importlib.util.spec_from_file_location("garnet_accel_leaves", leaves_path)
    if spec is None or spec.loader is None:
        raise ValueError(f"cannot load {leaves_path}")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def build_fingerprint(layout_out: Path, gates_dir: Path) -> dict:
    """Construct canonical fingerprint dict and compute deterministic layoutId."""
    model_path = layout_out / "model.json"
    fwd_path = layout_out / "forwarded.json"
    if not model_path.is_file():
        raise ValueError(f"missing model.json: {model_path}")
    if not fwd_path.is_file():
        raise ValueError(f"missing forwarded.json: {fwd_path}")

    with model_path.open("r", encoding="utf-8") as f:
        model_root = json.load(f)
    classes_map = model_root.get("classes")
    if not isinstance(classes_map, dict):
        raise ValueError("model.json missing 'classes' dict")
    with fwd_path.open("r", encoding="utf-8") as f:
        forwarded = json.load(f)
    if not isinstance(forwarded, dict):
        raise ValueError("forwarded.json must be a JSON object")

    leaves = load_leaves_spec(gates_dir)
    expect_classes = int(leaves.EXPECT_CLASSES)
    expect_slots = int(leaves.EXPECT_SLOTS)
    if len(leaves.LEAVES) != expect_classes:
        raise ValueError(
            f"LEAVES count {len(leaves.LEAVES)} != EXPECT_CLASSES {expect_classes}"
        )

    osdefine_slots = set(int(x) for x in leaves.OSDEFINE_SLOTS)
    tramp_map = leaves.TRAMP
    class_entries = []
    total_slots = 0

    for leaf, parent, extra in leaves.LEAVES:
        if parent not in classes_map:
            raise ValueError(f"parent class {parent} missing from model.json")
        c = classes_map[parent]
        inst_size = c.get("size")
        nslots = c.get("nslots")
        rows = c.get("rows")
        if not isinstance(inst_size, int) or inst_size <= 0:
            raise ValueError(f"parent {parent} missing positive integer size: {inst_size!r}")
        if not isinstance(nslots, int) or nslots <= 0:
            raise ValueError(f"parent {parent} missing positive integer nslots: {nslots!r}")
        if not isinstance(rows, list) or len(rows) != nslots:
            raise ValueError(
                f"parent {parent} rows count {len(rows) if isinstance(rows, list) else 'invalid'} != nslots {nslots}"
            )
        total_slots += nslots

        ovr = set(osdefine_slots)
        for r in rows:
            idx = r.get("i")
            if not isinstance(idx, int) or idx < 0 or idx >= nslots:
                raise ValueError(f"invalid row index {idx!r} in {parent}")
            if r.get("pure"):
                ovr.add(idx)
        ovr.update(int(x) for x in forwarded.get(parent, []))
        ovr.update(int(x) for x in extra)
        if leaf in tramp_map:
            ovr.update(int(x) for x in tramp_map[leaf][1].keys())

        inherited_bindings = []
        rows_by_idx = {r["i"]: r for r in rows}
        if len(rows_by_idx) != nslots:
            raise ValueError(f"duplicate row indices in {parent}")

        for idx in range(nslots):
            if idx in ovr:
                continue
            r = rows_by_idx[idx]
            if r.get("pure"):
                raise ValueError(f"pure slot {parent}[{idx}] unexpectedly omitted from overrides")
            mangled = r.get("mangled")
            if not isinstance(mangled, list) or not mangled or not all(isinstance(s, str) and s for s in mangled):
                raise ValueError(
                    f"inherited slot {parent}[{idx}] missing exported mangled symbol binding"
                )
            inherited_bindings.append({
                "slot": idx,
                "syms": sorted(set(mangled)),
            })

        class_entries.append({
            "leaf": leaf,
            "parent": parent,
            "instanceSize": inst_size,
            "nslots": nslots,
            "overrideSlots": sorted(ovr),
            "inheritedBindings": inherited_bindings,
        })

    if total_slots != expect_slots:
        raise ValueError(f"total slot count {total_slots} != EXPECT_SLOTS {expect_slots}")

    canonical_core = {
        "schema": SCHEMA_FINGERPRINT,
        "arch": "x86_64",
        "expectClasses": expect_classes,
        "expectSlots": expect_slots,
        "classes": class_entries,
    }
    canonical_bytes = json.dumps(
        canonical_core, sort_keys=True, separators=(",", ":"), ensure_ascii=True
    ).encode("utf-8")
    layout_id = hashlib.sha256(canonical_bytes).hexdigest()

    return {
        "schema": SCHEMA_FINGERPRINT,
        "arch": "x86_64",
        "expectClasses": expect_classes,
        "expectSlots": expect_slots,
        "layoutId": layout_id,
        "shortId": layout_id[:16],
        "classes": class_entries,
    }


def write_json_atomic(path: Path, payload: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    text = json.dumps(payload, indent=2, sort_keys=False, ensure_ascii=True) + "\n"
    tmp = path.with_name(f".{path.name}.tmp.{os.getpid()}")
    tmp.write_text(text, encoding="utf-8")
    os.replace(tmp, path)


def build_variants_index(
    fingerprint: dict,
    accel_rel: str,
    fingerprint_rel: str,
    verified_builds: list[str],
    existing_index_path: Path | None = None,
) -> dict:
    layout_id = fingerprint["layoutId"]
    short_id = fingerprint["shortId"]
    entry = {
        "layoutId": layout_id,
        "shortId": short_id,
        "accelBundleRelPath": accel_rel,
        "fingerprintRelPath": fingerprint_rel,
        "verifiedBuilds": sorted(set(b for b in verified_builds if b)),
    }
    variants = {}
    if existing_index_path and existing_index_path.is_file():
        old = json.loads(existing_index_path.read_text(encoding="utf-8"))
        if old.get("schema") != SCHEMA_VARIANTS:
            raise ValueError(f"unexpected variants index schema: {old.get('schema')!r}")
        for item in old.get("variants", []):
            lid = item.get("layoutId")
            if lid:
                variants[lid] = item
        if layout_id in variants:
            merged_builds = set(variants[layout_id].get("verifiedBuilds", []))
            merged_builds.update(entry["verifiedBuilds"])
            entry["verifiedBuilds"] = sorted(merged_builds)
    variants[layout_id] = entry
    ordered = [variants[k] for k in sorted(variants.keys())]
    return {
        "schema": SCHEMA_VARIANTS,
        "defaultVariant": layout_id,
        "variants": ordered,
    }


def main(argv: list[str] | None = None) -> int:
    repo = Path(__file__).resolve().parents[2]
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--layout-out",
        required=True,
        help="Path to C64_LAYOUT_OUT containing model.json and forwarded.json",
    )
    ap.add_argument(
        "--gates-dir",
        default=str(repo / "src" / "garnet-accel" / "gates"),
        help="Path to src/garnet-accel/gates",
    )
    ap.add_argument("--out", required=True, help="Output layout-fingerprint.json path")
    ap.add_argument("--variants-index", help="Optional variants/index.json path to write/update")
    ap.add_argument("--accel-rel", help="Relative path to GarnetAccel.kext inside payload")
    ap.add_argument("--fingerprint-rel", help="Relative path to layout-fingerprint.json inside payload")
    ap.add_argument(
        "--verified-build",
        action="append",
        default=[],
        help="Verified macOS build tag (e.g. 25G332); repeatable",
    )
    ap.add_argument("--print-layout-id", action="store_true", help="Print layoutId stdout")
    args = ap.parse_args(argv)

    try:
        fp = build_fingerprint(Path(args.layout_out), Path(args.gates_dir))
    except (ValueError, OSError, json.JSONDecodeError) as exc:
        print(f"export_fingerprint.py: ERROR: {exc}", file=sys.stderr)
        return 1

    out_path = Path(args.out)
    write_json_atomic(out_path, fp)

    if args.variants_index:
        short_id = fp["shortId"]
        accel_rel = args.accel_rel or f"variants/{short_id}/GarnetAccel.kext"
        fp_rel = args.fingerprint_rel or f"variants/{short_id}/layout-fingerprint.json"
        idx_path = Path(args.variants_index)
        idx = build_variants_index(
            fp, accel_rel, fp_rel, args.verified_build, existing_index_path=idx_path
        )
        write_json_atomic(idx_path, idx)

    if args.print_layout_id:
        print(fp["layoutId"])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
