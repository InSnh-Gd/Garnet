#!/usr/bin/env python3
"""Apply the canonical patch series to a clean Mesa archive and test 0022."""

from __future__ import annotations

import argparse
from contextlib import nullcontext
import hashlib
from pathlib import Path
import re
import subprocess
import sys
import tarfile


PATCH_RE = re.compile(r"^(\d{4})-.+\.patch$")


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command: list[str], *, stdin: Path | None = None) -> subprocess.CompletedProcess[str]:
    print("+", " ".join(command), flush=True)
    with stdin.open("rb") if stdin else nullcontext(None) as input_file:
        result = subprocess.run(
            command,
            stdin=input_file,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
    if result.stdout:
        print(result.stdout, end="", flush=True)
    if result.returncode:
        raise SystemExit(f"command failed ({result.returncode}): {' '.join(command)}")
    return result


def patch_files(patch_dir: Path, last_patch: str) -> list[Path]:
    if not re.fullmatch(r"\d{4}", last_patch):
        raise SystemExit(f"invalid final patch number: {last_patch}")
    patches: list[tuple[str, Path]] = []
    for path in patch_dir.iterdir():
        match = PATCH_RE.match(path.name)
        if match and match.group(1) <= last_patch:
            patches.append((match.group(1), path))
    patches.sort()
    actual = [number for number, _ in patches]
    expected = [f"{number:04d}" for number in range(1, int(last_patch) + 1)]
    if actual != expected:
        raise SystemExit(
            f"patch series is not contiguous through {last_patch}: "
            f"expected {expected[0]}..{expected[-1]}, got {actual}"
        )
    return [path for _, path in patches]


def extract_archive(archive: Path, destination: Path) -> Path:
    destination.mkdir(parents=True)
    with tarfile.open(archive, "r:*") as tar:
        tar.extractall(destination, filter="data")
    roots = [path for path in destination.iterdir() if path.name != "__MACOSX"]
    if len(roots) != 1 or not roots[0].is_dir():
        raise SystemExit(f"archive must contain one top-level directory: {roots}")
    return roots[0]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("mesa_archive", type=Path)
    parser.add_argument("output_dir", type=Path)
    parser.add_argument(
        "--patches", type=Path, default=Path(__file__).resolve().parent
    )
    parser.add_argument("--last", default="0022")
    parser.add_argument("--expected-archive-sha256")
    args = parser.parse_args()

    archive = args.mesa_archive.resolve()
    output = args.output_dir.resolve()
    patch_dir = args.patches.resolve()
    if not archive.is_file():
        raise SystemExit(f"missing archive: {archive}")
    if output.exists() and any(output.iterdir()):
        raise SystemExit(f"output directory is not empty: {output}")
    output.mkdir(parents=True, exist_ok=True)

    archive_hash = digest(archive)
    if args.expected_archive_sha256 and archive_hash != args.expected_archive_sha256:
        raise SystemExit(
            f"archive hash mismatch: expected {args.expected_archive_sha256}, "
            f"got {archive_hash}"
        )

    source = extract_archive(archive, output / "extract")
    patches = patch_files(patch_dir, args.last)
    log: list[str] = [
        f"archive={archive}",
        f"archive_sha256={archive_hash}",
        "patches:",
    ]
    for patch in patches:
        patch_hash = digest(patch)
        log.append(f"{patch_hash}  {patch.name}")
        result = run(
            [
                "patch",
                "-d",
                str(source),
                "-p1",
                "--fuzz=0",
                "--force",
                "--batch",
                "--no-backup-if-mismatch",
            ],
            stdin=patch,
        )
        log.append(result.stdout)

    rejects = list(source.rglob("*.rej")) + list(source.rglob("*.orig"))
    if rejects:
        raise SystemExit(f"patch series left rejects/originals: {rejects}")

    test_output = output / "test"
    runner = Path(__file__).resolve().with_name(
        "test-radv-darwin-argument-limits.py"
    )
    result = run([sys.executable, str(runner), str(source), str(test_output)])
    log.append("=== test runner ===\n")
    log.append(result.stdout)
    log.append("=== source hashes ===\n")
    for name in (
        "include/darwin/radv_darwin_argument_limits.h",
        "src/amd/vulkan/radv_darwin_argument_limits.c",
        "src/amd/vulkan/radv_darwin_argument_limits_core.h",
    ):
        log.append(f"{digest(source / name)}  {name}")
    log_path = output / "apply-and-test.log"
    log_path.write_text("\n".join(log), encoding="utf-8")
    print(f"APPLY_AND_TEST_PASS log={log_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
