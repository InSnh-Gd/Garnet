#!/usr/bin/env python3
"""Offline unit and real-layout tests for tools/ioaccel-layout/export_fingerprint.py."""

from __future__ import annotations

import copy
import json
from pathlib import Path
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(HERE))

import export_fingerprint as ef  # noqa: E402


def make_synthetic_layout(tmp: Path) -> Path:
    gates_dir = REPO / "src" / "garnet-accel" / "gates"
    leaves = ef.load_leaves_spec(gates_dir)
    # Match EXPECT_CLASSES (13) and EXPECT_SLOTS (2720) with exact parent slot counts
    # if present in build-offline, otherwise distribute deterministic synthetic counts.
    sample_counts = {
        "IOGraphicsAccelerator2": (3544, 350),
        "IOAccelEventMachineFast2": (3376, 95),
        "IOAccelTask": (600, 51),
        "IOAccelDisplayMachine": (376, 284),
        "IOAccelSysMemory": (408, 66),
        "IOAccelMemoryMap": (280, 57),
        "IOAccelVidMemory": (272, 69),
        "IOAccelResource2": (384, 79),
        "IOAccel2DContext2": (4352, 368),
        "IOAccelSharedUserClient2": (360, 308),
        "IOAccelCommandQueue": (1704, 337),
        "IOAccelDisplayPipe": (792, 306),
        "IOFramebuffer": (464, 350),
    }
    classes = {}
    forwarded = {}
    for _, parent, _ in leaves.LEAVES:
        inst_size, nslots = sample_counts[parent]
        fwd_slots = [nslots - 2] if nslots > 10 else []
        forwarded[parent] = fwd_slots
        rows = []
        for idx in range(nslots):
            is_pure = idx in (3, 4)
            rows.append({
                "i": idx,
                "off": 16 + 8 * idx,
                "pure": is_pure,
                "mangled": [] if is_pure else [f"__ZN{len(parent)}{parent}4slot{idx}Ev"],
                "name": "__cxa_pure_virtual" if is_pure else f"{parent}::slot{idx}()",
            })
        classes[parent] = {
            "size": inst_size,
            "nslots": nslots,
            "rows": rows,
        }
    layout_dir = tmp / "layout-out"
    layout_dir.mkdir(parents=True, exist_ok=True)
    (layout_dir / "model.json").write_text(json.dumps({"classes": classes}), encoding="utf-8")
    (layout_dir / "forwarded.json").write_text(json.dumps(forwarded), encoding="utf-8")
    return layout_dir


class ExportFingerprintTest(unittest.TestCase):
    def setUp(self):
        self.gates_dir = REPO / "src" / "garnet-accel" / "gates"

    def test_deterministic_export_and_variants_index(self):
        with tempfile.TemporaryDirectory() as td:
            tmp = Path(td)
            layout_dir = make_synthetic_layout(tmp)
            out1 = tmp / "fp1.json"
            out2 = tmp / "fp2.json"
            idx_path = tmp / "variants" / "index.json"

            self.assertEqual(
                ef.main([
                    "--layout-out", str(layout_dir),
                    "--gates-dir", str(self.gates_dir),
                    "--out", str(out1),
                    "--variants-index", str(idx_path),
                    "--verified-build", "25G332",
                ]),
                0,
            )
            self.assertEqual(
                ef.main([
                    "--layout-out", str(layout_dir),
                    "--gates-dir", str(self.gates_dir),
                    "--out", str(out2),
                ]),
                0,
            )
            self.assertEqual(out1.read_bytes(), out2.read_bytes())

            fp = json.loads(out1.read_text(encoding="utf-8"))
            self.assertEqual(fp["schema"], "garnet-layout-fingerprint-v1")
            self.assertEqual(fp["arch"], "x86_64")
            self.assertEqual(fp["expectClasses"], 13)
            self.assertEqual(fp["expectSlots"], 2720)
            self.assertEqual(len(fp["layoutId"]), 64)
            self.assertEqual(fp["shortId"], fp["layoutId"][:16])
            self.assertEqual(len(fp["classes"]), 13)

            for cls in fp["classes"]:
                ovr = set(cls["overrideSlots"])
                inh = {item["slot"] for item in cls["inheritedBindings"]}
                self.assertEqual(ovr & inh, set())
                self.assertEqual(len(ovr) + len(inh), cls["nslots"])
                self.assertTrue({0, 1, 7}.issubset(ovr))
                for b in cls["inheritedBindings"]:
                    self.assertTrue(len(b["syms"]) >= 1)

            raw_text = out1.read_text(encoding="utf-8")
            self.assertNotIn("/Users/", raw_text)
            self.assertNotIn("/home/", raw_text)

            idx = json.loads(idx_path.read_text(encoding="utf-8"))
            self.assertEqual(idx["schema"], "garnet-accel-variants-v1")
            self.assertEqual(idx["defaultVariant"], fp["layoutId"])
            self.assertEqual(len(idx["variants"]), 1)
            self.assertEqual(idx["variants"][0]["verifiedBuilds"], ["25G332"])

    def test_layout_id_changes_on_size_or_symbol_mutation(self):
        with tempfile.TemporaryDirectory() as td:
            tmp = Path(td)
            layout_dir = make_synthetic_layout(tmp)
            fp_base = ef.build_fingerprint(layout_dir, self.gates_dir)

            # Mutate instanceSize
            model_path = layout_dir / "model.json"
            orig_model = json.loads(model_path.read_text(encoding="utf-8"))
            mut_size = copy.deepcopy(orig_model)
            mut_size["classes"]["IOFramebuffer"]["size"] += 8
            model_path.write_text(json.dumps(mut_size), encoding="utf-8")
            fp_size = ef.build_fingerprint(layout_dir, self.gates_dir)
            self.assertNotEqual(fp_size["layoutId"], fp_base["layoutId"])

            # Mutate an inherited slot symbol
            mut_sym = copy.deepcopy(orig_model)
            mut_sym["classes"]["IOFramebuffer"]["rows"][2]["mangled"] = ["__ZN13IOFramebuffer9differentEv"]
            model_path.write_text(json.dumps(mut_sym), encoding="utf-8")
            fp_sym = ef.build_fingerprint(layout_dir, self.gates_dir)
            self.assertNotEqual(fp_sym["layoutId"], fp_base["layoutId"])

    def test_fail_closed_on_missing_class_or_symbol(self):
        with tempfile.TemporaryDirectory() as td:
            tmp = Path(td)
            layout_dir = make_synthetic_layout(tmp)
            model_path = layout_dir / "model.json"
            orig_model = json.loads(model_path.read_text(encoding="utf-8"))

            # Empty mangled symbol on inherited slot
            bad_sym = copy.deepcopy(orig_model)
            bad_sym["classes"]["IOFramebuffer"]["rows"][2]["mangled"] = []
            model_path.write_text(json.dumps(bad_sym), encoding="utf-8")
            with self.assertRaises(ValueError):
                ef.build_fingerprint(layout_dir, self.gates_dir)

            # Missing class
            bad_cls = copy.deepcopy(orig_model)
            del bad_cls["classes"]["IOAccelTask"]
            model_path.write_text(json.dumps(bad_cls), encoding="utf-8")
            with self.assertRaises(ValueError):
                ef.build_fingerprint(layout_dir, self.gates_dir)

    def test_real_25g332_layout_if_present(self):
        real_layout = REPO / "build-offline" / "vm-25G332" / "layout-25G332"
        if not (real_layout / "model.json").is_file():
            return
        fp = ef.build_fingerprint(real_layout, self.gates_dir)
        self.assertEqual(fp["expectClasses"], 13)
        self.assertEqual(fp["expectSlots"], 2720)
        self.assertEqual(len(fp["classes"]), 13)


if __name__ == "__main__":
    unittest.main()
