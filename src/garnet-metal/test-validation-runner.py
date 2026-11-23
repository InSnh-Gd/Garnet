#!/usr/bin/env python3
"""Failure propagation checks for the compatibility runner; no Metal or GPU work."""

import ast
import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

spec = importlib.util.spec_from_file_location("compatibility", Path(__file__).with_name("run-compatibility.py"))
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


class RunnerTests(unittest.TestCase):
    def test_device_assembly_preserves_source_extraction_contract(self):
        source = Path(__file__).parent
        spec = importlib.util.spec_from_file_location(
            "device_assembly", source / "assemble-device.py")
        assembly = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(assembly)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            device = root / "GarnetDevice.m"
            device.write_text('#include "rdna4_test_impl.inc"\n'
                              '#include "rdna4_argument.inc"\n')
            (root / "rdna4_test_impl.inc").write_text("static int test;\n")
            self.assertEqual(assembly.assemble(device, root),
                             'static int test;\n#include "rdna4_argument.inc"\n')
            (root / "rdna4_test_impl.inc").write_text('#include "rdna4_child_impl.inc"\n')
            (root / "rdna4_child_impl.inc").write_text("static int child;\n")
            self.assertEqual(assembly.assemble(device, root),
                             'static int child;\n#include "rdna4_argument.inc"\n')
            (root / "rdna4_child_impl.inc").write_text('#include "rdna4_test_impl.inc"\n')
            with self.assertRaisesRegex(ValueError, "cyclic device module include"):
                assembly.assemble(device, root)
            device.write_text("legacy monolithic source\n")
            self.assertEqual(assembly.assemble(device, root), device.read_text())
            device.write_text('#include "rdna4_missing_impl.inc"\n')
            with self.assertRaises(FileNotFoundError):
                assembly.assemble(device, root)
        expanded = assembly.assemble(source / "GarnetDevice.m", source)
        self.assertNotIn('_impl.inc"', expanded)
        self.assertEqual(expanded.count("@implementation GarnetDevice\n"), 1)

    def test_tier2_manifest_is_complete_and_listable_without_metal(self):
        path, manifest = runner.load_tier2_manifest()
        self.assertTrue(path.is_file())
        ids = [case["id"] for case in manifest["cases"]]
        self.assertEqual(len(ids), len(set(ids)))
        result = subprocess.run(
            [sys.executable, runner.__file__, "--list-cases"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        listed = json.loads(result.stdout)
        self.assertEqual([case["id"] for case in listed["cases"]], ids)

    def test_tier2_case_selects_only_its_runner_cases_and_sides(self):
        _, manifest = runner.load_tier2_manifest()
        selection = runner.Tier2Selection(manifest, ["T02.quad-uniform-control"])
        self.assertEqual(selection.runner_groups, ["argument-quad-lod"])
        self.assertFalse(selection.matches("argument-quad-lod-divergent-reference"))
        self.assertTrue(selection.matches("argument-quad-lod-uniform-reference"))
        self.assertTrue(selection.matches("argument-quad-lod-uniform-bundle"))
        self.assertEqual(selection.missing(), {})
        self.assertEqual(
            selection.report()["matched"]["T02.quad-uniform-control:reference"],
            ["argument-quad-lod-uniform-reference"])

    def test_tier2_selection_reports_a_missing_side(self):
        _, manifest = runner.load_tier2_manifest()
        selection = runner.Tier2Selection(manifest, ["T02.quad-explicit-gradient-control"])
        self.assertTrue(selection.matches("argument-quad-lod-gradient-reference"))
        self.assertEqual(
            selection.missing(),
            {("T02.quad-explicit-gradient-control", "candidate"):
             ["argument-quad-lod-gradient-bundle"]})

    def test_selection_requires_every_declared_pattern(self):
        _, manifest = runner.load_tier2_manifest()
        selection = runner.Tier2Selection(
            manifest, ["T03.sampler-coordinate-support-host"])
        self.assertTrue(selection.matches("sampler-coordinate-runtime"))
        self.assertEqual(selection.missing(), {
            ("T03.sampler-coordinate-support-host", "candidate"):
            ["sampler-coordinate-metadata", "sampler-coordinate-state"],
        })

    def test_ready_groups_dispatch_exact_commands_without_running_fixtures(self):
        _, manifest = runner.load_tier2_manifest()
        groups = {
            "summary-event-progress", "gather-capability", "null-runtime",
            "null-shape", "null-shape-reflection", "null-remap",
            "argument-gather-cube", "argument-image-operations",
            "summary-plan", "summary-owners", "summary-runtime",
            "writer-epochs-host", "argument-capacity", "argument-public-sampler",
            "argument-pool", "argument-limit-snapshot",
            "argument-limit-snapshot-device", "snapshot-admission",
            "argument-scan-host",
            "argument-contract", "argument-resources",
            "argument-sampling-matrix",
            "argument-sampling-array",
            "argument-depth-mips",
            "argument-sampling-limits",
        }
        cases = [case for case in manifest["cases"]
                 if case["runner_group"] in groups or case["id"] in (
                     "T02.quad-margin-sampler-implicit",
                     "T02.quad-margin-image-implicit",
                     "T02.quad-margin-control-coordinate",
                     "T02.quad-margin-control-lod1")]
        self.assertEqual({case["runner_group"] for case in cases},
                         groups | {"quad-margin"})
        invocations = {}

        def record(out, name, command, marker, **kwargs):
            self.assertNotIn(name, invocations)
            invocations[name] = (list(command), marker, kwargs)
            return True

        with tempfile.TemporaryDirectory(dir=os.environ.get("TMPDIR")) as directory:
            base = Path(directory)
            bundle, include, output = (base / name for name in ("bundle", "include", "output"))
            bundle.mkdir()
            include.mkdir()
            arguments = [runner.__file__, "vm", str(bundle), str(include), str(output),
                         "--sampling-bias-limit", "15.9989996"]
            for case in cases:
                arguments.extend(("--case", case["id"]))
            with mock.patch.object(sys, "argv", arguments), \
                    mock.patch.object(sys, "platform", "darwin"), \
                    mock.patch.object(os, "geteuid", return_value=0), \
                    mock.patch.object(subprocess, "check_output", return_value="VMM"), \
                    mock.patch.object(runner, "run_case", side_effect=record):
                self.assertEqual(runner.main(), 0)
            report = json.loads((output / "tier2-selection.json").read_text())
            self.assertEqual(report["missing"], {})
            expected = {pattern for case in cases for side in ("reference", "candidate")
                        for pattern in case[side]["runner_cases"]}
            actual = {name for name, (_, _, options) in invocations.items()
                      if not options.get("build")}
            self.assertEqual(actual, expected)
            for name, (command, marker, options) in invocations.items():
                with self.subTest(invocation=name):
                    if options.get("build"):
                        for flag in ("-arch", "x86_64", "-Wall", "-Wextra", "-Werror"):
                            self.assertIn(flag, command)
                    else:
                        self.assertNotEqual(options["evidence_class"], "unclassified")
                        self.assertTrue(marker)
            for control in ("coordinate", "lod1"):
                for side in ("reference", "bundle"):
                    command, _, options = invocations[
                        f"quad-margin-control-{control}-{side}"]
                    self.assertEqual(command[-2:], ["--control", control])
                    self.assertNotIn("--sampling", command)
                    self.assertIn(
                        "modes=1 samplings=1 flips=1 cases=1" if control == "coordinate"
                        else "modes=3 samplings=1 flips=2 cases=6", options["required"])
            for mode in ("sampler", "image"):
                for side in ("reference", "bundle"):
                    command = invocations[f"quad-margin-{mode}-implicit-{side}"][0]
                    self.assertEqual(command[-4:], ["--mode", mode, "--sampling", "implicit"])
                    self.assertNotIn("--control", command)
            for side in ("reference", "bundle"):
                for comparison, code in (("never", 0), ("less", 1), ("always", 7)):
                    command, _, options = invocations[f"image-null-sampling-depth-2d-0-{comparison}-{side}"]
                    self.assertEqual(command[-4:], ["--case", "depth-2d-0", "--null-sampling", comparison])
                    self.assertIn(f"compare={code}", options["required"])
                    self.assertIn("dual_use=1", options["required"])
                    self.assertIn("output_words=7", options["required"])
                for kind in ("color-2d-0", "color-2d_ms-1", "depth-cube_array-0"):
                    command, _, options = invocations[f"image-null-guard-{kind}-{side}"]
                    self.assertEqual(command[-3:], ["--case", kind, "--null-guard"])
                    self.assertNotIn("--compile-only", command)
                    for required in ("dispatches=8", "storage=shared,private", "null_members=0"):
                        self.assertIn(required, options["required"])
                    self.assertEqual(options["evidence_class"],
                                     "gpu-reference" if side == "reference" else "gpu-candidate")
                for kind in ("color-2d_ms-0", "color-2d_ms-1", "color-2d_ms-2", "depth-2d_ms-0"):
                    command, _, options = invocations[f"argument-image-operations-{kind}-{side}"]
                    self.assertEqual(command[-2:], ["--case", kind])
                    self.assertNotIn("--compile-only", command)
                    self.assertIn("sample=2", options["required"])
                    self.assertEqual(options["evidence_class"],
                                     "gpu-reference" if side == "reference" else "gpu-candidate")
                    command, _, options = invocations[f"argument-image-direct-ms-read-{kind}-{side}"]
                    self.assertEqual(command[-5:], ["--case", kind, "--direct-ms-read", "--operation", "read"])
                    self.assertNotIn("--compile-only", command)
                    self.assertIn("sample=2", options["required"])
                    self.assertIn("direct_read=1", options["required"])
                    self.assertEqual(options["evidence_class"],
                                     "gpu-reference" if side == "reference" else "gpu-candidate")
                    command, _, options = invocations[f"argument-image-direct-ms-query-controls-{kind}-{side}"]
                    self.assertEqual(command[-3:], ["--case", kind, "--direct-ms-read"])
                    self.assertIn("operation=all", options["required"])
                    command, _, options = invocations[f"argument-image-ms-query-only-{kind}-{side}"]
                    self.assertEqual(command[-4:], ["--case", kind, "--operation", "query"])
                    self.assertNotIn("--direct-ms-read", command)
                    self.assertIn("operation=query", options["required"])
                command, _, options = invocations["argument-image-operations-" + side]
                self.assertEqual(command[1:3], ["--" + side,
                    "-" if side == "reference" else str(bundle)])
                self.assertIn("cases=29 selections=58 msl=2.1", options["required"])
                for shape in ("cube", "cube_array"):
                    for operation in ("operations", "read-grid"):
                        depth_name = f"argument-image-{operation}-depth-{shape}-0-" + (
                            "reference-capture" if side == "reference" else "bundle")
                        depth_command, _, depth_options = invocations[depth_name]
                        self.assertEqual(depth_command[depth_command.index("--case") + 1],
                                         f"depth-{shape}-0")
                        if side == "reference":
                            self.assertEqual(depth_command[-1], "--compile-only")
                            self.assertTrue(depth_options["capture"])
                            self.assertEqual(depth_options["evidence_class"], "capture-only")
                        else:
                            self.assertNotIn("--compile-only", depth_command)
                            self.assertEqual(depth_options["evidence_class"], "gpu-candidate")
                        if operation == "read-grid":
                            self.assertEqual(depth_command[depth_command.index("--operation") + 1],
                                             "read-grid")
                        else:
                            self.assertNotIn("--operation", depth_command)
                command, _, options = invocations["argument-sampling-npot-bias--2-" + side]
                self.assertEqual(command[-3:], ["npot", "bias", "-2"])
                self.assertIn("draws=8", options["required"])
                command, _, options = invocations["argument-sampling-npot-grad-negative-over-" + side]
                self.assertEqual(command[-4:], ["npot", "grad", "-19.9989996", "15.9989996"])
                self.assertIn("bias_limit=15.9989996", options["required"])
                command, _, options = invocations["argument-sampling-array-npot-grad--2-" + side]
                self.assertEqual(command[-3:], ["array-npot", "grad", "-2"])
                self.assertIn("ARGUMENT_SAMPLING_ARRAY_PATTERN layers=1:2 offset=1:-1 spatial=1",
                              options["required"])
                command, _, options = invocations["argument-depth-mips-array-npot-level0-" + side]
                self.assertEqual(command[-2:], ["array-npot", "level0"])
                self.assertIn("layers=1:2", options["required"])
                self.assertIn("guards=target,argument,control,upload gpu_execution=1", options["required"])
                command, _, options = invocations["argument-depth-mips-array-npot-mixed-" + side]
                self.assertEqual(command[-2:], ["array-npot", "mixed"])
                self.assertIn("ARGUMENT_DEPTH_MIXED_TOKEN ordinary=1 comparison=3 shared_dynamic_token=1",
                              options["required"])
            reflection = invocations["null-shape-reflection"][0]
            command, marker, options = invocations["multisample-binding-host"]
            self.assertEqual(Path(command[0]).name, "test-multisample-binding")
            self.assertEqual(marker, "MULTISAMPLE_BINDING_PASS")
            self.assertEqual(options["evidence_class"], "host-mock")
            self.assertIn("matrix=12", options["required"])
            self.assertIn("gpu_execution=0", options["required"])
            command, marker, options = invocations["null-sampler-bindings"]
            self.assertEqual(Path(command[0]).name, "test-null-sampler-bindings")
            self.assertEqual(marker, "NULL_SAMPLER_BINDINGS_PASS")
            self.assertEqual(options["evidence_class"], "host-mock")
            self.assertIn("undefined_gpu_oracle=0", options["required"])
            command, marker, options = invocations["null-texel-dummies"]
            self.assertEqual(Path(command[0]).name, "test-null-texel-dummies")
            self.assertEqual(marker, "NULL_TEXEL_DUMMIES_PASS")
            self.assertEqual(options["evidence_class"], "host-mock")
            for required in ("shapes=6", "failures=42", "typed_buffer_views=1", "gpu_execution=0"):
                self.assertIn(required, options["required"])
            self.assertEqual([Path(value).name for value in reflection[1:]],
                             ["null-shapes-abi9.spv", "null-shapes-invalid.spv"])
            runtime_build = invocations["build-test-argument-summary-runtime"][0]
            self.assertEqual(runtime_build[runtime_build.index("-x") + 1], "c")
            metadata = json.loads((output / "metadata.json").read_text())
            self.assertIn(".c", {Path(name).suffix for name in metadata["source_sha256"]})
            fixture = Path(runner.__file__).parent / "fixtures/null-shapes-abi9.spv"
            self.assertEqual(metadata["fixture_sha256"]["fixtures/null-shapes-abi9.spv"],
                             hashlib.sha256(fixture.read_bytes()).hexdigest())

    def test_sampling_limits_require_a_usable_queried_bound_before_platform_access(self):
        for limit in (None, "nan", "inf", "-inf", "-1", "121"):
            with self.subTest(limit=limit):
                command = [sys.executable, runner.__file__, "vm", "/missing/bundle",
                           "/missing/include", "/must-not-be-created", "--case",
                           "T02.sampling-limits.npot.bias.positive-limit"]
                if limit is not None:
                    command.append("--sampling-bias-limit=" + limit)
                result = subprocess.run(command, capture_output=True, text=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("sampling-limits requires --sampling-bias-limit", result.stderr)

    def test_target_memory_cases_reject_vm_before_platform_or_bundle_access(self):
        for case_id in ("A03.large-addressing-small-backing",
                        "A03.physical-over4g-private",
                        "A03.physical-over4g-shared",
                        "A03.physical-over4g-managed"):
            with self.subTest(case=case_id):
                result = subprocess.run(
                    [sys.executable, runner.__file__, "vm", "/missing/bundle",
                     "/missing/include", "/must-not-be-created", "--case", case_id],
                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                self.assertEqual(result.returncode, 2)
                self.assertIn("requires explicit live mode on target hardware", result.stderr)
                self.assertNotIn("run as root on macOS", result.stderr)

    def test_every_manifest_pattern_has_a_dispatch_without_executing_children(self):
        _, manifest = runner.load_tier2_manifest()
        with tempfile.TemporaryDirectory(dir=os.environ.get("TMPDIR")) as directory:
            base = Path(directory)
            bundle, include, samples = (base / name for name in ("bundle", "include", "samples"))
            for path in (bundle, include, samples):
                path.mkdir()
            output = base / "output"
            arguments = [runner.__file__, "live", str(bundle), str(include), str(output),
                         "--metadata-samples", str(samples), "--sampling-bias-limit", "15.9989996"]
            for case in manifest["cases"]:
                arguments.extend(("--case", case["id"]))
            with mock.patch.object(sys, "argv", arguments), \
                    mock.patch.object(sys, "platform", "darwin"), \
                    mock.patch.object(os, "geteuid", return_value=0), \
                    mock.patch.dict(os.environ, {"RADV_DARWIN_MOCK": "0"}), \
                    mock.patch.object(subprocess, "check_output", return_value=""), \
                    mock.patch.object(runner, "run_case", return_value=True):
                self.assertEqual(runner.main(), 0)
            report = json.loads((output / "tier2-selection.json").read_text())
            self.assertEqual(report["missing"], {})
            self.assertEqual(set(report["selected_cases"]),
                             {case["id"] for case in manifest["cases"]})

    def test_tier2_sampler_usage_selects_full_reference(self):
        _, manifest = runner.load_tier2_manifest()
        selection = runner.Tier2Selection(
            manifest, ["T03.sampler-usage", "T03.sampler-state-host",
                       "T03.sampler-runtime-host"])
        self.assertEqual(selection.runner_groups, ["sampler-usage"])
        self.assertTrue(selection.matches("sampler-usage-reference"))
        self.assertFalse(selection.matches("sampler-usage-reference-capture"))
        self.assertTrue(selection.matches("sampler-usage-bundle"))
        self.assertTrue(selection.matches("sampler-state"))
        self.assertTrue(selection.matches("sampler-runtime"))
        self.assertEqual(
            selection.evidence_class("sampler-coordinate-runtime"),
            "host-mock")
        self.assertEqual(
            selection.evidence_class("sampler-coordinate-metadata"),
            "host-mock")
        self.assertEqual(
            selection.evidence_class("sampler-coordinate-state"),
            "host-cpu")
        self.assertEqual(selection.missing(), {})

    def test_new_fixture_manifest_cases_declare_evidence(self):
        _, manifest = runner.load_tier2_manifest()
        by_id = {case["id"]: case for case in manifest["cases"]}
        expected = {
            "T03.sampler-compare-functions": (
                "sampler-compare", "gpu-reference", "gpu-candidate"),
            "T03.sampler-compare-capture": (
                "sampler-compare", "capture-only", "capture-only"),
            "A04.writer-epochs": (
                "writer-epochs", "gpu-reference", "gpu-candidate"),
            "T02.private-retire-layout": (
                "private-retire", "not-applicable", "layout-only"),
            "T02.private-retire-gpu": (
                "private-retire", "gpu-reference", "gpu-candidate"),
            "T02.private-summary-runtime": (
                "private-summary", "not-applicable", "gpu-candidate"),
            "A04.submit-faults-host": (
                "submit-faults", "not-applicable", "host-mock"),
            "A04.segment-retirement": (
                "segment-retire", "not-applicable", "host-mock"),
            "T03.sampler-api-reject": (
                "sampler-api-reject", "not-applicable", "host-mock"),
            "T03.sampler-metadata": (
                "sampler-metadata", "not-applicable", "host-mock"),
            "T03.sampler-coordinates": (
                "sampler-coordinates", "gpu-reference", "gpu-candidate"),
            "T03.sampler-coordinates-capture": (
                "sampler-coordinates", "capture-only", "capture-only"),
            "T03.sampler-coordinate-support-host": (
                "sampler-usage", "not-applicable", "host-mock"),
            "T02.quad-margin-implicit": (
                "quad-margin", "gpu-reference", "gpu-candidate"),
            "T02.quad-margin-bias1": (
                "quad-margin", "gpu-reference", "gpu-candidate"),
            "T02.quad-margin-gradient": (
                "quad-margin", "gpu-reference", "gpu-candidate"),
            "T03.sampler-binding-state": (
                "sampler-binding", "not-applicable", "host-mock"),
            "T03.sampler-binding-oracle-host": (
                "sampler-binding", "not-applicable", "host-cpu"),
            "A04.argument-scan-runtime-host": (
                "argument-scan-runtime", "not-applicable", "host-mock"),
            "T03.reflection-trim-abi8": (
                "reflection-trim", "not-applicable", "offline-metadata"),
        }
        for case_id, (group, reference_class, candidate_class) in expected.items():
            with self.subTest(case_id=case_id):
                case = by_id[case_id]
                self.assertEqual(case["runner_group"], group)
                self.assertEqual(
                    case["reference"]["execution_class"], reference_class)
                self.assertEqual(
                    case["candidate"]["execution_class"], candidate_class)
                if reference_class in ("gpu-reference", "capture-only"):
                    self.assertIn("MSL 2.1", case["oracle"])
                self.assertTrue(case["stage"])
                self.assertTrue(case["storage"])
                self.assertTrue(case["operation"])

    def test_quad_margin_sampling_selectors_are_independent(self):
        _, manifest = runner.load_tier2_manifest()
        selection = runner.Tier2Selection(
            manifest, ["T02.quad-margin-gradient"])
        self.assertEqual(
            selection.evidence_class("quad-margin-gradient-reference"),
            "gpu-reference")
        self.assertEqual(
            selection.evidence_class("quad-margin-gradient-bundle"),
            "gpu-candidate")
        self.assertIsNone(
            selection.evidence_class("quad-margin-implicit-reference"))
        self.assertIsNone(
            selection.evidence_class("quad-margin-bias1-bundle"))
        self.assertEqual(selection.missing(), {})
        implicit = runner.Tier2Selection(
            manifest, ["T02.quad-margin-implicit"])
        self.assertTrue(implicit.matches("quad-margin-implicit-reference"))
        self.assertFalse(implicit.matches("quad-margin-gradient-reference"))
        known_failure = next(
            case for case in manifest["cases"]
            if case["id"] == "T02.quad-margin-implicit")["known_failure"]
        self.assertEqual(known_failure["status"], "open")
        self.assertIn("actual=23 expected=21", known_failure["observation"])

    def test_capture_private_layout_and_writer_selectors_stay_separate(self):
        _, manifest = runner.load_tier2_manifest()
        capture = runner.Tier2Selection(
            manifest, ["T03.sampler-compare-capture"])
        self.assertEqual(
            capture.evidence_class("sampler-compare-reference-capture"),
            "capture-only")
        self.assertEqual(
            capture.evidence_class("sampler-compare-bundle-capture"),
            "capture-only")
        self.assertIsNone(capture.evidence_class("sampler-compare-reference"))
        self.assertEqual(capture.missing(), {})

        layout = runner.Tier2Selection(
            manifest, ["T02.private-retire-layout"])
        self.assertEqual(
            layout.evidence_class("private-retire-layout"), "layout-only")
        self.assertIsNone(layout.evidence_class("private-retire-bundle"))
        self.assertEqual(layout.missing(), {})

        retired = runner.Tier2Selection(
            manifest, ["A04.sampler-retirement"])
        self.assertTrue(retired.matches("sampler-retire-reference"))
        self.assertTrue(retired.matches("sampler-retire-bundle"))
        self.assertFalse(
            retired.matches("sampler-retire-writer-epochs-reference"))
        self.assertEqual(retired.missing(), {})

    def test_runner_registers_new_host_and_parameterized_selectors(self):
        source = Path(runner.__file__).read_text()
        for group in (
                "writer-epochs", "sampler-compare", "sampler-api-reject",
                "sampler-metadata", "sampler-coordinates", "sampler-binding",
                "argument-scan-runtime", "private-summary", "submit-faults",
                "private-retire", "segment-retire", "quad-margin",
                "reflection-trim"):
            self.assertIn(group, runner.TIER2_RUNNER_GROUPS)
        for text in (
                '"--only", "writer-epochs"',
                '"--layout-only"',
                '"--sampling", sampling',
                '"test-reflection-trim.py"',
                '"--metadata-samples"',
                '"test-argument-quad-margin"',
                '"test-argument-private-retire"',
                '"test-argument-segment-retire"',
                '"test-argument-scan-runtime"',
                '"test-argument-private-summary"',
                '"test-argument-submit-faults"',
                '"test-sampler-api-reject"',
                '"test-sampler-metadata"',
                '"test-sampler-coordinates"',
                '"test-sampler-coordinate-runtime"',
                '"test-sampler-coordinate-metadata"',
                '"test-sampler-coordinate-state.c"',
                '"-fblocks", "-O1"',
                '"-DRDNA4_SAMPLER_BINDING_ORACLE_ONLY"',
                '"actual_fragment=1"'):
            self.assertIn(text, source)

    def test_every_new_case_matches_its_runner_names_and_classes(self):
        _, manifest = runner.load_tier2_manifest()
        expected = {
            "A02.nested-ms-query-copy": {
                "argument-nested-pointer-query-ms-copy-reference-capture": "capture-only",
                "argument-nested-pointer-query-ms-copy-bundle": "gpu-candidate",
            },
            "T03.sampler-compare-functions": {
                "sampler-compare-reference": "gpu-reference",
                "sampler-compare-bundle": "gpu-candidate",
            },
            "T03.sampler-compare-capture": {
                "sampler-compare-reference-capture": "capture-only",
                "sampler-compare-bundle-capture": "capture-only",
            },
            "A04.writer-epochs": {
                "sampler-retire-writer-epochs-reference": "gpu-reference",
                "sampler-retire-writer-epochs-bundle": "gpu-candidate",
            },
            "T02.private-retire-layout": {
                "private-retire-layout": "layout-only",
            },
            "T02.private-retire-gpu": {
                "private-retire-reference": "gpu-reference",
                "private-retire-bundle": "gpu-candidate",
            },
            "T02.private-summary-runtime": {
                "private-summary-runtime": "gpu-candidate",
            },
            "A04.submit-faults-host": {
                "submit-faults": "host-mock",
            },
            "A04.segment-retirement": {
                "segment-retire": "host-mock",
            },
            "T03.sampler-api-reject": {
                "sampler-api-reject": "host-mock",
            },
            "T03.sampler-metadata": {
                "sampler-metadata": "host-mock",
            },
            "T03.sampler-coordinates": {
                "sampler-coordinates-reference": "gpu-reference",
                "sampler-coordinates-bundle": "gpu-candidate",
            },
            "T03.sampler-coordinates-capture": {
                "sampler-coordinates-reference-capture": "capture-only",
                "sampler-coordinates-bundle-capture": "capture-only",
            },
            "T03.sampler-coordinate-support-host": {
                "sampler-coordinate-runtime": "host-mock",
                "sampler-coordinate-metadata": "host-mock",
                "sampler-coordinate-state": "host-cpu",
            },
            "T02.quad-margin-implicit": {
                "quad-margin-implicit-reference": "gpu-reference",
                "quad-margin-implicit-bundle": "gpu-candidate",
            },
            "T02.quad-margin-bias1": {
                "quad-margin-bias1-reference": "gpu-reference",
                "quad-margin-bias1-bundle": "gpu-candidate",
            },
            "T02.quad-margin-gradient": {
                "quad-margin-gradient-reference": "gpu-reference",
                "quad-margin-gradient-bundle": "gpu-candidate",
            },
            "T03.sampler-binding-state": {
                "sampler-binding-state": "host-mock",
            },
            "T03.sampler-binding-oracle-host": {
                "sampler-binding-oracle": "host-cpu",
            },
            "A04.argument-scan-runtime-host": {
                "argument-scan-runtime": "host-mock",
            },
            "T03.reflection-trim-abi8": {
                "reflection-trim-abi8": "offline-metadata",
            },
        }
        for case_id, names in expected.items():
            with self.subTest(case_id=case_id):
                selection = runner.Tier2Selection(manifest, [case_id])
                for name, evidence_class in names.items():
                    self.assertEqual(
                        selection.evidence_class(name), evidence_class)
                self.assertEqual(selection.missing(), {})

    def test_metadata_samples_are_required_before_platform_checks(self):
        with tempfile.TemporaryDirectory(dir=os.environ.get("TMPDIR")) as directory:
            output = Path(directory) / "must-not-exist"
            base = [
                sys.executable, runner.__file__, "vm", "/missing/bundle",
                "/missing/include", str(output), "--case",
                "T03.sampler-metadata"]
            for extra, message in (
                    ([], "--metadata-samples DIRECTORY is required"),
                    (["--metadata-samples", str(Path(directory) / "missing")],
                     "is not a readable directory")):
                with self.subTest(extra=extra):
                    result = subprocess.run(
                        base + extra, stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE, text=True)
                    self.assertEqual(result.returncode, 2)
                    self.assertIn(message, result.stderr)
                    self.assertNotIn("run as root on macOS", result.stderr)
                    self.assertFalse(output.exists())

    def test_runner_requires_all_declared_markers_and_records_evidence(self):
        with tempfile.TemporaryDirectory(dir=os.environ.get("TMPDIR")) as directory:
            out = Path(directory)
            (out / "results.tsv").write_text(
                "case\tstatus\texit\tlog\tevidence_class\n")
            script = "print('EXPECTED_PASS'); print('required=mock')"
            self.assertTrue(
                runner.run_case(
                    out, "required-present", [sys.executable, "-c", script],
                    "EXPECTED_PASS", evidence_class="host-mock",
                    required=("required=mock",)))
            self.assertFalse(
                runner.run_case(
                    out, "required-missing", [sys.executable, "-c", script],
                    "EXPECTED_PASS", evidence_class="host-mock",
                    required=("required=missing",)))
            results = (out / "results.tsv").read_text()
            self.assertIn("required-present\tPASS\t0\t", results)
            self.assertIn("host-mock\n", results)

    def test_capture_only_fixture_markers_declare_no_gpu_execution(self):
        source_dir = Path(runner.__file__).parent
        required = {
            ("test-argument-depth-sampler.m", "ARGUMENT_DEPTH_SAMPLER_CAPTURE"),
            ("test-argument-integer-gather.m", "ARGUMENT_INTEGER_GATHER_CAPTURE_PASS"),
            ("test-argument-nested.m", "ARGUMENT_NESTED_CAPTURE"),
            ("test-argument-null.m", "ARGUMENT_NULL_CAPTURE"),
            ("test-argument-quad-lod.m", "ARGUMENT_QUAD_LOD_CAPTURE"),
            ("test-argument-quad-margin.m", "ARGUMENT_QUAD_MARGIN_CAPTURE"),
            ("test-sampler-compare-functions.m", "ARGUMENT_SAMPLER_COMPARE_CAPTURE"),
            ("test-sampler-coordinates.m", "ARGUMENT_SAMPLER_COORDINATES_CAPTURE"),
            ("test-sampler-usage.m", "ARGUMENT_SAMPLER_USAGE_CAPTURE"),
        }
        checked = set()
        for source in sorted(source_dir.glob("*.m")):
            text = source.read_text()
            if "--capture-only" not in text:
                continue
            markers = list(re.finditer(
                r"\b[A-Z][A-Z0-9_]*_CAPTURE(?:_PASS)?\b", text))
            self.assertTrue(
                markers, f"{source.name}: capture-only has no CAPTURE marker")
            for match in markers:
                marker = match.group()
                start = max(
                    text.rfind("printf(", 0, match.start()),
                    text.rfind("puts(", 0, match.start()))
                self.assertNotEqual(
                    start, -1, f"{source.name}: {marker} has no output call")
                end = text.find(";", match.end())
                self.assertNotEqual(
                    end, -1, f"{source.name}: unterminated {marker}")
                statement = text[start:end]
                literals = re.findall(r'"(?:\\.|[^"\\])*"', statement)
                combined = "".join(ast.literal_eval(lit) for lit in literals)
                self.assertIn(marker, combined)
                self.assertIn(
                    "gpu_execution=0", combined,
                    f"{source.name}: {marker} must declare no GPU execution")
                checked.add((source.name, marker))
        self.assertTrue(
            required <= checked,
            f"capture-only fixtures not checked: {sorted(required - checked)}")

    def test_tier2_manifest_rejects_incomplete_case(self):
        _, manifest = runner.load_tier2_manifest()
        broken = copy.deepcopy(manifest)
        del broken["cases"][0]["oracle"]
        with self.assertRaisesRegex(ValueError, "missing oracle"):
            runner.validate_tier2_manifest(broken)
        broken = copy.deepcopy(manifest)
        broken["cases"][0]["backend_gap"] = ""
        with self.assertRaisesRegex(ValueError, "backend_gap must be a non-empty"):
            runner.validate_tier2_manifest(broken)
        broken = copy.deepcopy(manifest)
        broken["cases"][0]["runner_group"] = "missing-group"
        with self.assertRaisesRegex(ValueError, "unknown runner_group"):
            runner.validate_tier2_manifest(broken)

    def test_tier2_manifest_rejects_mislabeled_evidence(self):
        _, manifest = runner.load_tier2_manifest()
        broken = copy.deepcopy(manifest)
        broken["cases"][0]["candidate"]["execution_class"] = "gpu-reference"
        with self.assertRaisesRegex(ValueError, "cannot be gpu-reference"):
            runner.validate_tier2_manifest(broken)
        broken = copy.deepcopy(manifest)
        broken["cases"][0]["reference"]["execution_class"] = "unknown"
        with self.assertRaisesRegex(ValueError, "execution_class must be one of"):
            runner.validate_tier2_manifest(broken)

    def test_tier2_unknown_case_fails_before_metal_access(self):
        with tempfile.TemporaryDirectory(dir=os.environ.get("TMPDIR")) as directory:
            output = Path(directory) / "must-not-exist"
            result = subprocess.run(
                [sys.executable, runner.__file__, "vm", "/missing/bundle", "/missing/include",
                 str(output), "--case", "not-a-case"],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            self.assertEqual(result.returncode, 2)
            self.assertIn("unknown tier2 case", result.stderr)
            self.assertFalse(output.exists())

    def test_vm_rejects_large_memory_before_opening_bundle(self):
        with tempfile.TemporaryDirectory(dir=os.environ.get("TMPDIR")) as directory:
            output = Path(directory) / "must-not-exist"
            for mode in ("private", "shared", "managed", "nocopy-total"):
                with self.subTest(mode=mode):
                    result = subprocess.run(
                        [sys.executable, runner.__file__, "vm", "/missing/bundle", "/missing/include",
                         str(output), "--only", "memory", "--memory-mode", mode],
                        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                    self.assertEqual(result.returncode, 2)
                    self.assertIn("large memory modes require live mode", result.stderr)
                    self.assertFalse(output.exists())

    def test_verdicts(self):
        with tempfile.TemporaryDirectory(dir=os.environ.get("TMPDIR")) as directory:
            out = Path(directory)
            (out / "results.tsv").write_text("case\tstatus\texit\tlog\n")
            cases = [
                ("pass", "print('EXPECTED_PASS')", True, {}),
                ("child-failure", "print('EXPECTED_PASS'); raise SystemExit(7)", False, {}),
                ("missing-output", "print('unrelated')", False, {}),
                ("marker-in-command-only", "# EXPECTED_PASS\nprint('unrelated')", False, {}),
                ("skip", "print('ARCHIVE_SKIP missing feature'); print('EXPECTED_PASS')", False, {}),
                ("timeout", "import time; time.sleep(30)", False, {"timeout": 0.05}),
                ("build", "", True, {"build": True}),
                ("build-failure", "raise SystemExit(2)", False, {"build": True}),
                ("capture", "print('EXPECTED_PASS'); print('gpu_execution=0')", True, {"capture": True}),
                ("capture-missing-output", "print('unrelated')", False, {"capture": True}),
                ("capture-gpu-execution", "print('EXPECTED_PASS'); print('gpu_execution=1')", False, {"capture": True}),
                ("capture-mixed-gpu-execution", "print('EXPECTED_PASS'); print('gpu_execution=0'); print('gpu_execution=1')", False, {"capture": True}),
                ("capture-missing-gpu-execution", "print('EXPECTED_PASS')", False, {"capture": True}),
                ("capture-gpu-in-command-only", "# gpu_execution=0\nprint('EXPECTED_PASS')", False, {"capture": True}),
                ("live-missing-handshake", "print('EXPECTED_PASS')", False, {"live": True}),
                ("live-handshake-in-command-only", "# radv-darwin: native kext: ABI 1.0\nprint('EXPECTED_PASS')", False, {"live": True}),
                ("live-mock", "print('radv-darwin: native kext: ABI 1.0'); print('vm-mock EXPECTED_PASS')", False, {"live": True}),
                ("live", "print('radv-darwin: native kext: ABI 1.0'); print('EXPECTED_PASS')", True, {"live": True}),
            ]
            for name, script, expected, kwargs in cases:
                with self.subTest(name=name):
                    self.assertEqual(runner.run_case(out, name, [sys.executable, "-c", script],
                                                     "EXPECTED_PASS", **kwargs), expected)
            self.assertIn("skip\tSKIP\t0", (out / "results.tsv").read_text())
            self.assertIn("timeout\tFAIL\t124", (out / "results.tsv").read_text())
            self.assertIn("build\tBUILD\t0", (out / "results.tsv").read_text())
            self.assertIn("build-failure\tFAIL\t2", (out / "results.tsv").read_text())
            self.assertIn("capture\tCAPTURE\t0", (out / "results.tsv").read_text())
            self.assertIn("capture-missing-output\tFAIL\t0", (out / "results.tsv").read_text())
            self.assertIn("capture-gpu-execution\tFAIL\t0", (out / "results.tsv").read_text())
            self.assertIn("capture-mixed-gpu-execution\tFAIL\t0",
                          (out / "results.tsv").read_text())
            self.assertIn("capture-missing-gpu-execution\tFAIL\t0",
                          (out / "results.tsv").read_text())
            self.assertIn("capture-gpu-in-command-only\tFAIL\t0",
                          (out / "results.tsv").read_text())
            counts = runner.summarize_results(out / "results.tsv")
            self.assertEqual(counts["PASS"], 2)
            self.assertEqual(counts["CAPTURE"], 1)
            self.assertEqual(counts["BUILD"], 1)
            self.assertEqual(counts["SKIP"], 1)


if __name__ == "__main__":
    unittest.main()
