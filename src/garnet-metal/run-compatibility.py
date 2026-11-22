#!/usr/bin/env python3
"""Run the existing Metal compatibility tests serially and preserve each verdict."""

import argparse
import fnmatch
import hashlib
import json
import math
import os
from pathlib import Path
import re
import signal
import subprocess
import sys


TIER2_MANIFEST = Path(__file__).with_name("tier2-cases.json")
TIER2_STAGES = {"compute", "vertex", "fragment", "host", "offline"}
TIER2_EVIDENCE_CLASSES = {
    "gpu-reference",
    "gpu-candidate",
    "host-mock",
    "host-cpu",
    "layout-only",
    "capture-only",
    "offline-metadata",
    "compile-only",
    "not-applicable",
}
TIER2_RUNNER_GROUPS = (
    "sampler",
    "sampler-scope",
    "sampler-retire",
    "writer-epochs",
    "sampler-usage",
    "sampler-compare",
    "sampler-api-reject",
    "sampler-metadata",
    "sampler-coordinates",
    "sampler-binding",
    "argument-scan-runtime",
    "private-summary",
    "summary-event-progress",
    "submit-faults",
    "private-retire",
    "segment-retire",
    "quad-margin",
    "argument-fragment-flow",
    "argument-sampling-matrix",
    "argument-sampling-array",
    "argument-depth-mips",
    "argument-sampling-limits",
    "reflection-trim",
    "summary-plan",
    "summary-owners",
    "summary-runtime",
    "writer-epochs-host",
    "argument-capacity",
    "argument-public-sampler",
    "argument-pool",
    "argument-limit-snapshot",
    "argument-limit-snapshot-device",
    "snapshot-admission",
    "argument-scan-host",
    "gather-capability",
    "null-runtime",
    "null-shape",
    "null-shape-reflection",
    "null-remap",
    "archive",
    "arguments",
    "argument-arrays",
    "argument-contract",
    "argument-resources",
    "argument-gpu",
    "argument-copy",
    "argument-nested",
    "argument-integer-gather",
    "argument-gather-cube",
    "argument-image-operations",
    "sampler-patterns",
    "argument-depth-sampler",
    "argument-quad-lod",
    "intclear",
    "reflection",
    "stitching",
    "renderbox",
    "nocopy",
    "memory",
    "large-buffer-addressing",
    "large-memory-over4g",
)


def validate_tier2_manifest(manifest):
    if manifest.get("schema_version") != 1:
        raise ValueError("tier2 manifest schema_version must be 1")
    if not isinstance(manifest.get("language"), str) or not manifest["language"]:
        raise ValueError("tier2 manifest language must be a non-empty string")
    cases = manifest.get("cases")
    if not isinstance(cases, list) or not cases:
        raise ValueError("tier2 manifest cases must be a non-empty list")
    seen = set()
    for case in cases:
        required = ("id", "requirements", "matrix", "stage", "storage", "operation",
                    "oracle", "runner_group", "reference", "candidate", "backend_gap")
        missing = [key for key in required if key not in case]
        if missing:
            raise ValueError("tier2 case is missing " + ", ".join(missing))
        if not isinstance(case["id"], str) or not case["id"]:
            raise ValueError("tier2 case id must be a non-empty string")
        if case["id"] in seen:
            raise ValueError("duplicate tier2 case id: " + case["id"])
        seen.add(case["id"])
        for key in ("requirements", "matrix", "stage", "storage", "operation"):
            if (not isinstance(case[key], list) or not case[key] or
                    not all(isinstance(value, str) and value for value in case[key])):
                raise ValueError(f"{case['id']} {key} must be a non-empty string list")
        unknown_stages = set(case["stage"]) - TIER2_STAGES
        if unknown_stages:
            raise ValueError(f"{case['id']} has unknown stages: {sorted(unknown_stages)}")
        if not isinstance(case["oracle"], str) or not case["oracle"]:
            raise ValueError(case["id"] + " oracle must be a non-empty string")
        if not isinstance(case["runner_group"], str) or not case["runner_group"]:
            raise ValueError(case["id"] + " runner_group must be a non-empty string")
        if case["runner_group"] not in TIER2_RUNNER_GROUPS:
            raise ValueError(
                f"{case['id']} has unknown runner_group: {case['runner_group']}")
        if not isinstance(case["backend_gap"], str) or not case["backend_gap"]:
            raise ValueError(case["id"] + " backend_gap must be a non-empty string")
        matched = False
        for side in ("reference", "candidate"):
            entry = case[side]
            if not isinstance(entry, dict):
                raise ValueError(f"{case['id']} {side} must be an object")
            if not isinstance(entry.get("domain"), str) or not entry["domain"]:
                raise ValueError(f"{case['id']} {side}.domain must be a non-empty string")
            execution_class = entry.get("execution_class")
            if execution_class not in TIER2_EVIDENCE_CLASSES:
                raise ValueError(
                    f"{case['id']} {side}.execution_class must be one of "
                    + ", ".join(sorted(TIER2_EVIDENCE_CLASSES)))
            forbidden_class = (
                "gpu-candidate" if side == "reference" else "gpu-reference")
            if execution_class == forbidden_class:
                raise ValueError(
                    f"{case['id']} {side}.execution_class cannot be "
                    f"{forbidden_class}")
            patterns = entry.get("runner_cases")
            if (not isinstance(patterns, list) or
                    not all(isinstance(pattern, str) and pattern for pattern in patterns)):
                raise ValueError(f"{case['id']} {side}.runner_cases must be a string list")
            if patterns and execution_class == "not-applicable":
                raise ValueError(
                    f"{case['id']} {side} cannot select runner cases as not-applicable")
            if not patterns and execution_class != "not-applicable":
                raise ValueError(
                    f"{case['id']} {side} without runner cases must be not-applicable")
            overrides = entry.get("runner_evidence", {})
            if not isinstance(overrides, dict):
                raise ValueError(
                    f"{case['id']} {side}.runner_evidence must be an object")
            unknown_override = sorted(set(overrides) - set(patterns))
            if unknown_override:
                raise ValueError(
                    f"{case['id']} {side}.runner_evidence names unknown runner cases: "
                    + ", ".join(unknown_override))
            for pattern, evidence_class in overrides.items():
                if evidence_class not in TIER2_EVIDENCE_CLASSES:
                    raise ValueError(
                        f"{case['id']} {side}.runner_evidence[{pattern!r}] "
                        "has an unknown execution class")
                if evidence_class == "not-applicable":
                    raise ValueError(
                        f"{case['id']} {side}.runner_evidence[{pattern!r}] "
                        "cannot be not-applicable")
                if evidence_class == forbidden_class:
                    raise ValueError(
                        f"{case['id']} {side}.runner_evidence[{pattern!r}] "
                        f"cannot be {forbidden_class}")
            matched = matched or bool(patterns)
        if not matched:
            raise ValueError(case["id"] + " selects no runner cases")


def load_tier2_manifest(path=TIER2_MANIFEST):
    path = Path(path)
    manifest = json.loads(path.read_text())
    validate_tier2_manifest(manifest)
    return path, manifest


class Tier2Selection:
    def __init__(self, manifest, case_ids):
        by_id = {case["id"]: case for case in manifest["cases"]}
        unknown = sorted(set(case_ids) - set(by_id))
        if unknown:
            raise ValueError("unknown tier2 case(s): " + ", ".join(unknown))
        self.case_ids = list(dict.fromkeys(case_ids))
        self.cases = [by_id[case_id] for case_id in self.case_ids]
        self.required = {}
        self.required_evidence = {}
        for case in self.cases:
            for side in ("reference", "candidate"):
                entry = case[side]
                patterns = entry["runner_cases"]
                if patterns:
                    self.required[(case["id"], side)] = patterns
                    overrides = entry.get("runner_evidence", {})
                    self.required_evidence[(case["id"], side)] = [
                        (pattern, overrides.get(
                            pattern, entry["execution_class"]))
                        for pattern in patterns
                    ]
        self.matched = {key: [] for key in self.required}
        self.matched_evidence = {}

    @property
    def runner_groups(self):
        return list(dict.fromkeys(case["runner_group"] for case in self.cases))

    def evidence_class(self, name):
        matching = []
        for key, classified_patterns in self.required_evidence.items():
            for pattern, evidence_class in classified_patterns:
                if fnmatch.fnmatchcase(name, pattern):
                    matching.append((key, evidence_class))
        if not matching:
            return None
        classes = {evidence_class for _, evidence_class in matching}
        if len(classes) != 1:
            raise ValueError(
                f"runner case {name!r} has conflicting evidence classes: "
                + ", ".join(sorted(classes)))
        for key, _ in matching:
            if name not in self.matched[key]:
                self.matched[key].append(name)
        self.matched_evidence[name] = next(iter(classes))
        return next(iter(classes))

    def matches(self, name):
        return self.evidence_class(name) is not None

    def missing(self):
        missing = {}
        for key, patterns in self.required.items():
            unmatched = [pattern for pattern in patterns
                         if not any(fnmatch.fnmatchcase(name, pattern)
                                    for name in self.matched[key])]
            if unmatched:
                missing[key] = unmatched
        return missing

    def report(self):
        return {
            "selected_cases": self.case_ids,
            "runner_groups": self.runner_groups,
            "required_evidence": {
                f"{case_id}:{side}": [
                    {"runner_case": pattern, "execution_class": evidence_class}
                    for pattern, evidence_class in classified_patterns
                ]
                for (case_id, side), classified_patterns
                in self.required_evidence.items()
            },
            "matched": {f"{case_id}:{side}": names
                        for (case_id, side), names in self.matched.items()},
            "matched_evidence": self.matched_evidence,
            "missing": {f"{case_id}:{side}": patterns
                        for (case_id, side), patterns in self.missing().items()},
        }


def run_case(out, name, command, marker, *, timeout=180, env=None, live=False,
             capture=False, build=False, evidence_class="unclassified", required=()):
    log = out / (name + ".log")
    with log.open("w") as stream:
        stream.write("command=" + json.dumps([str(p) for p in command]) + "\n")
        stream.flush()
        process = subprocess.Popen(command, stdout=stream, stderr=subprocess.STDOUT,
                                   env=env, start_new_session=True)
        try:
            code = process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait()
            code = 124
            stream.write("VALIDATION_TIMEOUT\n")
    text = log.read_text(errors="replace").partition("\n")[2]
    skipped = bool(re.search(r"(?m)^[A-Z][A-Z0-9_]*_SKIP\b", text))
    backend_ok = not live or "radv-darwin: native kext: ABI " in text
    if live and re.search(r"(?i)vm-mock|moltenvk|apple paravirtual|lavapipe|llvmpipe", text):
        backend_ok = False
    gpu_execution = re.findall(r"\bgpu_execution\s*=\s*([^\s]+)", text)
    capture_ok = bool(gpu_execution) and all(value == "0" for value in gpu_execution)
    required_ok = all(value in text for value in required)
    passed = code == 0 and marker in text and required_ok and backend_ok
    if build and code == 0:
        status = "BUILD"
    elif skipped and code == 0:
        status = "SKIP"
    elif passed and capture and capture_ok:
        status = "CAPTURE"
    elif passed and not capture:
        status = "PASS"
    else:
        status = "FAIL"
    with (out / "results.tsv").open("a") as stream:
        stream.write(
            f"{name}\t{status}\t{code}\t{log.name}\t{evidence_class}\n")
    print(f"COMPATIBILITY_{status} {name} evidence={evidence_class} "
          f"exit={code} log={log}", flush=True)
    return status in ("PASS", "CAPTURE", "BUILD")


def summarize_results(path):
    counts = {status: 0 for status in ("PASS", "CAPTURE", "BUILD", "SKIP", "FAIL")}
    lines = Path(path).read_text().splitlines()
    if lines and lines[0].split("\t")[:2] == ["case", "status"]:
        lines = lines[1:]
    for line in lines:
        fields = line.split("\t")
        if len(fields) >= 2 and fields[1] in counts:
            counts[fields[1]] += 1
    return counts


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", nargs="?", choices=("vm", "live"))
    parser.add_argument("bundle", nargs="?", type=Path)
    parser.add_argument("vulkan_include", nargs="?", type=Path)
    parser.add_argument("output", nargs="?", type=Path)
    parser.add_argument("--only", choices=TIER2_RUNNER_GROUPS,
                        action="append", help="Limit the run to selected tests; memory requires explicit selection.")
    parser.add_argument("--list-cases", action="store_true",
                        help="List the Tier 2 execution manifest without running a GPU test.")
    parser.add_argument("--case", dest="tier2_case", action="append", default=[],
                        metavar="ID",
                        help="Run one Tier 2 manifest case (repeatable); selects its runner group.")
    parser.add_argument("--memory-mode", choices=("small", "private", "shared", "managed", "nocopy-total"),
                        default="small", help="Memory workload; large modes require live mode.")
    parser.add_argument("--memory-iterations", type=int, choices=range(1, 17), default=3,
                        metavar="1..16", help="Allocation/execution/release cycles for the memory group (default 3).")
    parser.add_argument("--metadata-samples", type=Path,
                        help="Required sample directory for the sampler-metadata case.")
    parser.add_argument("--sampling-bias-limit", type=float,
                        help="Queried backend maxSamplerLodBias for the sampling-limits oracle.")
    args = parser.parse_args()
    try:
        manifest_path, manifest = load_tier2_manifest()
    except (OSError, ValueError, json.JSONDecodeError) as error:
        parser.error(f"invalid Tier 2 manifest: {error}")
    if args.list_cases:
        print(json.dumps({
            "manifest": str(manifest_path),
            "sha256": hashlib.sha256(manifest_path.read_bytes()).hexdigest(),
            "cases": manifest["cases"],
        }, indent=2))
        return 0
    if any(value is None for value in (args.mode, args.bundle, args.vulkan_include, args.output)):
        parser.error("mode, bundle, vulkan_include and output are required")
    try:
        tier2_selection = Tier2Selection(manifest, args.tier2_case) if args.tier2_case else None
    except ValueError as error:
        parser.error(str(error))
    if args.mode == "vm" and args.memory_mode != "small":
        parser.error("large memory modes require live mode")
    common = ["/usr/bin/clang", "-arch", "x86_64", "-fobjc-arc", "-Wall", "-Wextra", "-Werror",
              "-framework", "Foundation", "-framework", "Metal"]
    if tier2_selection:
        selected_groups = tier2_selection.runner_groups
        if args.only:
            unsupported = sorted(set(selected_groups) - set(args.only))
            if unsupported:
                parser.error("--case group(s) not selected by --only: " + ", ".join(unsupported))
            groups = args.only
        else:
            groups = selected_groups
    else:
        groups = args.only or ["sampler", "archive", "arguments", "reflection", "stitching", "renderbox", "nocopy"]
    if "argument-sampling-limits" in groups:
        if (args.sampling_bias_limit is None or not math.isfinite(args.sampling_bias_limit) or
                not 0 <= args.sampling_bias_limit <= 120):
            parser.error("sampling-limits requires --sampling-bias-limit from the backend query (finite, 0..120 for finite Grad controls)")
        args.sampling_bias_limit = abs(args.sampling_bias_limit)
    target_groups = {"large-buffer-addressing", "large-memory-over4g"}.intersection(groups)
    if target_groups and args.mode != "live":
        parser.error(", ".join(sorted(target_groups)) +
                     " requires explicit live mode on target hardware")
    metadata_samples = None
    if "sampler-metadata" in groups:
        if args.metadata_samples is None:
            parser.error(
                "--metadata-samples DIRECTORY is required for sampler-metadata")
        metadata_samples = args.metadata_samples.resolve()
        if not metadata_samples.is_dir():
            parser.error(
                f"--metadata-samples is not a readable directory: {metadata_samples}")
    elif args.metadata_samples is not None:
        metadata_samples = args.metadata_samples.resolve()
        if not metadata_samples.is_dir():
            parser.error(
                f"--metadata-samples is not a readable directory: {metadata_samples}")
    if sys.platform != "darwin" or os.geteuid() != 0:
        parser.error("run as root on macOS; use vm mode only inside the isolated VM")
    features = subprocess.check_output(["/usr/sbin/sysctl", "-n", "machdep.cpu.features"], text=True).split()
    if ("VMM" in features) != (args.mode == "vm"):
        parser.error("requested mode does not match the machine")
    if args.mode == "live" and os.environ.get("RADV_DARWIN_MOCK", "0") != "0":
        parser.error("live mode refuses RADV_DARWIN_MOCK")
    source = Path(__file__).resolve().parent
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    bundle = args.bundle.resolve(strict=True)
    include = args.vulkan_include.resolve(strict=True)
    (out / "results.tsv").write_text(
        "case\tstatus\texit\tlog\tevidence_class\n")
    metadata = {
        "mode": args.mode,
        "os": subprocess.check_output(["/usr/bin/sw_vers"], text=True),
        "bundle": str(bundle),
        "tier2_manifest": {
            "path": str(manifest_path),
            "sha256": hashlib.sha256(manifest_path.read_bytes()).hexdigest(),
            "selected_cases": tier2_selection.case_ids if tier2_selection else [],
        },
        "source_sha256": {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                          for p in source.iterdir() if p.suffix in (".m", ".c", ".h", ".inc", ".py")},
        "fixture_sha256": {
            str(p.relative_to(source)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in (source / "fixtures").rglob("*") if p.is_file()
        },
        "bundle_sha256": {str(p.relative_to(bundle)): hashlib.sha256(p.read_bytes()).hexdigest()
                          for p in bundle.rglob("*") if p.is_file()},
    }
    if metadata_samples is not None:
        metadata["metadata_samples"] = {
            "path": str(metadata_samples),
            "sha256": {
                str(path.relative_to(metadata_samples)):
                    hashlib.sha256(path.read_bytes()).hexdigest()
                for path in metadata_samples.rglob("*") if path.is_file()
            },
        }
    if "argument-sampling-limits" in groups:
        metadata["sampling_bias_limit"] = args.sampling_bias_limit
    (out / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    env = os.environ.copy()
    env.update(RDNA4M_ALLOW="1", RDNA4M_TEST_INPROC="1", MTL_ERROR_MODE="assert")
    if args.mode == "vm":
        env.update(RADV_DARWIN_MOCK="1", RADV_DARWIN_MOCK_GC="gfx1200")
    failures = 0

    def case(name, command, marker, native=False, timeout=180, case_env=None,
             capture=False, evidence_class=None, required=()):
        nonlocal failures
        if tier2_selection:
            manifest_class = tier2_selection.evidence_class(name)
            if manifest_class is None:
                return
            if evidence_class is not None and evidence_class != manifest_class:
                raise RuntimeError(
                    f"{name}: runner evidence {evidence_class} conflicts with "
                    f"manifest evidence {manifest_class}")
            evidence_class = manifest_class
        elif evidence_class is None:
            evidence_class = "unclassified"
        if not run_case(
                out, name, command, marker, timeout=timeout,
                env=env if case_env is None else case_env,
                live=native and args.mode == "live", capture=capture,
                evidence_class=evidence_class, required=required):
            failures += 1

    build_cache = {}

    def build(name, extra=()):
        if name in build_cache:
            return build_cache[name]
        executable = out / name
        command = common + list(extra) + [str(source / (name + ".m")), "-o", str(executable)]
        # A successful compiler has no required stdout marker.
        if not run_case(out, "build-" + name, command, "", env=env, build=True,
                        evidence_class="compile-only"):
            raise RuntimeError("compilation failed: " + name)
        build_cache[name] = str(executable)
        return build_cache[name]

    def build_c(name, suffix=".c", extra=()):
        executable = out / name
        command = ["/usr/bin/clang", "-arch", "x86_64", "-std=c11",
                   "-Wall", "-Wextra", "-Werror", "-I", str(source),
                   "-I", str(include)] + list(extra) + [
                       str(source / (name + suffix)), "-o", str(executable)]
        if not run_case(out, "build-" + name, command, "", env=env,
                        build=True, evidence_class="compile-only"):
            raise RuntimeError("compilation failed: " + name)
        return str(executable)

    for group in dict.fromkeys(groups):
        if group == "sampler":
            exe = build("test-compute-argbuf-sampler", ["-framework", "IOKit"])
            case("sampler-reference", [exe, "--reference"], "ARGBUF_SAMPLER_PASS")
            case("sampler-bundle", [exe, str(bundle), "--" + args.mode], "ARGBUF_SAMPLER_PASS", True)
        elif group == "sampler-usage":
            exe = build("test-sampler-usage")
            runtime = build("test-sampler-runtime",
                            ["-I", str(include), "-framework", "IOKit",
                             "-framework", "IOSurface"])
            case(
                "sampler-runtime", [runtime], "SAMPLER_RUNTIME_PASS",
                evidence_class="host-mock",
                required=("simulated_vulkan=1", "gpu_execution=0"))
            state = out / "test-sampler-state"
            state_build = ["/usr/bin/clang", "-std=c11", "-Wall", "-Wextra", "-Werror",
                           "-I", str(source), "-I", str(include),
                           str(source / "test-sampler-state.c"), "-o", str(state)]
            if not run_case(
                    out, "build-sampler-state", state_build, "", env=env,
                    build=True, evidence_class="compile-only"):
                raise RuntimeError("compilation failed: test-sampler-state")
            case(
                "sampler-state", [str(state)],
                "sampler-state: normal/compare representations",
                evidence_class="host-cpu")
            coordinate_runtime = build(
                "test-sampler-coordinate-runtime",
                ["-I", str(include), "-framework", "IOKit",
                 "-framework", "IOSurface"])
            case(
                "sampler-coordinate-runtime", [coordinate_runtime],
                "SAMPLER_COORDINATE_RUNTIME_PASS",
                evidence_class="host-mock",
                required=("simulated_vulkan=1", "gpu_execution=0"))
            coordinate_metadata = build(
                "test-sampler-coordinate-metadata",
                ["-I", str(include), "-framework", "IOKit",
                 "-framework", "IOSurface"])
            case(
                "sampler-coordinate-metadata", [coordinate_metadata],
                "SAMPLER_COORDINATE_METADATA_PASS",
                evidence_class="host-mock",
                required=(
                    "table=8",
                    "flags_tail=1",
                    "dimensions_offset=1",
                    "direct674=1",
                    "malformed_reject=1",
                    "legacy7=1",
                    "actual_fragment=1",
                    "simulated_vulkan=1",
                    "gpu_execution=0",
                ))
            coordinate_state = out / "test-sampler-coordinate-state"
            coordinate_state_build = [
                "/usr/bin/clang", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-I", str(source), "-I", str(include),
                str(source / "test-sampler-coordinate-state.c"),
                "-o", str(coordinate_state)]
            if not run_case(
                    out, "build-sampler-coordinate-state",
                    coordinate_state_build, "", env=env, build=True,
                    evidence_class="compile-only"):
                raise RuntimeError(
                    "compilation failed: test-sampler-coordinate-state")
            case(
                "sampler-coordinate-state", [str(coordinate_state)],
                "sampler-coordinate-state: pixel validation",
                evidence_class="host-cpu")
            for operation in ("direct", "constexpr-never", "constexpr-always",
                              "argument-ordinary", "argument-compare", "argument-mixed"):
                for target, label in (("--reference", "reference"), (str(bundle), "bundle")):
                    name = "sampler-usage-" + operation + "-" + label
                    case(name, [exe, target, str(out / name), "--case", operation],
                         "ARGUMENT_SAMPLER_USAGE_PASS", label == "bundle", timeout=120,
                         required=("case=" + operation,))
            for target, label in (("--reference", "reference"), (str(bundle), "bundle")):
                name = "sampler-usage-" + label
                command = [exe, target, str(out / name)]
                case(name, command, "ARGUMENT_SAMPLER_USAGE_PASS",
                     label == "bundle", timeout=120)
        elif group == "sampler-scope":
            ownership = build("test-argument-sampler-scope")
            case("sampler-scope-bundle", [ownership, str(bundle), str(out / "sampler-scope-bundle")],
                 "ARGUMENT_SAMPLER_SCOPE_PASS backend=bundle owner_assertions=asserted")
            rebind = build("test-argument-sampler-rebind")
            for target, label in (("--reference", "reference"), (str(bundle), "bundle")):
                case("sampler-rebind-" + label, [rebind, target, str(out / ("sampler-rebind-" + label))],
                     "ARGUMENT_SAMPLER_REBIND_PASS", label == "bundle", timeout=120)
        elif group == "sampler-retire":
            exe = build("test-argument-sampler-retire")
            for target, label in (("--reference", "reference"), (str(bundle), "bundle")):
                case("sampler-retire-" + label, [exe, target, str(out / ("sampler-retire-" + label))],
                     "ARGUMENT_SAMPLER_RETIRE_PASS", label == "bundle", timeout=120)
        elif group == "writer-epochs":
            exe = build("test-argument-sampler-retire")
            for target, label in (("--reference", "reference"), (str(bundle), "bundle")):
                name = "sampler-retire-writer-epochs-" + label
                case(
                    name,
                    [exe, target, str(out / name), "--only", "writer-epochs"],
                    "ARGUMENT_SAMPLER_WRITER_EPOCHS_PASS scan_budget=not_run",
                    native=label == "bundle",
                    timeout=120,
                    evidence_class=(
                        "gpu-reference" if label == "reference"
                        else "gpu-candidate"),
                    required=(
                        f"backend={'reference' if label == 'reference' else 'bundle'}",
                        "abandoned_concurrent_copy=1",
                        "abandoned_own_intent=1",
                        "pending_peer_preserved=1",
                    ))
        elif group == "sampler-compare":
            exe = build("test-sampler-compare-functions")
            for target, label in (("--reference", "reference"), (str(bundle), "bundle")):
                name = "sampler-compare-" + label
                evidence_class = (
                    "gpu-reference" if label == "reference"
                    else "gpu-candidate")
                case(
                    name,
                    [exe, target, str(out / name)],
                    f"ARGUMENT_SAMPLER_COMPARE_PASS backend={label}",
                    native=label == "bundle",
                    timeout=120,
                    evidence_class=evidence_class,
                    required=(
                        "functions=8",
                        "direct=8-runtime-samplers",
                        "constexpr=8-static-samplers",
                        "offset=256 guards=exact",
                    ))
                capture_name = name + "-capture"
                case(
                    capture_name,
                    [exe, target, str(out / capture_name), "--capture-only"],
                    f"ARGUMENT_SAMPLER_COMPARE_CAPTURE backend={label}",
                    native=label == "bundle",
                    timeout=120,
                    capture=True,
                    evidence_class="capture-only",
                    required=("air=2", "gpu_execution=0", "msl=2.1"))
        elif group == "sampler-api-reject":
            exe = build(
                "test-sampler-api-reject",
                ["-I", str(include), "-framework", "IOKit",
                 "-framework", "IOSurface"])
            case(
                "sampler-api-reject",
                [exe],
                "SAMPLER_API_REJECT_PASS invalid_enums=6 anisotropy_overflow=1 "
                "invalid_vk_calls=0 initial_failure_rollback=1 retry=1",
                evidence_class="host-mock",
                required=("simulated_vulkan=1", "gpu_execution=0"))
        elif group == "sampler-metadata":
            exe = build(
                "test-sampler-metadata",
                ["-I", str(include), "-framework", "IOKit",
                 "-framework", "IOSurface"])
            case(
                "sampler-metadata",
                [exe, str(metadata_samples)],
                "SAMPLER_METADATA_PASS actual_cases=5",
                evidence_class="host-mock",
                required=(
                    "gpu_portable_pair=1",
                    "direct_pair_strict=1",
                    "malformed_rejected=14",
                    "stage_metadata_words=96",
                    "simulated_vulkan=1",
                    "gpu_execution=0",
                ))
        elif group == "sampler-coordinates":
            exe = build("test-sampler-coordinates")
            for target, label in (("--reference", "reference"), (str(bundle), "bundle")):
                name = "sampler-coordinates-" + label
                evidence_class = (
                    "gpu-reference" if label == "reference"
                    else "gpu-candidate")
                case(
                    name,
                    [exe, target, str(out / name)],
                    f"ARGUMENT_SAMPLER_COORDINATES_PASS backend={label}",
                    native=label == "bundle",
                    timeout=120,
                    evidence_class=evidence_class,
                    required=(
                        "coordinates=10",
                        "ordinary=1 sample_compare=3 gather=1 gather_compare=3",
                        "cpu_oracle=1 guards=exact",
                        "reject=pixel-repeat",
                    ))
                capture_name = name + "-capture"
                case(
                    capture_name,
                    [exe, target, str(out / capture_name), "--capture-only"],
                    f"ARGUMENT_SAMPLER_COORDINATES_CAPTURE backend={label}",
                    native=label == "bundle",
                    timeout=120,
                    capture=True,
                    evidence_class="capture-only",
                    required=("air=4", "gpu_execution=0", "msl=2.1"))
        elif group == "sampler-binding":
            exe = build(
                "test-sampler-binding-state",
                ["-I", str(include), "-fblocks", "-O1",
                 "-framework", "IOKit",
                 "-framework", "IOSurface"])
            case(
                "sampler-binding-state",
                [exe],
                "SAMPLER_BINDING_STATE_PASS",
                evidence_class="host-mock",
                required=(
                    "binding_cases=",
                    "setter_cases=",
                    "mapping_cases=",
                    "wrapped_rejects=",
                    "oracle_cases=",
                    "simulated_vulkan=1",
                    "gpu_execution=0",
                ))
            oracle = out / "test-sampler-binding-state-oracle"
            oracle_build = common + [
                "-I", str(include), "-fblocks", "-O1",
                "-DRDNA4_SAMPLER_BINDING_ORACLE_ONLY",
                str(source / "test-sampler-binding-state.m"),
                "-o", str(oracle),
            ]
            if not run_case(
                    out, "build-sampler-binding-state-oracle",
                    oracle_build, "", env=env, build=True,
                    evidence_class="compile-only"):
                raise RuntimeError(
                    "compilation failed: test-sampler-binding-state oracle")
            case(
                "sampler-binding-oracle",
                [str(oracle)],
                "SAMPLER_BINDING_ORACLE_PASS",
                evidence_class="host-cpu",
                required=("oracle_cases=",))
        elif group == "argument-scan-runtime":
            exe = build(
                "test-argument-scan-runtime",
                ["-I", str(include), "-I", str(source), "-fblocks", "-O1",
                 "-framework", "IOKit", "-framework", "IOSurface"])
            case(
                "argument-scan-runtime",
                [exe],
                "ARGUMENT_SCAN_RUNTIME_PASS",
                evidence_class="host-mock",
                required=(
                    "shared_rounds=2",
                    "managed_rounds=2",
                    "large_sparse_tokens=2",
                    "unreflected_large_root=0",
                    "aliasable_raw_read=0",
                    "no_all_device_fallback=1",
                    "declared_child_cases=8",
                    "gpu_execution=0",
                ))
        elif group == "private-summary":
            exe = build("test-argument-private-summary")
            case(
                "private-summary-runtime",
                [exe, "--bundle", str(bundle)],
                "ARGUMENT_PRIVATE_SUMMARY_PASS rounds=10",
                native=True,
                timeout=360,
                evidence_class="gpu-candidate",
                required=(
                    "typed_tokens=2",
                    "transport_tokens=1",
                    "partial_overwrites=18",
                    "final_fill=1",
                    "command_history=11",
                    "weak_owners_retired=1",
                    "private_contents=null",
                    "backend=runtime-summary-gpu",
                    "gpu_execution=1",
                ))
        elif group == "summary-event-progress":
            exe = build("test-argument-summary-event-progress")
            case(
                "summary-event-progress", [exe, "--bundle", str(bundle)],
                "ARGUMENT_SUMMARY_EVENT_PROGRESS_PASS rounds=8",
                native=True, timeout=360, evidence_class="gpu-candidate",
                required=(
                    "source_released_before_gate=8", "writer_incomplete=8",
                    "owner_retirements=7", "producer_progress=8",
                    "same_cb_event_blocked=1", "independent_producer_queue=1",
                    "final_owner_live=0", "private_contents=null",
                    "backend=runtime-summary-gpu", "gpu_execution=1",
                ))
        elif group == "submit-faults":
            exe = build(
                "test-argument-submit-faults",
                ["-I", str(include), "-I", str(source), "-fblocks", "-O1",
                 "-framework", "IOKit", "-framework", "IOSurface"])
            case(
                "submit-faults",
                [exe],
                "ARGUMENT_SUBMIT_FAULTS_PASS",
                evidence_class="host-mock",
                required=(
                    "mock_vulkan=1",
                    "mock_metal_queue=1",
                    "gpu_execution=0",
                    "quarantined=1",
                ))
        elif group == "private-retire":
            exe = build("test-argument-private-retire")
            case(
                "private-retire-layout",
                [exe, "--layout-only", str(out / "private-retire-layout")],
                "ARGUMENT_PRIVATE_RETIRE_LAYOUT",
                evidence_class="layout-only",
                required=(
                    "samplers=2 texture=1 transport=separate",
                    "msl=2.1 gpu_execution=0 submissions=0",
                ))
            for target, label in (("--reference", "reference"), (str(bundle), "bundle")):
                name = "private-retire-" + label
                evidence_class = (
                    "gpu-reference" if label == "reference"
                    else "gpu-candidate")
                marker = (
                    "ARGUMENT_PRIVATE_RETIRE_REFERENCE_PASS"
                    if label == "reference"
                    else "ARGUMENT_PRIVATE_RETIRE_PASS")
                case(
                    name,
                    [exe, target, str(out / name)],
                    marker,
                    native=label == "bundle",
                    timeout=180,
                    evidence_class=evidence_class,
                    required=(
                        "rounds=2 typed_tokens=2 transport=separate",
                        "partial_overwrite=2",
                        "consumer_samples=6",
                        "gpu_execution=1",
                        f"backend={label}",
                    ))
        elif group == "segment-retire":
            exe = build(
                "test-argument-segment-retire",
                ["-I", str(include), "-framework", "IOKit",
                 "-framework", "IOSurface"])
            case(
                "segment-retire",
                [exe],
                "ARGUMENT_SEGMENT_RETIRE_PASS",
                evidence_class="host-mock",
                required=(
                    "mock_vulkan=1",
                    "mock_metal_lifecycle=1",
                    "gpu_execution=0",
                ))
        elif group == "argument-fragment-flow":
            exe = build("test-argument-fragment-flow")
            for flow in ("uniform-branch", "uniform-loop", "helper-calls",
                         "uniform-return", "uniform-discard", "varying-merge",
                         "quad-uniform-branch", "quad-uniform-loop",
                         "quad-uniform-branch-lane-resources",
                         "quad-uniform-loop-lane-resources",
                         "runtime-coordinate-branch", "runtime-coordinate-loop",
                         "runtime-array-coordinate-branch", "runtime-array-coordinate-loop",
                         "runtime-coordinate-return", "runtime-coordinate-discard",
                         "runtime-coordinate-continue", "runtime-coordinate-continue-grad",
                         "runtime-coordinate-affine", "runtime-coordinate-affine-grad",
                         "runtime-coordinate-math-loop", "runtime-coordinate-math-loop-grad",
                         "runtime-coordinate-buffer", "runtime-coordinate-buffer-grad",
                         "runtime-coordinate-buffer-helper", "runtime-coordinate-buffer-helper-grad",
                         "runtime-coordinate-guarded", "runtime-coordinate-guarded-grad",
                         "runtime-coordinate-guarded-null", "runtime-coordinate-dynamic",
                         "runtime-coordinate-dynamic-grad", "runtime-coordinate-dynamic-null",
                         "runtime-coordinate-effects", "runtime-coordinate-effects-grad", "runtime-coordinate-effects-null"):
                for target, label in (("--reference", "reference"),
                                      (str(bundle), "bundle")):
                    name = f"argument-fragment-flow-{flow}-{label}"
                    case(
                        name, [exe, target, str(out / name), flow],
                        f"ARGUMENT_FRAGMENT_FLOW_PASS backend={label}",
                        native=label == "bundle", timeout=120,
                        evidence_class=("gpu-reference" if label == "reference"
                                        else "gpu-candidate"),
                        required=(f"flow={flow}", "flows=1 draws=4", "cpu_oracle=1",
                                  "guards=target,argument,control", "gpu_execution=1"))
        elif group in ("argument-sampling-matrix", "argument-sampling-array"):
            exe = build("test-argument-sampling-matrix")
            array_shape = group == "argument-sampling-array"
            for shape in (("array-pot", "array-npot") if array_shape else ("pot", "npot")):
                for sampling in ("bias", "grad", "level"):
                    for bias in ((-2, 0, 2) if array_shape else (-2, -1, 0, 1, 2)):
                        for target, label in (("--reference", "reference"), (str(bundle), "bundle")):
                            name = f"argument-sampling-{shape}-{sampling}-{bias}-{label}"
                            case(name, [exe, target, str(out / name), shape, sampling, str(bias)],
                                 f"ARGUMENT_SAMPLING_MATRIX_PASS backend={label}",
                                 native=label == "bundle", timeout=120,
                                 evidence_class="gpu-reference" if label == "reference" else "gpu-candidate",
                                 required=(f"shape={shape}", f"mode={sampling}", f"bias={bias}", "draws=8",
                                           "cpu_oracle=1", "sampler_clamps=0:4,1:2", "nonzero_offsets=1",
                                           "guards=target,argument,control", "gpu_execution=1") +
                                          (("ARGUMENT_SAMPLING_ARRAY_PATTERN layers=1:2 offset=1:-1 spatial=1",)
                                           if array_shape else ()))
        elif group == "argument-depth-mips":
            exe = build("test-argument-depth-mips")
            for shape in ("pot", "npot", "array-pot", "array-npot"):
                for mode in ("implicit", "level0", "mixed"):
                    for target, label in (("--reference", "reference"), (str(bundle), "bundle")):
                        name = f"argument-depth-mips-{shape}-{mode}-{label}"
                        case(name, [exe, target, str(out / name), shape, mode],
                             f"ARGUMENT_DEPTH_MIPS_PASS backend={label}", native=label == "bundle", timeout=120,
                             evidence_class="gpu-reference" if label == "reference" else "gpu-candidate",
                             required=(f"shape={shape}", f"mode={mode}", "draws=8 msl=2.1 cpu_oracle=1",
                                       "compare=less,greater sampler_clamps=0:4,1:2 offset=1:-1",
                                       "layers=1:2" if shape.startswith("array-") else "layers=0",
                                       "guards=target,argument,control,upload gpu_execution=1") +
                                      (("ARGUMENT_DEPTH_MIXED_TOKEN ordinary=1 comparison=3 shared_dynamic_token=1",)
                                       if mode == "mixed" else ()))
        elif group == "argument-sampling-limits":
            exe = build("test-argument-sampling-matrix")
            limit = args.sampling_bias_limit
            values = (("positive-limit", limit), ("negative-limit", -limit),
                      ("positive-over", limit + 4), ("negative-over", -limit - 4))
            for shape in ("pot", "npot"):
                for sampling in ("bias", "grad"):
                    for limit_case, bias in values:
                        bias_text, limit_text = format(bias, ".9g"), format(limit, ".9g")
                        for target, label in (("--reference", "reference"), (str(bundle), "bundle")):
                            name = f"argument-sampling-{shape}-{sampling}-{limit_case}-{label}"
                            case(name, [exe, target, str(out / name), shape, sampling, bias_text, limit_text],
                                 f"ARGUMENT_SAMPLING_MATRIX_PASS backend={label}",
                                 native=label == "bundle", timeout=120,
                                 evidence_class="gpu-reference" if label == "reference" else "gpu-candidate",
                                 required=(f"shape={shape}", f"mode={sampling}", f"bias={bias_text}",
                                           f"bias_limit={limit_text}", "draws=8", "cpu_oracle=1",
                                           "sampler_clamps=0:4,1:2", "nonzero_offsets=1",
                                           "guards=target,argument,control", "gpu_execution=1"))
        elif group == "quad-margin":
            exe = build("test-argument-quad-margin")
            for sampling in ("implicit", "bias1", "gradient"):
                for target, label in (("--reference", "reference"),
                                      (str(bundle), "bundle")):
                    name = f"quad-margin-{sampling}-{label}"
                    evidence_class = (
                        "gpu-reference" if label == "reference"
                        else "gpu-candidate")
                    case(
                        name,
                        [exe, target, str(out / name),
                         "--sampling", sampling],
                        f"ARGUMENT_QUAD_MARGIN_PASS backend={label}",
                        native=label == "bundle",
                        timeout=120,
                        evidence_class=evidence_class,
                        required=(
                            f"sampling={sampling}",
                            "interior_pixels=16",
                            "guards=target,argument,control",
                        ))
            for mode in ("sampler", "image"):
                for target, label in (("--reference", "reference"),
                                      (str(bundle), "bundle")):
                    name = f"quad-margin-{mode}-implicit-{label}"
                    case(
                        name, [exe, target, str(out / name),
                               "--mode", mode, "--sampling", "implicit"],
                        f"ARGUMENT_QUAD_MARGIN_PASS backend={label}",
                        native=label == "bundle", timeout=120,
                        evidence_class=("gpu-reference" if label == "reference"
                                        else "gpu-candidate"),
                        required=("modes=1 samplings=1 flips=2 cases=2",
                                  "interior_pixels=16", "cpu_oracle=1",
                                  "guards=target,argument,control control=none"))
            for control in ("coordinate", "lod1"):
                control_counts = ("modes=1 samplings=1 flips=1 cases=1"
                                  if control == "coordinate" else
                                  "modes=3 samplings=1 flips=2 cases=6")
                for target, label in (("--reference", "reference"),
                                      (str(bundle), "bundle")):
                    name = f"quad-margin-control-{control}-{label}"
                    case(
                        name, [exe, target, str(out / name), "--control", control],
                        f"ARGUMENT_QUAD_MARGIN_PASS backend={label}",
                        native=label == "bundle", timeout=120,
                        evidence_class=("gpu-reference" if label == "reference"
                                        else "gpu-candidate"),
                        required=(control_counts,
                                  "interior_pixels=16", "cpu_oracle=1",
                                  "guards=target,argument,control",
                                  f"control={control}"))
        elif group in ("gather-capability", "null-runtime"):
            fixture = "test-" + group
            exe = build(
                fixture, ["-I", str(include), "-I", str(source), "-fblocks", "-O1",
                          "-framework", "IOKit", "-framework", "IOSurface"])
            case(group, [exe], group.upper().replace("-", "_") + "_PASS",
                 evidence_class="host-mock",
                 required=(("mock_vulkan=1", "gpu_execution=0")
                           if group == "gather-capability" else ()))
            if group == "gather-capability":
                cpu = build_c("test-gather-caps")
                case("gather-caps", [cpu], "17 checks, 0 failures",
                     evidence_class="host-cpu")
            else:
                dummy = build("test-null-sampler-bindings", ["-I", str(include),
                    "-framework", "IOKit", "-framework", "IOSurface"])
                case("null-sampler-bindings", [dummy], "NULL_SAMPLER_BINDINGS_PASS",
                     evidence_class="host-mock", required=("ordinary_comparison=1",
                         "allocation_failure_retry=1", "undefined_gpu_oracle=0", "gpu_execution=0"))
                texel = build("test-null-texel-dummies", ["-I", str(include),
                    "-framework", "IOKit", "-framework", "IOSurface"])
                case("null-texel-dummies", [texel], "NULL_TEXEL_DUMMIES_PASS",
                     evidence_class="host-mock", required=("shapes=6", "failures=42",
                         "zero_and_padding=1", "rollback_retry=1", "typed_buffer_views=1", "gpu_execution=0"))
                live = build("test-texture-buffer-runtime", ["-I", str(include),
                    "-framework", "IOKit", "-framework", "IOSurface"])
                case("texture-buffer-runtime", [live], "TEXTURE_BUFFER_RUNTIME_PASS",
                     evidence_class="host-mock", required=("gpu_execution=0",))
        elif group in ("null-shape", "null-shape-reflection", "null-remap"):
            exe = build_c("test-" + group)
            command = [exe]
            if group == "null-shape-reflection":
                command.extend(str(source / "fixtures" / (name + ".spv"))
                               for name in ("null-shapes-abi9", "null-shapes-invalid"))
            case(group, command, group.upper().replace("-", "_") + "_PASS",
                 evidence_class=("offline-metadata" if group == "null-shape-reflection"
                                 else "host-cpu"))
        elif group in ("summary-plan", "summary-owners", "summary-runtime",
                       "writer-epochs-host", "argument-capacity",
                       "argument-public-sampler", "argument-scan-host"):
            fixture = {
                "summary-plan": "argument-summary-plan",
                "summary-owners": "argument-summary-owners",
                "summary-runtime": "argument-summary-runtime",
                "writer-epochs-host": "argument-writer-epochs",
                "argument-scan-host": "argument-scan",
            }.get(group, group)
            exe = build_c("test-" + fixture,
                          suffix=".m" if group == "summary-runtime" else ".c",
                          extra=("-x", "c") if group == "summary-runtime" else ())
            marker = ("ARGUMENT_WRITER_EPOCH_PASS" if group == "writer-epochs-host"
                      else fixture.upper().replace("-", "_") + "_PASS")
            case(group, [exe], marker,
                 evidence_class="host-mock" if group == "summary-runtime" else "host-cpu")
        elif group == "argument-limit-snapshot":
            provider = build_c("test-argument-limit-snapshot-provider",
                               extra=("-dynamiclib",))
            exe = build_c("test-argument-limit-snapshot")
            case(group, [exe, provider], "ARGUMENT_LIMIT_SNAPSHOT_PASS",
                 evidence_class="host-mock",
                 required=("gpu_execution=0", "vulkan_object=0 native_open=0"))
        elif group == "argument-limit-snapshot-device":
            exe = build("test-argument-limit-snapshot-device")
            case(group, [exe], "ARGUMENT_LIMIT_SNAPSHOT_DEVICE_PASS",
                 evidence_class="host-mock",
                 required=("opens=0", "vulkan_object=0 native_open=0"))
        elif group in ("snapshot-admission", "argument-pool"):
            exe = build(
                "test-" + group,
                ["-I", str(include), "-I", str(source), "-fblocks", "-O1",
                 "-framework", "IOKit", "-framework", "IOSurface"])
            case(group, [exe], group.upper().replace("-", "_") + "_PASS",
                 evidence_class="host-mock", required=("gpu_execution=0",))
        elif group == "argument-gather-cube":
            exe = build("test-argument-gather-cube")
            for target, label in (("--reference", "reference"), (str(bundle), "bundle")):
                name = "argument-gather-cube-" + label
                case(
                    name, [exe, target, str(out / name)],
                    "ARGUMENT_GATHER_CUBE_PASS base_mip=1 cube=1 cube_array=1",
                    native=label == "bundle", timeout=120,
                    evidence_class=("gpu-reference" if label == "reference"
                                    else "gpu-candidate"),
                    required=(f"ARGUMENT_GATHER_CUBE_CASE_PASS backend={label}",
                              "argument_entries=11 argument_bytes=88",
                              "order=BL,BR,TR,TL", "dref_less=0,1",
                              "guards=arguments,float,int"))
        elif group == "sampler-patterns":
            exe = build("test-sampler-patterns")
            for label in ("reference", "bundle"):
                name = "sampler-patterns-" + label
                case(name, [exe, "--" + label,
                            "-" if label == "reference" else str(bundle), str(out / name)],
                     "SAMPLER_PATTERNS_PASS", label == "bundle", timeout=360,
                     evidence_class=("gpu-reference" if label == "reference" else "gpu-candidate"),
                     required=("patterns=28", "dynamic_and_direct=1", "nonzero_resource=1",
                               "guards=output,argument,parameters", "gpu_execution=1"))
                name = "sampler-anisotropy-" + label
                case(name, [exe, "--" + label,
                            "-" if label == "reference" else str(bundle), str(out / name),
                            "--anisotropy"], "SAMPLER_ANISOTROPY_PASS", label == "bundle",
                     timeout=120,
                     evidence_class=("gpu-reference" if label == "reference" else "gpu-candidate"),
                     required=("states=1,2,4,8,16", "resources=2", "dispatches=10",
                               "direct_argument_constexpr=1", "isotropic_control=1",
                               "stripe_recovery=1", "gpu_execution=1"))
        elif group == "argument-image-operations":
            binding = build("test-multisample-binding",
                            ["-I", str(include), "-framework", "IOKit", "-framework", "IOSurface"])
            case("multisample-binding-host", [binding], "MULTISAMPLE_BINDING_PASS",
                 evidence_class="host-mock", required=("matrix=12", "gpu_execution=0"))
            runtime = build("test-synthesized-read-sampler",
                            ["-I", str(include), "-framework", "IOKit",
                             "-framework", "IOSurface"])
            case("synthesized-read-sampler-host", [runtime], "READ_SAMPLER_PASS",
                 evidence_class="host-mock",
                 required=("explicit_mip=1", "fragment_relocation=1",
                           "missing_state_rejected=1", "gpu_execution=0"))
            exe = build("test-argument-image-operations")
            for shape in ("2d", "2d_array", "cube", "cube_array"):
                selected = "depth-" + shape + "-0"
                for comparison, code in (("never", 0), ("less", 1), ("always", 7)):
                    for label in ("reference", "bundle"):
                        name = "image-null-sampling-" + selected + "-" + comparison + "-" + label
                        case(name, [exe, "--" + label, "-" if label == "reference" else str(bundle),
                                    str(out / name), "--case", selected, "--null-sampling", comparison],
                             "ARGUMENT_NULL_GUARD_PASS", native=label == "bundle",
                             evidence_class="gpu-reference" if label == "reference" else "gpu-candidate",
                             required=("dispatches=8", "storage=shared,private", "null_members=0",
                                       "dual_use=1", f"compare={code}", "output_words=7", "gpu_execution=1"))
            for shape in ("1d", "1d_array", "2d", "2d_array", "3d", "cube", "cube_array", "2d_ms"):
                for depth in ((False, True) if shape not in ("1d", "1d_array", "3d") else (False,)):
                    for kind in range(1 if depth else 3):
                        selected = ("depth-" if depth else "color-") + shape + "-" + str(kind)
                        for label in ("reference", "bundle"):
                            name = "image-null-guard-" + selected + "-" + label
                            case(name, [exe, "--" + label, "-" if label == "reference" else str(bundle),
                                        str(out / name), "--case", selected, "--null-guard"],
                                 "ARGUMENT_NULL_GUARD_PASS", native=label == "bundle",
                                 evidence_class="gpu-reference" if label == "reference" else "gpu-candidate",
                                 required=("dispatches=8", "storage=shared,private", "null_members=0",
                                           "source_and_destination_opposite=1", "gpu_execution=1"))
            for selected in ("color-2d_ms-0", "color-2d_ms-1", "color-2d_ms-2", "depth-2d_ms-0"):
                for label in ("reference", "bundle"):
                    name = "argument-image-operations-" + selected + "-" + label
                    case(name, [exe, "--" + label, "-" if label == "reference" else str(bundle),
                                str(out / name), "--case", selected],
                         "ARGUMENT_IMAGE_OPERATIONS_PASS", native=label == "bundle",
                         evidence_class="gpu-reference" if label == "reference" else "gpu-candidate",
                         required=("operation=all", "cases=1 selections=2", "sample=2", "gpu_execution=1"))
                    name = "argument-image-direct-ms-read-" + selected + "-" + label
                    case(name, [exe, "--" + label, "-" if label == "reference" else str(bundle),
                                str(out / name), "--case", selected, "--direct-ms-read", "--operation", "read"],
                         "ARGUMENT_IMAGE_OPERATIONS_PASS", native=label == "bundle",
                         evidence_class="gpu-reference" if label == "reference" else "gpu-candidate",
                         required=("operation=read", "cases=1 selections=2", "sample=2",
                                   "direct_read=1", "gpu_execution=1"))
                    name = "argument-image-direct-ms-query-controls-" + selected + "-" + label
                    case(name, [exe, "--" + label, "-" if label == "reference" else str(bundle),
                                str(out / name), "--case", selected, "--direct-ms-read"],
                         "ARGUMENT_IMAGE_OPERATIONS_PASS", native=label == "bundle",
                         evidence_class="gpu-reference" if label == "reference" else "gpu-candidate",
                         required=("operation=all", "cases=1 selections=2", "sample=2",
                                   "direct_read=1", "gpu_execution=1"))
                    name = "argument-image-ms-query-only-" + selected + "-" + label
                    case(name, [exe, "--" + label, "-" if label == "reference" else str(bundle),
                                str(out / name), "--case", selected, "--operation", "query"],
                         "ARGUMENT_IMAGE_OPERATIONS_PASS", native=label == "bundle",
                         evidence_class="gpu-reference" if label == "reference" else "gpu-candidate",
                         required=("operation=query", "cases=1 selections=2", "gpu_execution=1"))
            for shape in ("cube", "cube_array"):
                selected = "depth-" + shape + "-0"
                name = "argument-image-operations-" + selected + "-bundle"
                case(name, [exe, "--bundle", str(bundle), str(out / name), "--case", selected],
                     "ARGUMENT_IMAGE_OPERATIONS_PASS", native=True, evidence_class="gpu-candidate",
                     required=("operation=all", "cases=1 selections=2", "cube_gradient=float3", "gpu_execution=1"))
                name = "argument-image-operations-" + selected + "-reference-capture"
                case(name, [exe, "--reference", "-", str(out / name), "--case", selected, "--compile-only"],
                     "ARGUMENT_IMAGE_OPERATIONS_CAPTURE", capture=True,
                     evidence_class="capture-only", required=("cases=1", "msl=2.1", "gpu_execution=0"))
                name = "argument-image-read-grid-" + selected + "-bundle"
                case(name, [exe, "--bundle", str(bundle), str(out / name),
                            "--case", selected, "--operation", "read-grid"],
                     "ARGUMENT_IMAGE_OPERATIONS_PASS", native=True,
                     evidence_class="gpu-candidate",
                     required=("operation=read-grid", "cases=1", "gpu_execution=1",
                               "ARGUMENT_CUBE_READS_PASS", "faces=6", "mips=2", "read_and_sample=1"))
                name = "argument-image-read-grid-" + selected + "-reference-capture"
                case(name, [exe, "--reference", "-", str(out / name), "--case", selected,
                            "--operation", "read-grid", "--compile-only"],
                     "ARGUMENT_IMAGE_OPERATIONS_CAPTURE", capture=True,
                     evidence_class="capture-only", required=("cases=1", "msl=2.1", "gpu_execution=0"))
            for shape in ("cube", "cube_array"):
                for kind in range(3):
                    selected = "color-" + shape + "-" + str(kind)
                    for operation in ("write", "read-grid"):
                        for label in ("reference", "bundle"):
                            name = "argument-image-" + operation + "-" + selected + "-" + label
                            case(name, [exe, "--" + label,
                                       "-" if label == "reference" else str(bundle), str(out / name),
                                       "--case", selected, "--operation", operation],
                                 "ARGUMENT_IMAGE_OPERATIONS_PASS", label == "bundle",
                                 required=("operation=" + operation, "cases=1", "gpu_execution=1") + (
                                     ("ARGUMENT_CUBE_READS_PASS", "faces=6", "mips=2", "read_and_sample=1")
                                     if operation == "read-grid" else ()))
            for label in ("reference", "bundle"):
                name = "argument-image-operations-" + label
                case(
                    name, [exe, "--" + label,
                           "-" if label == "reference" else str(bundle), str(out / name)],
                    f"ARGUMENT_IMAGE_OPERATIONS_PASS backend={label}",
                    native=label == "bundle", timeout=360,
                    evidence_class=("gpu-reference" if label == "reference"
                                    else "gpu-candidate"),
                    required=("cases=29 selections=58 msl=2.1",
                              "private_depth_staging=1", "typed_ms=3",
                              "depth_ms=1", "cube_gradient=float3",
                              "guards=argument,output,selection,upload,all_unselected_texels",
                              "gpu_execution=1"))
        elif group == "reflection-trim":
            case(
                "reflection-trim-abi8",
                [sys.executable, str(source / "test-reflection-trim.py")],
                "REFLECTION_TRIM_ABI8_PASS paths=2 rust_fixture=1",
                timeout=60,
                evidence_class="offline-metadata")
        elif group == "archive":
            exe = build("test-binary-archive")
            for mode in ("arch", "archive-type", "base-renderbox", "empty", "renderbox", "missing", "dylib", "mtl4", "fallback"):
                case("archive-" + mode, [exe, str(bundle), mode], "ARCHIVE_PASS mode=" + mode,
                     mode == "fallback")
        elif group == "arguments":
            exe = build("test-argument-encoder")
            for target, label in (("--reference", "reference"), (str(bundle), "bundle")):
                layout = [exe, target] + (["--layout-only"] if label == "reference" else [])
                case("arguments-layout-" + label, layout,
                     "ARG_LAYOUT_PASS cases=8" if label == "reference" else "ARG8_COVERAGE_PASS cases=18")
                if label == "reference":
                    valid_cases = ("nested.pointer_array", "nested.inline_array", "nested.wrapper_array", "stride.padded",
                                   "metadata.1d", "metadata.cube", "metadata.2d_array", "metadata.msaa", "metadata.read_write",
                                   "descriptor.1d", "descriptor.cube", "descriptor.2d_array", "descriptor.msaa", "descriptor.read_write")
                    for name in valid_cases:
                        case("arguments-reference-" + name, [exe, target, "--a7-probe", name], "ARG7_DONE case=" + name)
                case("arguments-dispatch-" + label, [exe, target, str(out / ("arguments-" + label))],
                     "ARGUMENT_ENCODER_PASS", label == "bundle")
        elif group == "argument-arrays":
            boundary = build("test-argument-binding", ["-I", str(include), "-framework", "IOKit", "-framework", "IOSurface"])
            case(
                "argument-binding", [boundary], "ARGUMENT_BINDING_PASS",
                evidence_class="host-mock",
                required=("simulated_vulkan=1", "gpu_execution=0"))
            descriptor_scan = build_c("test-argument-descriptor-scan")
            case(
                "argument-descriptor-scan", [descriptor_scan],
                "ARGUMENT_DESCRIPTOR_SCAN_PASS",
                evidence_class="host-mock",
                required=("production_reflection=1", "gpu_execution=0"))
            descriptor_layouts = build(
                "test-argument-descriptor-layouts",
                ["-I", str(include), "-framework", "IOKit",
                 "-framework", "IOSurface"])
            case(
                "argument-descriptor-layouts", [descriptor_layouts],
                "ARGUMENT_DESCRIPTOR_LAYOUTS_PASS",
                evidence_class="host-mock",
                required=("allocation_preflight=1", "gpu_execution=0"))
            tables = build("test-argument-tables", ["-I", str(include), "-framework", "IOKit", "-framework", "IOSurface"])
            case(
                "argument-tables", [tables], "ARGUMENT_TABLES_PASS",
                evidence_class="host-mock",
                required=("simulated_vulkan=1", "gpu_execution=0"))
            worker = build("test-argument-worker")
            case(
                "argument-worker",
                [worker, str(out / "argument-worker-cache")],
                "ARGUMENT_WORKER_PASS",
                evidence_class="host-mock",
                required=("gpu_execution=0",))
            postpass = build("test-argument-postpass", ["-I", str(include), "-framework", "IOKit", "-framework", "IOSurface"])
            case(
                "argument-postpass",
                [postpass, str(out / "argument-postpass-cache")],
                "ARGUMENT_POSTPASS_PASS",
                evidence_class="host-mock",
                required=("gpu_execution=0",))
            exe = build("test-argument-arrays")
            for target, label in (("--reference", "reference"), (str(bundle), "bundle")):
                case("argument-arrays-" + label, [exe, target, str(out / ("argument-arrays-" + label))],
                     "ARGUMENT_ARRAYS_PASS rounds=2 outputs=128", label == "bundle", timeout=120)
        elif group == "argument-contract":
            exe = build("test-argument-contract", ["-fblocks"])
            case("argument-contract-bundle", [exe, str(bundle)],
                 "ARGUMENT_CONTRACT_PASS", native=True,
                 evidence_class="gpu-candidate",
                 required=("invalid=7 array_element=1 gpu_words=16",))
        elif group == "argument-resources":
            exe = build("test-argument-resources")
            for target, label in (("--reference", "reference"), (str(bundle), "bundle")):
                name = "argument-resources-" + label
                case(name, [exe, target, str(out / name)],
                     "ARGUMENT_RESOURCES_PASS quadrants=4 channels=16",
                     native=label == "bundle",
                     evidence_class=("gpu-reference" if label == "reference"
                                     else "gpu-candidate"),
                     required=("exact_values=1..16 output_guards=unchanged",
                               "argument_guards=unchanged indirect_texture_alive=YES"))
        elif group == "argument-gpu":
            exe = build("test-argument-gpu-produced")
            for target, label in (("--reference", "reference"), (str(bundle), "bundle")):
                case("argument-gpu-" + label, [exe, target, str(out / ("argument-gpu-" + label))],
                     "ARGUMENT_GPU_PASS rounds=2 buffer_pointer=device-copy", label == "bundle", timeout=120)
        elif group == "argument-copy":
            exe = build("test-argument-resource-copy")
            for target, label in (("--reference", "reference"), (str(bundle), "bundle")):
                case("argument-copy-" + label, [exe, target, str(out / ("argument-copy-" + label))],
                     "ARGUMENT_COPY_PASS rounds=2 modes=shared,private tokens=texture,sampler,const-pointer",
                     label == "bundle", timeout=120)
        elif group in ("argument-integer-gather", "argument-depth-sampler", "argument-quad-lod"):
            exe = build("test-" + group)
            marker = group.upper().replace("-", "_") + "_PASS"
            for target, label in (("--reference", "reference"), (str(bundle), "bundle")):
                modes = ("divergent", "uniform", "gradient") if group == "argument-quad-lod" else (None,)
                for mode in modes:
                    suffix = ("-" + mode) if mode else ""
                    name = group + suffix + "-" + label
                    command = [exe, target, str(out / name)]
                    if mode:
                        command += ["--mode", mode]
                    case(name, command, marker, label == "bundle", timeout=120)
                if group == "argument-depth-sampler":
                    name = group + "-array-" + label
                    case(name, [exe, target, str(out / name), "--array"],
                         marker, label == "bundle", timeout=120)
        elif group == "intclear":
            exe = build("test-intclear-semantics")
            for target, label in (("--reference", "reference"), (str(bundle), "bundle")):
                case("intclear-signed32-" + label, [exe, target, "--r32s"],
                     "clears of an R32Sint target", label == "bundle", timeout=120)
        elif group == "argument-nested":
            nulls = build("test-argument-null")
            null_kinds = ("2d-read", "2d-rw", "2d-empty-rw", "uint-read", "uint-rw",
                          "3d-read", "3d-rw", "array-read", "array-rw",
                          "flat-2d-read", "flat-2d-rw", "sample-2d", "sample-3d",
                          "sample-array", "sample-cube", "gather-2d",
                          "sample-3d-empty", "gather-2d-empty",
                          "flat-sample-2d", "flat-sample-3d", "flat-sample-array",
                          "flat-sample-cube", "flat-gather-2d", "flat-sample-3d-empty",
                          "flat-gather-2d-empty", "sample-2d-empty-nil-sampler",
                          "fragment-sample-2d", "fragment-bias-2d", "fragment-gather-2d")
            for kind in null_kinds:
                for target, label in (("--reference", "reference"), (str(bundle), "bundle")):
                    name = "argument-null-" + kind + "-" + label
                    command = [nulls, target, str(out / name), kind]
                    capture = args.mode == "vm" and label == "reference"
                    if capture:
                        command.append("--capture-only")
                        name += "-capture"
                    case(name, command,
                         ("ARGUMENT_NULL_CAPTURE kind=" if capture else "ARGUMENT_NULL_PASS kind=") + kind,
                         native=label == "bundle", timeout=120, capture=capture)
            exe = build("test-argument-nested")
            for kind in ("inline", "records", "pointer", "records-copy", "pointer-copy",
                         "records-read-copy", "pointer-read-copy", "records-rw-copy", "pointer-rw-copy",
                         "records-render-copy", "pointer-render-copy", "records-null-copy", "pointer-null-copy",
                         "records-null-only-copy", "pointer-null-only-copy", "records-query-copy", "pointer-query-copy",
                         "records-query-uint-copy", "pointer-query-rw-copy", "records-query-null-copy", "pointer-query-null-copy",
                         "records-query-render-copy", "pointer-query-render-copy", "records-query-3d-copy",
                         "pointer-query-array-copy", "records-query-view-copy", "pointer-query-ms-copy"):
                for target, label in (("--reference", "reference"), (str(bundle), "bundle")):
                    name = "argument-nested-" + kind + "-" + label
                    command = [exe, target, str(out / name), kind]
                    capture = args.mode == "vm" and label == "reference" and kind != "inline"
                    if capture:
                        command.append("--capture-only")
                        name += "-capture"
                    case(name, command, ("ARGUMENT_NESTED_CAPTURE kind=" if capture else "ARGUMENT_NESTED_PASS kind=") + kind,
                         label == "bundle", timeout=120, capture=capture)
                    if args.mode == "live" and label == "reference" and kind != "inline":
                        case(name + "-capture", command + ["--capture-only"],
                             "ARGUMENT_NESTED_CAPTURE kind=" + kind,
                             timeout=120, capture=True, evidence_class="capture-only")
        elif group == "reflection":
            audit = out / "librdna4featureaudit.dylib"
            command = common + ["-I", str(include), "-DRDNA4_FEATURE_AUDIT", "-dynamiclib",
                                "-install_name", "@rpath/librdna4featureaudit.dylib",
                                str(source / "test-system-pipelines.m"), "-o", str(audit)]
            if not run_case(
                    out, "build-reflection-audit", command, "", env=env,
                    build=True, evidence_class="compile-only"):
                raise RuntimeError("reflection audit compilation failed")
            exe = build("test-pipeline-reflection", ["-L", str(out), "-lrdna4featureaudit", "-Wl,-rpath,@executable_path"])
            case("reflection-reference", [exe, "--reference", str(out / "layout-reference")], "REFLECTION_PASS no submissions")
            case("reflection-bundle", [exe, str(bundle), str(out / "layout-bundle")], "REFLECTION_PASS no submissions")
        elif group == "stitching":
            exe = build("test-stitching")
            case("stitching-reference", [exe, "--reference"], "STITCHING_LIBRARY_PASS AIR=")
            case("stitching-bundle", [exe, str(bundle)], "STITCHING_LINKED_ACCESSOR_PASS arguments result=returned")
            exe = build("test-stitch-reflection")
            case("stitch-reflection", [exe, str(source / "fixtures/stitch-reflection")], "STITCH_PASS cases=")
        elif group == "renderbox":
            exe = build("test-renderbox", ["-framework", "CoreGraphics"])
            case("renderbox-reference", [exe, "--reference"], "RENDERBOX_PASS")
            case("renderbox-bundle", [exe, str(bundle)], "RENDERBOX_PASS")
        elif group == "nocopy":
            exe = build("test-nocopy-readonly")
            case("nocopy-reference", [exe, "--reference"], "NOCOPY_PASS")
            case("nocopy-bundle", [exe, str(bundle)], "NOCOPY_PASS")
        elif group == "large-buffer-addressing":
            exe = build("test-large-buffer-addressing", ["-fblocks", "-O1"])
            for target, label in (("--reference", "reference"), (str(bundle), "bundle")):
                case("large-buffer-addressing-" + label, [exe, target],
                     "LARGE_BUFFER_ADDRESSING_PASS cases=8 rounds=16 dispatches=16",
                     native=label == "bundle", timeout=360,
                     evidence_class=("gpu-reference" if label == "reference"
                                     else "gpu-candidate"),
                     required=("small_backing=1 actual_over_4g=0", "msl_version=2.1"))
        elif group == "large-memory-over4g":
            exe = build("test-large-memory")
            for mode in ("private", "shared", "managed"):
                case("large-memory-over4g-" + mode,
                     [exe, str(bundle), mode, str(args.memory_iterations)],
                     "LARGE_MEMORY_PASS mode=" + mode, native=True,
                     timeout=1800 * args.memory_iterations,
                     case_env=dict(env, RADV_DARWIN_MOCK="0"),
                     evidence_class="gpu-candidate",
                     required=("payload_bytes=8589803520 buffer_bytes=8589934592",
                               "rmw_probe_invocations=1 gpu_execution=1"))
        elif group == "memory":
            lifetime = build("test-large-memory-lifetime", ["-I", str(include), "-framework", "IOKit", "-framework", "IOSurface"])
            case("memory-lifetime", [lifetime, "all"], "LARGE_MEMORY_LIFETIME_ALL_PASS", timeout=90)
            exe = build("test-large-memory")
            memory_env = dict(env, RADV_DARWIN_MOCK="0")
            case("memory-" + args.memory_mode,
                 [exe, str(bundle), args.memory_mode, str(args.memory_iterations)],
                 "LARGE_MEMORY_PASS mode=" + args.memory_mode, True,
                 timeout=180 * args.memory_iterations if args.memory_mode == "small" else 1800 * args.memory_iterations,
                 case_env=memory_env)
    if tier2_selection:
        missing = tier2_selection.missing()
        (out / "tier2-selection.json").write_text(
            json.dumps(tier2_selection.report(), indent=2) + "\n")
        for (case_id, side), patterns in missing.items():
            failures += 1
            print(f"TIER2_SELECTION_MISSING case={case_id} side={side} "
                  f"patterns={','.join(patterns)}", flush=True)
    counts = summarize_results(out / "results.tsv")
    print(f"COMPATIBILITY_SUITE mode={args.mode} pass={counts['PASS']} "
          f"capture={counts['CAPTURE']} build={counts['BUILD']} "
          f"skip={counts['SKIP']} fail={counts['FAIL']} failures={failures}")
    return int(failures != 0)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"COMPATIBILITY_FAIL {error}", file=sys.stderr)
        sys.exit(1)
