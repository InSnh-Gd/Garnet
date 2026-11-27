#!/usr/bin/env python3
"""Fail-closed identity checks for the standalone Garnet build inputs."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import re
import stat
import subprocess
import sys
import tempfile


FP_SCHEMA = "garnet-layout-fingerprint-v1"
VARIANTS_SCHEMA = "garnet-accel-variants-v1"
RELEASE_TARGET_SILICON = "navi44"
RELEASE_TARGET_PCI_IDS = "1002:7590"
RELEASE_FIRMWARE_LICENSE = "LICENSE.amdgpu"
RELEASE_VULKAN_LIB = (
    "/Library/GPUBundles/GarnetMetal.bundle/Contents/Resources/libvulkan_radeon.dylib"
)


GARNET_KEXT = "Garnet.kext"
GARNET_ID = "org.garnet.driver"
GARNET_EXECUTABLE = "Garnet"
ACCEL_KEXT = "GarnetAccel.kext"
ACCEL_ID = "org.garnet.accel"
ACCEL_EXECUTABLE = "GarnetAccel"
PCIBARCOMPAT_KEXT = "GarnetPCIBarCompat.kext"
PCIBARCOMPAT_ID = "org.garnet.pcibarcompat"
PCIBARCOMPAT_EXECUTABLE = "GarnetPCIBarCompat"
ANCHOR_INJECTOR_KEXT = "GarnetAnchorInjector.kext"
ANCHOR_INJECTOR_ID = "org.garnet.anchor-injector"
ANCHOR_ID = "org.garnet.code-signing"
AMFIANCHOR_KEXT = "AMFIAnchor.kext"
AMFIANCHOR_ID = "org.b00t0x.AMFIAnchor"
AMFIANCHOR_EXECUTABLE = "AMFIAnchor"
RECOVER_EFI_REL = "optional/GarnetRecover.efi"
DELIVERY_FORMAT = "garnet-delivery-v1"
LEGACY_IDENTITY_TOKENS = (
    b"AMDRadeonRDNA4",
    b"as.vit9696.Lilu",
    b"RDNA4FB",
    b"RDNA4Bringup",
    b"RDNA4Accel",
    b"RDNA4Metal",
    b"Navi48",
)
IGNORED_INPUT_ROOTS = (
    "src/garnet/firmware",
    "src/garnet-metal/spvcache",
)


class IdentityError(Exception):
    """A build input or output does not match its required identity."""


def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def digest_names(names):
    return sha256_bytes(
        "".join("{}\n".format(name) for name in sorted(names)).encode("utf-8")
    )


def valid_exclude_name(value):
    if not value or value.startswith("/") or "\\" in value:
        return False
    return all(part not in ("", ".", "..") for part in value.split("/"))


def path_is_excluded(relative, excludes):
    parts = relative.split("/")
    if ".git" in parts:
        return True
    return any(
        relative == excluded
        or relative.startswith(excluded + "/")
        or excluded in parts
        for excluded in excludes
    )


def directory_identity(root, excludes):
    entries = []
    file_count = 0
    symlink_count = 0
    for directory, dirnames, filenames in os.walk(root, followlinks=False):
        relative_directory = os.path.relpath(directory, root)
        if relative_directory == ".":
            relative_directory = ""

        kept_directories = []
        for name in sorted(dirnames):
            relative = "/".join(part for part in (relative_directory, name) if part)
            full = os.path.join(directory, name)
            if path_is_excluded(relative, excludes):
                continue
            if os.path.islink(full):
                target = os.readlink(full)
                entries.append(
                    (
                        os.fsencode(relative),
                        b"L  "
                        + sha256_bytes(os.fsencode(target)).encode("ascii")
                        + b"  ./"
                        + os.fsencode(relative)
                        + b"\n",
                    )
                )
                symlink_count += 1
            else:
                kept_directories.append(name)
        dirnames[:] = kept_directories

        for name in sorted(filenames):
            relative = "/".join(part for part in (relative_directory, name) if part)
            if path_is_excluded(relative, excludes):
                continue
            full = os.path.join(directory, name)
            metadata = os.lstat(full)
            if stat.S_ISLNK(metadata.st_mode):
                target = os.readlink(full)
                entries.append(
                    (
                        os.fsencode(relative),
                        b"L  "
                        + sha256_bytes(os.fsencode(target)).encode("ascii")
                        + b"  ./"
                        + os.fsencode(relative)
                        + b"\n",
                    )
                )
                symlink_count += 1
            elif stat.S_ISREG(metadata.st_mode):
                digest = sha256_file(Path(os.fsdecode(os.fsencode(full))))
                entries.append(
                    (
                        os.fsencode(relative),
                        digest.encode("ascii")
                        + b"  ./"
                        + os.fsencode(relative)
                        + b"\n",
                    )
                )
                file_count += 1
            else:
                raise IdentityError(
                    "unsupported input type: {}".format(os.path.join(root, relative))
                )
    canonical = b"".join(entry for _, entry in sorted(entries))
    return sha256_bytes(canonical), file_count, symlink_count


def read_key_values(path):
    values = {}
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as error:
        raise IdentityError("cannot read {}: {}".format(path, error)) from error
    for number, raw in enumerate(lines, 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if "=" not in line:
            raise IdentityError("{}:{}: expected KEY=VALUE".format(path, number))
        key, value = line.split("=", 1)
        if not re.fullmatch(r"[A-Z][A-Z0-9_]*", key):
            raise IdentityError("{}:{}: invalid key {!r}".format(path, number, key))
        if key in values:
            raise IdentityError("{}:{}: duplicate key {}".format(path, number, key))
        values[key] = value
    return values


def read_identity_values(path, stop_before_section=None):
    values = {}
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as error:
        raise IdentityError("cannot read {}: {}".format(path, error)) from error
    for number, raw in enumerate(lines, 1):
        if stop_before_section and raw.strip() == stop_before_section:
            break
        if not raw or raw.startswith("#"):
            continue
        if "=" not in raw:
            raise IdentityError("{}:{}: expected KEY=VALUE".format(path, number))
        key, value = raw.split("=", 1)
        if not key or key in values:
            raise IdentityError(
                "{}:{}: missing or duplicate key {!r}".format(path, number, key)
            )
        values[key] = value
    return values


def run_git(args, cwd, env=None, check=True):
    command = ["git"] + args
    try:
        result = subprocess.run(
            command,
            cwd=str(cwd),
            env=env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
    except OSError as error:
        raise IdentityError("cannot run git: {}".format(error)) from error
    if check and result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip()
        raise IdentityError(
            "{} failed in {}: {}".format(" ".join(command), cwd, detail)
        )
    return result


def run_git_bytes(args, cwd):
    command = ["git"] + args
    try:
        result = subprocess.run(
            command,
            cwd=str(cwd),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
    except OSError as error:
        raise IdentityError("cannot run git: {}".format(error)) from error
    if result.returncode != 0:
        detail = result.stderr.decode("utf-8", "replace").strip()
        raise IdentityError(
            "{} failed in {}: {}".format(" ".join(command), cwd, detail)
        )
    return result.stdout


def git_output(args, cwd, env=None):
    return run_git(args, cwd, env=env).stdout.strip()


def valid_exclude_path(value):
    if not value or value in (".", ".."):
        return False
    if value.startswith("/") or "\\" in value:
        return False
    return all(part not in ("", ".", "..") for part in value.split("/"))


def generated_source_excludes(source):
    excludes = set()
    for path in source.iterdir():
        if path.is_dir() and re.fullmatch(r"[Bb]uild(?:[-.].*)?", path.name):
            excludes.add(path.name)
        elif path.is_file() and (path.name.endswith(".log") or path.name == ".DS_Store"):
            excludes.add(path.name)
    return excludes


def series_patches(patches_dir):
    series = patches_dir / "series"
    try:
        lines = series.read_text(encoding="utf-8").splitlines()
    except OSError as error:
        raise IdentityError("cannot read patch series {}: {}".format(series, error)) from error
    patches = []
    for number, raw in enumerate(lines, 1):
        name = raw.strip()
        if not name or name.startswith("#"):
            continue
        if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._+-]*", name):
            raise IdentityError(
                "{}:{}: patch must be a plain filename: {!r}".format(
                    series, number, name
                )
            )
        patch = patches_dir / name
        if not patch.is_file():
            raise IdentityError("{}:{}: missing patch {}".format(series, number, patch))
        if name in patches:
            raise IdentityError("{}:{}: duplicate patch {}".format(series, number, name))
        patches.append(name)
    if not patches:
        raise IdentityError("{}: patch series is empty".format(series))
    return series, patches


def stage_tree(source, excludes, env=None):
    args = ["add", "-f", "-A", "--", "."]
    for path in excludes:
        if not valid_exclude_path(path):
            raise IdentityError("invalid source exclusion {!r}".format(path))
        args.extend(
            [":(exclude){}".format(path), ":(exclude){}/**".format(path)]
        )
    git_output(args, source, env=env)
    return git_output(["write-tree"], source, env=env)


def tree_for_worktree(source, temporary, excludes):
    index = temporary / "source-index"
    env = os.environ.copy()
    env["GIT_INDEX_FILE"] = str(index)
    git_output(["read-tree", "HEAD"], source, env=env)
    return stage_tree(source, excludes, env=env)


def check_submodule(source, path, expected):
    directory = source / path
    if not directory.is_dir():
        raise IdentityError("missing submodule: {}".format(directory))
    top = Path(git_output(["rev-parse", "--show-toplevel"], directory)).resolve()
    if top != directory.resolve():
        raise IdentityError("submodule path is not its own repository: {}".format(directory))
    actual = git_output(["rev-parse", "HEAD"], directory)
    if actual != expected:
        raise IdentityError(
            "submodule {} is at {}, expected {}".format(path, actual, expected)
        )
    dirty = git_output(
        ["status", "--porcelain", "--untracked-files=normal"], directory
    )
    if dirty:
        raise IdentityError("submodule {} is dirty:\n{}".format(path, dirty))
    return actual, git_output(["rev-parse", "HEAD^{tree}"], directory)


def verify_standalone_kext(kext_path, expected_bundle, expected_id, expected_exec, prefix):
    kext = Path(kext_path).resolve()
    if kext.name != expected_bundle:
        raise IdentityError(
            "{} bundle must be {}, got {}".format(prefix, expected_bundle, kext.name)
        )
    plist = kext / "Contents/Info.plist"
    try:
        data = plist.read_bytes()
        info = plistlib.loads(data)
    except (OSError, plistlib.InvalidFileException) as error:
        raise IdentityError("cannot read {}: {}".format(plist, error)) from error
    if info.get("CFBundleIdentifier") != expected_id:
        raise IdentityError(
            "wrong CFBundleIdentifier: {!r}".format(info.get("CFBundleIdentifier"))
        )
    executable = info.get("CFBundleExecutable")
    if executable != expected_exec:
        raise IdentityError(
            "wrong CFBundleExecutable: {!r}".format(executable)
        )
    binary = kext / "Contents/MacOS" / executable
    if not binary.is_file():
        raise IdentityError("missing {} executable: {}".format(prefix, binary))
    libraries = info.get("OSBundleLibraries")
    if isinstance(libraries, dict) and "as.vit9696.Lilu" in libraries:
        raise IdentityError(
            "{} bundle must not declare as.vit9696.Lilu".format(prefix)
        )
    binary_data = binary.read_bytes()
    for label, blob in (("Info.plist", data), ("executable", binary_data)):
        for token in LEGACY_IDENTITY_TOKENS:
            if token in blob:
                raise IdentityError(
                    "legacy {} identity remains in {} {}".format(
                        token.decode("ascii"), prefix, label
                    )
                )
    version = info.get("CFBundleVersion")
    if not version:
        raise IdentityError("{} bundle has no CFBundleVersion".format(prefix))
    output = {
        "{}.bundle".format(prefix): kext.name,
        "{}.identifier".format(prefix): info["CFBundleIdentifier"],
        "{}.executable".format(prefix): executable,
        "{}.version".format(prefix): str(version),
        "{}.standalone".format(prefix): "true",
        "{}.legacy_identity".format(prefix): "absent",
        "{}.info_plist_sha256".format(prefix): sha256_bytes(data),
        "{}.executable_sha256".format(prefix): sha256_bytes(binary_data),
    }
    for key, value in output.items():
        print("{}={}".format(key, value))


def command_garnet(args):
    verify_standalone_kext(
        args.kext, GARNET_KEXT, GARNET_ID, GARNET_EXECUTABLE, "garnet"
    )


def command_accel(args):
    verify_standalone_kext(
        args.kext, ACCEL_KEXT, ACCEL_ID, ACCEL_EXECUTABLE, "accel"
    )


def command_pcibarcompat(args):
    verify_standalone_kext(
        args.kext,
        PCIBARCOMPAT_KEXT,
        PCIBARCOMPAT_ID,
        PCIBARCOMPAT_EXECUTABLE,
        "pcibarcompat",
    )


def verify_amfianchor_kext(kext_path):
    kext = Path(kext_path).resolve()
    if kext.name != AMFIANCHOR_KEXT:
        raise IdentityError(
            "amfianchor bundle must be {}, got {}".format(AMFIANCHOR_KEXT, kext.name)
        )
    plist = kext / "Contents/Info.plist"
    try:
        data = plist.read_bytes()
        info = plistlib.loads(data)
    except (OSError, plistlib.InvalidFileException) as error:
        raise IdentityError("cannot read {}: {}".format(plist, error)) from error
    if info.get("CFBundleIdentifier") != AMFIANCHOR_ID:
        raise IdentityError("wrong AMFIAnchor CFBundleIdentifier: {!r}".format(info.get("CFBundleIdentifier")))
    executable = info.get("CFBundleExecutable")
    if executable != AMFIANCHOR_EXECUTABLE:
        raise IdentityError("wrong AMFIAnchor CFBundleExecutable: {!r}".format(executable))
    binary = kext / "Contents/MacOS" / executable
    if not binary.is_file():
        raise IdentityError("missing AMFIAnchor executable: {}".format(binary))
    version = info.get("CFBundleVersion")
    if not version:
        raise IdentityError("AMFIAnchor bundle has no CFBundleVersion")
    output = {
        "amfianchor.bundle": kext.name,
        "amfianchor.identifier": info["CFBundleIdentifier"],
        "amfianchor.executable": executable,
        "amfianchor.version": str(version),
        "amfianchor.info_plist_sha256": sha256_bytes(data),
        "amfianchor.executable_sha256": sha256_bytes(binary.read_bytes()),
    }
    return output


def command_amfianchor(args):
    for key, value in verify_amfianchor_kext(args.kext).items():
        print("{}={}".format(key, value))


def verify_pe32plus_efi(efi_path):
    data = efi_path.read_bytes()
    if len(data) < 0x100 or data[:2] != b"MZ":
        raise IdentityError("invalid EFI binary (missing MZ header): {}".format(efi_path))
    pe_off = int.from_bytes(data[0x3C:0x40], "little")
    if pe_off + 0x70 > len(data) or data[pe_off : pe_off + 4] != b"PE\x00\x00":
        raise IdentityError("invalid EFI binary (missing PE header): {}".format(efi_path))
    machine = int.from_bytes(data[pe_off + 4 : pe_off + 6], "little")
    opt_off = pe_off + 24
    magic = int.from_bytes(data[opt_off : opt_off + 2], "little")
    subsystem = int.from_bytes(data[opt_off + 68 : opt_off + 70], "little")
    if machine != 0x8664 or magic != 0x020B or subsystem != 10:
        raise IdentityError(
            "EFI binary {} must be x86_64 PE32+ EFI application (machine=0x{:x}, magic=0x{:x}, subsystem={})".format(
                efi_path, machine, magic, subsystem
            )
        )


def command_input(args):
    if not re.fullmatch(r"[A-Z][A-Z0-9_]*", args.label):
        raise IdentityError("input label must be uppercase KEY: {!r}".format(args.label))
    path = Path(args.path).resolve()
    excludes = []
    for value in args.exclude:
        if not valid_exclude_name(value):
            raise IdentityError("invalid input exclusion {!r}".format(value))
        excludes.append(value)
    if path.is_file():
        metadata = path.stat()
        output = {
            "path": str(path),
            "kind": "file",
            "sha256": sha256_file(path),
            "size_bytes": str(metadata.st_size),
            "file_count": "1",
            "symlink_count": "0",
        }
    elif path.is_dir():
        digest, file_count, symlink_count = directory_identity(path, excludes)
        output = {
            "path": str(path),
            "kind": "directory",
            "sha256": digest,
            "file_count": str(file_count),
            "symlink_count": str(symlink_count),
        }
        git_marker = path / ".git"
        if git_marker.exists():
            parent = Path(git_output(["rev-parse", "--show-toplevel"], path)).resolve()
            if parent != path:
                raise IdentityError(
                    "input is below but not the root of a Git checkout: {}".format(path)
                )
            output["git_head"] = git_output(["rev-parse", "HEAD"], path)
            output["git_tree"] = git_output(["rev-parse", "HEAD^{tree}"], path)
            output["git_dirty"] = (
                "yes"
                if git_output(
                    ["status", "--porcelain", "--untracked-files=normal"], path
                )
                else "no"
            )
    else:
        raise IdentityError("input is not a file or directory: {}".format(path))
    for key, value in output.items():
        print("{}.{}={}".format(args.label, key, value))


def _append_manifest_kext(
    values, expected_ids, standalone_keys, sha_keys, prefix, out_prefix, expected_id, identity
):
    values["{}_BUNDLE_ID".format(out_prefix)] = identity.get("{}.identifier".format(prefix), "")
    values["{}_VERSION".format(out_prefix)] = identity.get("{}.version".format(prefix), "")
    values["{}_EXECUTABLE_SHA256".format(out_prefix)] = identity.get(
        "{}.executable_sha256".format(prefix), ""
    )
    values["{}_INFO_SHA256".format(out_prefix)] = identity.get(
        "{}.info_plist_sha256".format(prefix), ""
    )
    values["{}_STANDALONE".format(out_prefix)] = identity.get("{}.standalone".format(prefix), "")
    expected_ids["{}_BUNDLE_ID".format(out_prefix)] = expected_id
    standalone_keys.append("{}_STANDALONE".format(out_prefix))
    sha_keys.extend(
        ["{}_EXECUTABLE_SHA256".format(out_prefix), "{}_INFO_SHA256".format(out_prefix)]
    )


def command_manifest(args):
    garnet = read_identity_values(Path(args.garnet_identity).resolve())
    accel = read_identity_values(Path(args.accel_identity).resolve())
    values = {"FORMAT": DELIVERY_FORMAT}
    expected_ids = {}
    standalone_keys = []
    sha_keys = []
    _append_manifest_kext(
        values, expected_ids, standalone_keys, sha_keys, "garnet", "GARNET", GARNET_ID, garnet
    )
    _append_manifest_kext(
        values, expected_ids, standalone_keys, sha_keys, "accel", "ACCEL", ACCEL_ID, accel
    )
    if args.pcibarcompat_identity is not None:
        if not args.pcibarcompat_identity:
            raise IdentityError("delivery manifest pcibarcompat identity path is empty")
        pcibar = read_identity_values(Path(args.pcibarcompat_identity).resolve())
        _append_manifest_kext(
            values,
            expected_ids,
            standalone_keys,
            sha_keys,
            "pcibarcompat",
            "PCIBARCOMPAT",
            PCIBARCOMPAT_ID,
            pcibar,
        )
    if args.release_mode == "1":
        if args.pcibarcompat_identity is None:
            raise IdentityError("Release 1 manifest requires --pcibarcompat-identity")
        if args.target_silicon != RELEASE_TARGET_SILICON:
            raise IdentityError(
                "Release 1 TARGET_SILICON must be {}, got {!r}".format(
                    RELEASE_TARGET_SILICON, args.target_silicon
                )
            )
        if args.target_pci_ids != RELEASE_TARGET_PCI_IDS:
            raise IdentityError(
                "Release 1 TARGET_PCI_IDS must be {}, got {!r}".format(
                    RELEASE_TARGET_PCI_IDS, args.target_pci_ids
                )
            )
        if not args.default_layout_id or not re.fullmatch(
            r"[0-9a-f]{64}", args.default_layout_id
        ):
            raise IdentityError(
                "Release 1 DEFAULT_LAYOUT_ID must be 64-char hex SHA256, got {!r}".format(
                    args.default_layout_id
                )
            )
        if args.amd_firmware_license != RELEASE_FIRMWARE_LICENSE:
            raise IdentityError(
                "Release 1 AMD_FIRMWARE_LICENSE must be {}, got {!r}".format(
                    RELEASE_FIRMWARE_LICENSE, args.amd_firmware_license
                )
            )
        values["RELEASE_MODE"] = "1"
        values["TARGET_SILICON"] = args.target_silicon
        values["TARGET_PCI_IDS"] = args.target_pci_ids
        values["DEFAULT_LAYOUT_ID"] = args.default_layout_id
        values["AMD_FIRMWARE_LICENSE"] = args.amd_firmware_license
        if args.default_accel_variant_path:
            values["DEFAULT_ACCEL_VARIANT_PATH"] = args.default_accel_variant_path
    elif args.release_mode not in (None, "0"):
        raise IdentityError("invalid --release-mode: {!r}".format(args.release_mode))
    if args.trust_anchor_identity is not None:
        if not args.trust_anchor_identity:
            raise IdentityError("delivery manifest trust-anchor identity path is empty")
        anchor = read_identity_values(Path(args.trust_anchor_identity).resolve())
        values["TRUST_ANCHOR_ENABLED"] = "1"
        values["TRUST_ANCHOR_BUNDLE_ID"] = anchor.get("anchor.bundle_id", "")
        values["TRUST_ANCHOR_ID"] = anchor.get("anchor.id", "")
        values["TRUST_ANCHOR_SKID"] = anchor.get("anchor.skid", "")
        values["TRUST_ANCHOR_INFO_SHA256"] = anchor.get("anchor.info_plist_sha256", "")
        expected_ids["TRUST_ANCHOR_BUNDLE_ID"] = ANCHOR_INJECTOR_ID
        expected_ids["TRUST_ANCHOR_ID"] = ANCHOR_ID
        sha_keys.append("TRUST_ANCHOR_INFO_SHA256")
        if not re.fullmatch(r"[0-9a-f]{40}", values["TRUST_ANCHOR_SKID"]):
            raise IdentityError("delivery manifest TRUST_ANCHOR_SKID must be 40-char lowercase hex")
        if args.amfianchor_bundled not in ("0", "1"):
            raise IdentityError("invalid --amfianchor-bundled: {!r}".format(args.amfianchor_bundled))
        values["AMFIANCHOR_BUNDLED"] = args.amfianchor_bundled
    else:
        values["TRUST_ANCHOR_ENABLED"] = "0"
        values["AMFIANCHOR_BUNDLED"] = "0"
    for key, value in values.items():
        if not value:
            raise IdentityError("delivery manifest is missing {}".format(key))
        if "\n" in value or "\r" in value:
            raise IdentityError("delivery manifest value contains a line break: {}".format(key))
    for key, expected_id in expected_ids.items():
        if values[key] != expected_id:
            raise IdentityError("delivery manifest {} is {}".format(key, values[key]))
    for key in standalone_keys:
        if values[key] != "true":
            raise IdentityError("delivery manifest requires standalone kext builds")
    for key in sha_keys:
        if not re.fullmatch(r"[0-9a-f]{64}", values[key]):
            raise IdentityError("delivery manifest {} is not a SHA256".format(key))
    for key, value in values.items():
        print("{}={}".format(key, value))


def _safe_rel_path(payload, rel):
    if not rel or rel.startswith("/") or "\\" in rel or ".." in rel.split("/"):
        raise IdentityError("unsafe relative path in release payload: {!r}".format(rel))
    target = (payload / rel).resolve()
    try:
        target.relative_to(payload)
    except ValueError as error:
        raise IdentityError("path escapes payload root: {!r}".format(rel)) from error
    return target


def _verify_fingerprint_doc(fp_path, expected_layout_id=None):
    try:
        fp_raw = fp_path.read_text(encoding="utf-8")
        fp = json.loads(fp_raw)
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise IdentityError("invalid layout fingerprint {}: {}".format(fp_path, error)) from error
    if fp.get("schema") != FP_SCHEMA:
        raise IdentityError("unexpected fingerprint schema in {}: {!r}".format(fp_path, fp.get("schema")))
    if fp.get("arch") != "x86_64":
        raise IdentityError("unexpected fingerprint arch: {!r}".format(fp.get("arch")))
    if fp.get("expectClasses") != 13 or fp.get("expectSlots") != 2720:
        raise IdentityError("unexpected fingerprint expectations in {}".format(fp_path))
    lid = fp.get("layoutId")
    if not isinstance(lid, str) or not re.fullmatch(r"[0-9a-f]{64}", lid):
        raise IdentityError("invalid layoutId in {}".format(fp_path))
    if fp.get("shortId") != lid[:16]:
        raise IdentityError("mismatched shortId in {}".format(fp_path))
    if expected_layout_id and lid != expected_layout_id:
        raise IdentityError(
            "layoutId mismatch in {}: got {}, want {}".format(fp_path, lid, expected_layout_id)
        )
    classes = fp.get("classes")
    if not isinstance(classes, list) or len(classes) != 13:
        raise IdentityError("fingerprint classes must hold 13 entries in {}".format(fp_path))
    canonical_core = {
        "schema": FP_SCHEMA,
        "arch": "x86_64",
        "expectClasses": 13,
        "expectSlots": 2720,
        "classes": classes,
    }
    canonical = json.dumps(
        canonical_core, sort_keys=True, separators=(",", ":"), ensure_ascii=True
    ).encode("utf-8")
    actual_lid = sha256_bytes(canonical)
    if actual_lid != lid:
        raise IdentityError(
            "layoutId integrity mismatch in {}: declared {}, computed {}".format(
                fp_path, lid, actual_lid
            )
        )
    for bad in ("/Users/", "/home/", "__TEXT", "AppleParavirtGPU.kext", "mach_header"):
        if bad in fp_raw:
            raise IdentityError("forbidden path or binary token {!r} in {}".format(bad, fp_path))
    return fp


def command_verify_release_payload(args):
    payload = Path(args.payload).resolve()
    if not payload.is_dir():
        raise IdentityError("release payload is not a directory: {}".format(payload))

    actual_files = set()
    for root_dir, dirnames, filenames in os.walk(payload, followlinks=False):
        for name in dirnames + filenames:
            p = Path(root_dir) / name
            if p.is_symlink():
                raise IdentityError("release payload must not contain symlinks: {}".format(p))
        for name in filenames:
            p = Path(root_dir) / name
            if not stat.S_ISREG(p.lstat().st_mode):
                raise IdentityError("non-regular file in release payload: {}".format(p))
            actual_files.add(p.relative_to(payload).as_posix())

    sha_path = payload / "SHA256SUMS"
    manifest_path = payload / "MANIFEST.txt"
    if not sha_path.is_file():
        raise IdentityError("missing SHA256SUMS in release payload")
    if not manifest_path.is_file():
        raise IdentityError("missing MANIFEST.txt in release payload")

    recorded = {}
    for line_no, raw in enumerate(sha_path.read_text(encoding="utf-8").splitlines(), 1):
        if not raw:
            continue
        m = re.fullmatch(r"([0-9a-f]{64})  \./(.+)", raw)
        if not m:
            raise IdentityError("SHA256SUMS line {} malformed: {!r}".format(line_no, raw))
        digest, rel = m.group(1), m.group(2)
        if rel == "SHA256SUMS":
            raise IdentityError("SHA256SUMS must not list itself")
        if rel in recorded:
            raise IdentityError("duplicate SHA256SUMS entry: {}".format(rel))
        target = _safe_rel_path(payload, rel)
        if not target.is_file():
            raise IdentityError("SHA256SUMS references missing file: {}".format(rel))
        actual_digest = sha256_file(target)
        if actual_digest != digest:
            raise IdentityError("SHA256 mismatch for {}: expected {}, got {}".format(rel, digest, actual_digest))
        recorded[rel] = digest

    expected_covered = actual_files - {"SHA256SUMS"}
    missing_from_sums = expected_covered - set(recorded.keys())
    if missing_from_sums:
        raise IdentityError(
            "files missing from SHA256SUMS: {}".format(", ".join(sorted(missing_from_sums)[:5]))
        )

    manifest_kv = read_identity_values(manifest_path, stop_before_section="# Payload files")
    for k, want in (
        ("FORMAT", DELIVERY_FORMAT),
        ("RELEASE_MODE", "1"),
        ("TARGET_SILICON", RELEASE_TARGET_SILICON),
        ("TARGET_PCI_IDS", RELEASE_TARGET_PCI_IDS),
        ("CHIPS", RELEASE_TARGET_SILICON),
        ("AMD_FIRMWARE_LICENSE", RELEASE_FIRMWARE_LICENSE),
        ("GARNET_BUNDLE_ID", GARNET_ID),
        ("ACCEL_BUNDLE_ID", ACCEL_ID),
        ("PCIBARCOMPAT_BUNDLE_ID", PCIBARCOMPAT_ID),
    ):
        if manifest_kv.get(k) != want:
            raise IdentityError(
                "MANIFEST.txt {} mismatch: got {!r}, want {!r}".format(k, manifest_kv.get(k), want)
            )
    default_lid = manifest_kv.get("DEFAULT_LAYOUT_ID", "")
    if not re.fullmatch(r"[0-9a-f]{64}", default_lid):
        raise IdentityError("invalid DEFAULT_LAYOUT_ID in MANIFEST.txt: {!r}".format(default_lid))

    required_files = (
        "GarnetMetal.bundle.tar",
        "native-translate",
        "native-bundle-check",
        "publish-metal",
        "mtlprobe-direct",
        "garnettest",
        "install-native.sh",
        "uninstall-native.sh",
        "deploy-common.sh",
        "activate-native.sh",
        "recover-uninstall.sh",
        "collect-installed.sh",
        "com.garnet.native.plist",
        "com.garnet.native-translate.plist",
        "com.garnet.recovery.plist",
        "LICENSE.amdgpu",
        "default-app-denylist.txt",
        "layout-fingerprint.json",
        "variants/index.json",
        "optional/vulkan/radeon_icd.x86_64.json",
        "optional/vulkan/radv-app.sh",
        "LOCAL-SOURCE-MANIFEST.txt",
    )
    for rel in required_files:
        p = payload / rel
        if not p.is_file() or p.stat().st_size == 0 and rel not in ("default-app-denylist.txt",):
            raise IdentityError("missing or empty required release file: {}".format(rel))
    if (payload / "GarnetAccel.kext").exists():
        raise IdentityError("Release 1 payload must place GarnetAccel.kext under variants/<shortId>/ only")

    icd = json.loads((payload / "optional/vulkan/radeon_icd.x86_64.json").read_text(encoding="utf-8"))
    if icd.get("file_format_version") != "1.0.0":
        raise IdentityError("unexpected Vulkan ICD file_format_version")
    if icd.get("ICD", {}).get("library_path") != RELEASE_VULKAN_LIB:
        raise IdentityError("unexpected Vulkan ICD library_path: {!r}".format(icd.get("ICD", {}).get("library_path")))

    verify_standalone_kext(payload / GARNET_KEXT, GARNET_KEXT, GARNET_ID, GARNET_EXECUTABLE, "garnet")
    verify_standalone_kext(
        payload / "optional" / PCIBARCOMPAT_KEXT,
        PCIBARCOMPAT_KEXT,
        PCIBARCOMPAT_ID,
        PCIBARCOMPAT_EXECUTABLE,
        "pcibarcompat",
    )
    _verify_fingerprint_doc(payload / "layout-fingerprint.json", expected_layout_id=default_lid)

    idx = json.loads((payload / "variants/index.json").read_text(encoding="utf-8"))
    if idx.get("schema") != VARIANTS_SCHEMA:
        raise IdentityError("unexpected variants/index.json schema: {!r}".format(idx.get("schema")))
    if idx.get("defaultVariant") != default_lid:
        raise IdentityError("variants/index.json defaultVariant disagrees with MANIFEST DEFAULT_LAYOUT_ID")
    variants = idx.get("variants")
    if not isinstance(variants, list) or not variants:
        raise IdentityError("variants/index.json variants list is empty")
    seen_default = False
    for entry in variants:
        lid = entry.get("layoutId", "")
        if not re.fullmatch(r"[0-9a-f]{64}", lid) or entry.get("shortId") != lid[:16]:
            raise IdentityError("invalid variant entry: {!r}".format(entry))
        if lid == default_lid:
            seen_default = True
        accel_p = _safe_rel_path(payload, entry.get("accelBundleRelPath", ""))
        fp_p = _safe_rel_path(payload, entry.get("fingerprintRelPath", ""))
        verify_standalone_kext(accel_p, ACCEL_KEXT, ACCEL_ID, ACCEL_EXECUTABLE, "accel")
        _verify_fingerprint_doc(fp_p, expected_layout_id=lid)
    if not seen_default:
        raise IdentityError("defaultVariant {} not listed in variants/index.json".format(default_lid))

    recover_efi = payload / RECOVER_EFI_REL
    if recover_efi.exists():
        verify_pe32plus_efi(recover_efi)

    trust_enabled = manifest_kv.get("TRUST_ANCHOR_ENABLED", "0")
    anchor_kext = payload / "optional" / ANCHOR_INJECTOR_KEXT
    amfianchor_kext = payload / "optional" / AMFIANCHOR_KEXT
    if trust_enabled == "1":
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        import make_anchor_injector  # noqa: E402

        try:
            anchor_kv = make_anchor_injector.verify_injector_bundle(
                anchor_kext,
                expected_bundle_id=ANCHOR_INJECTOR_ID,
                expected_anchor_id=ANCHOR_ID,
            )
        except make_anchor_injector.AnchorError as err:
            raise IdentityError("trust-anchor injector verification failed: {}".format(err)) from err
        if manifest_kv.get("TRUST_ANCHOR_ID") != ANCHOR_ID:
            raise IdentityError("MANIFEST.txt TRUST_ANCHOR_ID mismatch")
        if manifest_kv.get("TRUST_ANCHOR_SKID") != anchor_kv["anchor.skid"]:
            raise IdentityError("MANIFEST.txt TRUST_ANCHOR_SKID disagrees with GarnetAnchorInjector.kext")
        if manifest_kv.get("AMFIANCHOR_BUNDLED") == "1":
            verify_amfianchor_kext(amfianchor_kext)
    elif trust_enabled == "0":
        if anchor_kext.exists():
            raise IdentityError("GarnetAnchorInjector.kext present while TRUST_ANCHOR_ENABLED=0")
    else:
        raise IdentityError("invalid TRUST_ANCHOR_ENABLED value: {!r}".format(trust_enabled))

    print("release_payload.verified=true")
    print("release_payload.file_count={}".format(len(recorded)))
    print("release_payload.default_layout_id={}".format(default_lid))
    print("release_payload.trust_anchor_enabled={}".format(trust_enabled))


def local_source_entry(repo, relative):
    path = os.path.join(os.fsencode(repo), relative)
    try:
        metadata = os.lstat(path)
    except FileNotFoundError:
        return "M", "000000", "-"
    except OSError as error:
        raise IdentityError(
            "cannot stat local source {}: {}".format(os.fsdecode(relative), error)
        ) from error
    if stat.S_ISLNK(metadata.st_mode):
        try:
            target = os.readlink(path)
        except OSError as error:
            raise IdentityError(
                "cannot read symlink {}: {}".format(os.fsdecode(relative), error)
            ) from error
        return "L", "120000", sha256_bytes(target)
    if stat.S_ISREG(metadata.st_mode):
        mode = "100755" if metadata.st_mode & 0o111 else "100644"
        try:
            return "F", mode, sha256_file(Path(os.fsdecode(path)))
        except OSError as error:
            raise IdentityError(
                "cannot hash local source {}: {}".format(os.fsdecode(relative), error)
            ) from error
    raise IdentityError(
        "unsupported local source type: {}".format(os.fsdecode(relative))
    )


def command_local_source(args):
    repo = Path(args.repo).resolve()
    if not repo.is_dir():
        raise IdentityError("repository is not a directory: {}".format(repo))
    top = Path(git_output(["rev-parse", "--show-toplevel"], repo)).resolve()
    if top != repo:
        raise IdentityError("local source is not the repository root: {}".format(repo))

    tracked = {
        path
        for path in run_git_bytes(
            ["ls-files", "-z", "--cached"], repo
        ).split(b"\0")
        if path
    }
    untracked = {
        path
        for path in run_git_bytes(
            ["ls-files", "-z", "--others", "--exclude-standard"], repo
        ).split(b"\0")
        if path
    }
    ignored_inputs = set()
    for relative_root in IGNORED_INPUT_ROOTS:
        root = repo / relative_root
        if not root.is_dir():
            continue
        for path in root.rglob("*"):
            if path.is_file() or path.is_symlink():
                ignored_inputs.add(os.fsencode(str(path.relative_to(repo))))
    ignored_inputs -= tracked
    ignored_inputs -= untracked
    output = Path(args.output).resolve()
    try:
        output_relative = os.fsencode(str(output.relative_to(repo)))
    except ValueError:
        output_relative = None
    if output_relative is not None:
        tracked.discard(output_relative)
        untracked.discard(output_relative)
        ignored_inputs.discard(output_relative)

    entries = []
    patch_entries = []
    for relative in sorted(tracked | untracked | ignored_inputs):
        entry_type, mode, digest = local_source_entry(repo, relative)
        path_text = os.fsdecode(relative)
        line = "{}\t{}\t{}\t{}\n".format(
            entry_type, mode, digest, json.dumps(path_text, ensure_ascii=True)
        )
        entries.append(line)
        if (
            path_text.startswith("mesa-patches/")
            or path_text.startswith("patches/")
            or "/patches/" in path_text
        ):
            patch_entries.append(line)

    canonical = "".join(entries)
    patch_canonical = "".join(patch_entries)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(
        "# RDNA4 local source snapshot v1\n"
        "# scope=git-tracked+untracked-nonignored+declared-ignored-inputs; "
        "generated build outputs excluded\n"
        "# fields=type<TAB>mode<TAB>sha256<TAB>path-json\n"
        + canonical,
        encoding="utf-8",
    )
    values = {
        "local_source.scope": "git-tracked+untracked-nonignored+declared-ignored-inputs",
        "local_source.digest": sha256_bytes(canonical.encode("utf-8")),
        "local_source.file_count": str(len(entries)),
        "local_source.tracked_count": str(len(tracked)),
        "local_source.untracked_count": str(len(untracked)),
        "local_source.ignored_input_count": str(len(ignored_inputs)),
        "local_source.patch_count": str(len(patch_entries)),
        "local_source.patch_digest": sha256_bytes(
            patch_canonical.encode("utf-8")
        ),
        "local_source.sidecar": output.name,
    }
    for key, value in values.items():
        print("{}={}".format(key, value))


def build_parser():
    parser = argparse.ArgumentParser(
        description="Verify standalone Garnet build outputs and local source inputs."
    )
    subparsers = parser.add_subparsers(dest="command")
    subparsers.required = True

    garnet = subparsers.add_parser("garnet")
    garnet.add_argument("kext")
    garnet.set_defaults(func=command_garnet)

    accel = subparsers.add_parser("accel")
    accel.add_argument("kext")
    accel.set_defaults(func=command_accel)

    pcibarcompat = subparsers.add_parser("pcibarcompat")
    pcibarcompat.add_argument("kext")
    pcibarcompat.set_defaults(func=command_pcibarcompat)

    amfianchor = subparsers.add_parser("amfianchor")
    amfianchor.add_argument("kext")
    amfianchor.set_defaults(func=command_amfianchor)

    input_identity = subparsers.add_parser("input")
    input_identity.add_argument("label")
    input_identity.add_argument("path")
    input_identity.add_argument("--exclude", action="append", default=[])
    input_identity.set_defaults(func=command_input)

    manifest = subparsers.add_parser("manifest")
    manifest.add_argument("--garnet-identity", required=True)
    manifest.add_argument("--accel-identity", required=True)
    manifest.add_argument("--pcibarcompat-identity")
    manifest.add_argument("--trust-anchor-identity")
    manifest.add_argument("--amfianchor-bundled", default="0")
    manifest.add_argument("--release-mode", default="0")
    manifest.add_argument("--target-silicon", default="")
    manifest.add_argument("--target-pci-ids", default="")
    manifest.add_argument("--default-layout-id", default="")
    manifest.add_argument("--amd-firmware-license", default="")
    manifest.add_argument("--default-accel-variant-path", default="")
    manifest.set_defaults(func=command_manifest)

    verify_rel = subparsers.add_parser("verify-release-payload")
    verify_rel.add_argument("payload")
    verify_rel.set_defaults(func=command_verify_release_payload)

    local_source = subparsers.add_parser("local-source")
    local_source.add_argument("--repo", required=True)
    local_source.add_argument("--output", required=True)
    local_source.set_defaults(func=command_local_source)
    return parser


def main():
    args = build_parser().parse_args()
    try:
        args.func(args)
    except IdentityError as error:
        print("build_identity.py: {}".format(error), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
