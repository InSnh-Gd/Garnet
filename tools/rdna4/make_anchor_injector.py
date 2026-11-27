#!/usr/bin/env python3
"""Generate and validate a Schema-1 AMFIAnchor codeless injector kext (public CA only).

Strictly rejects private-key material and malformed certificates.
Produces GarnetAnchorInjector.kext with zero CFBundleExecutable, carrying only
the Root CA public DER SubjectPublicKeyInfo (SPKI) and 20-byte SKID.
"""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import plistlib
import re
import subprocess
import sys


AMFIANCHOR_BUNDLE_ID = "org.b00t0x.AMFIAnchor"
AMFIANCHOR_MIN_VERSION = "0.1.0"
DEFAULT_BUNDLE_ID = "org.garnet.anchor-injector"
DEFAULT_ANCHOR_ID = "org.garnet.code-signing"
DEFAULT_VERSION = "1.0.0"

ID_PATTERN = re.compile(r"^[A-Za-z0-9._-]{1,63}$")
SKID_PATTERN = re.compile(r"X509v3 Subject Key Identifier:\s*\n\s*([0-9A-Fa-f:]+)")


class AnchorError(Exception):
    """Raised when certificate or injector validation fails."""


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def run_openssl(args: list[str], input_data: bytes | None = None) -> bytes:
    env = dict(os.environ, LC_ALL="C")
    openssl_bin = os.environ.get("GARNET_OPENSSL_BIN", "openssl")
    try:
        res = subprocess.run(
            [openssl_bin, *args],
            input=input_data,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=env,
            check=False,
        )
    except OSError as err:
        raise AnchorError(f"cannot execute openssl ({openssl_bin}): {err}") from err
    if res.returncode != 0:
        detail = res.stderr.decode("utf-8", "replace").strip() or res.stdout.decode("utf-8", "replace").strip()
        raise AnchorError(f"openssl {' '.join(args)} failed: {detail}")
    return res.stdout


def extract_public_anchor(cert_path: Path) -> tuple[bytes, bytes]:
    if cert_path.is_symlink():
        raise AnchorError(f"refusing symlink certificate: {cert_path}")
    if not cert_path.is_file():
        raise AnchorError(f"certificate file not found: {cert_path}")

    raw = cert_path.read_bytes()
    if not raw:
        raise AnchorError(f"certificate file is empty: {cert_path}")
    if b"PRIVATE KEY" in raw.upper():
        raise AnchorError("refusing input containing PRIVATE KEY material; pass public Root CA cert only")

    inform = "PEM" if raw.lstrip().startswith(b"-----BEGIN") else "DER"
    prefix = ["x509", "-inform", inform, "-in", str(cert_path)]

    text = run_openssl([*prefix, "-noout", "-text"]).decode("utf-8", "replace")
    if "CA:TRUE" not in text:
        raise AnchorError("certificate must be a CA certificate (Basic Constraints CA:TRUE required)")

    skid_match = SKID_PATTERN.search(text)
    if not skid_match:
        raise AnchorError("certificate is missing X509v3 Subject Key Identifier")
    try:
        skid = bytes.fromhex(skid_match.group(1).replace(":", ""))
    except ValueError as err:
        raise AnchorError(f"malformed Subject Key Identifier hex: {err}") from err
    if len(skid) != 20 or not any(skid):
        raise AnchorError(f"SKID must be 20 non-zero bytes, got {len(skid)} byte(s)")

    pub_pem = run_openssl([*prefix, "-pubkey", "-noout"])
    spki = run_openssl(["pkey", "-pubin", "-outform", "DER"], input_data=pub_pem)
    if not (64 <= len(spki) <= 1024):
        raise AnchorError(f"DER SPKI length must be 64..1024 bytes, got {len(spki)}")

    return spki, skid


def build_injector_plist(
    bundle_name: str,
    bundle_id: str,
    anchor_id: str,
    version: str,
    spki: bytes,
    skid: bytes,
) -> dict:
    if not ID_PATTERN.fullmatch(bundle_id):
        raise AnchorError(f"invalid bundle identifier: {bundle_id!r}")
    if not ID_PATTERN.fullmatch(anchor_id):
        raise AnchorError(f"invalid anchor identifier: {anchor_id!r}")
    if not version or "\n" in version or "\r" in version:
        raise AnchorError(f"invalid version string: {version!r}")

    personality = {
        "CFBundleIdentifier": AMFIANCHOR_BUNDLE_ID,
        "IOClass": "AMFIAnchorProvider",
        "IOProviderClass": "IOResources",
        "IOResourceMatch": "IOKit",
        "IOMatchCategory": bundle_id,
        "AMFIAnchorProviderID": bundle_id,
        "AMFIAnchorConfig": {
            "SchemaVersion": 1,
            "Anchors": {
                anchor_id: {
                    "Enabled": True,
                    "SPKI": spki,
                    "SKID": skid,
                }
            },
        },
    }
    return {
        "CFBundleIdentifier": bundle_id,
        "CFBundleName": bundle_name,
        "CFBundleVersion": version,
        "CFBundleShortVersionString": version,
        "CFBundlePackageType": "KEXT",
        "OSBundleRequired": "Root",
        "OSBundleLibraries": {
            AMFIANCHOR_BUNDLE_ID: AMFIANCHOR_MIN_VERSION,
        },
        "IOKitPersonalities": {
            bundle_id: personality,
        },
    }


def verify_injector_bundle(
    kext_path: Path,
    expected_bundle_id: str = DEFAULT_BUNDLE_ID,
    expected_anchor_id: str = DEFAULT_ANCHOR_ID,
) -> dict[str, str]:
    if kext_path.is_symlink() or not kext_path.is_dir():
        raise AnchorError(f"injector kext is not a regular directory: {kext_path}")
    plist_path = kext_path / "Contents" / "Info.plist"
    if plist_path.is_symlink() or not plist_path.is_file():
        raise AnchorError(f"missing Info.plist in {kext_path}")
    macos_dir = kext_path / "Contents" / "MacOS"
    if macos_dir.exists():
        raise AnchorError(f"codeless injector must not contain Contents/MacOS: {kext_path}")

    raw = plist_path.read_bytes()
    if b"PRIVATE KEY" in raw.upper():
        raise AnchorError(f"private key marker found in {plist_path}")
    try:
        info = plistlib.loads(raw)
    except Exception as err:
        raise AnchorError(f"invalid plist {plist_path}: {err}") from err

    if info.get("CFBundleIdentifier") != expected_bundle_id:
        raise AnchorError(f"wrong CFBundleIdentifier: {info.get('CFBundleIdentifier')!r}")
    if "CFBundleExecutable" in info:
        raise AnchorError("codeless injector must not declare CFBundleExecutable")
    if info.get("OSBundleRequired") != "Root":
        raise AnchorError(f"OSBundleRequired must be 'Root', got {info.get('OSBundleRequired')!r}")

    libs = info.get("OSBundleLibraries")
    if not isinstance(libs, dict) or libs.get(AMFIANCHOR_BUNDLE_ID) != AMFIANCHOR_MIN_VERSION:
        raise AnchorError(f"OSBundleLibraries must require {AMFIANCHOR_BUNDLE_ID} == {AMFIANCHOR_MIN_VERSION}")
    if "as.vit9696.Lilu" in libs:
        raise AnchorError("codeless injector must not link as.vit9696.Lilu directly")

    pers = info.get("IOKitPersonalities")
    if not isinstance(pers, dict) or expected_bundle_id not in pers:
        raise AnchorError(f"missing IOKitPersonalities[{expected_bundle_id!r}]")
    p = pers[expected_bundle_id]
    if p.get("CFBundleIdentifier") != AMFIANCHOR_BUNDLE_ID or p.get("IOClass") != "AMFIAnchorProvider":
        raise AnchorError("personality must target org.b00t0x.AMFIAnchor / AMFIAnchorProvider")
    cfg = p.get("AMFIAnchorConfig")
    if not isinstance(cfg, dict) or cfg.get("SchemaVersion") != 1:
        raise AnchorError("AMFIAnchorConfig.SchemaVersion must be 1")
    anchors = cfg.get("Anchors")
    if not isinstance(anchors, dict) or expected_anchor_id not in anchors:
        raise AnchorError(f"missing anchor {expected_anchor_id!r} in AMFIAnchorConfig.Anchors")
    entry = anchors[expected_anchor_id]
    if entry.get("Enabled", True) is not True:
        raise AnchorError(f"anchor {expected_anchor_id!r} must be enabled")
    spki = entry.get("SPKI")
    skid = entry.get("SKID")
    if not isinstance(spki, (bytes, bytearray)) or not (64 <= len(spki) <= 1024):
        raise AnchorError("invalid SPKI bytes in injector")
    if not isinstance(skid, (bytes, bytearray)) or len(skid) != 20 or not any(skid):
        raise AnchorError("invalid SKID bytes in injector")

    return {
        "anchor.bundle": kext_path.name,
        "anchor.bundle_id": expected_bundle_id,
        "anchor.id": expected_anchor_id,
        "anchor.version": str(info.get("CFBundleVersion", "")),
        "anchor.skid": bytes(skid).hex(),
        "anchor.spki_sha256": sha256_bytes(bytes(spki)),
        "anchor.info_plist_sha256": sha256_bytes(raw),
    }


def generate_injector(
    cert_path: Path,
    bundle_path: Path,
    bundle_id: str,
    anchor_id: str,
    version: str,
    overwrite: bool,
) -> dict[str, str]:
    if bundle_path.is_symlink():
        raise AnchorError(f"refusing symlink output path: {bundle_path}")
    if bundle_path.exists():
        if not overwrite:
            raise AnchorError(f"output bundle already exists: {bundle_path}")
        import shutil
        shutil.rmtree(bundle_path)

    spki, skid = extract_public_anchor(cert_path)
    info = build_injector_plist(
        bundle_name=bundle_path.stem,
        bundle_id=bundle_id,
        anchor_id=anchor_id,
        version=version,
        spki=spki,
        skid=skid,
    )
    contents = bundle_path / "Contents"
    contents.mkdir(parents=True, exist_ok=False)
    (contents / "Info.plist").write_bytes(plistlib.dumps(info, sort_keys=True))
    return verify_injector_bundle(bundle_path, expected_bundle_id=bundle_id, expected_anchor_id=anchor_id)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Generate or verify an AMFIAnchor Schema-1 codeless injector kext.")
    sub = parser.add_subparsers(dest="command", required=True)

    gen = sub.add_parser("generate", help="Generate codeless injector kext from public Root CA certificate")
    gen.add_argument("--certificate", type=Path, required=True, help="Public Root CA certificate (PEM or DER)")
    gen.add_argument("--bundle", type=Path, required=True, help="Output .kext directory path")
    gen.add_argument("--bundle-id", default=DEFAULT_BUNDLE_ID)
    gen.add_argument("--anchor-id", default=DEFAULT_ANCHOR_ID)
    gen.add_argument("--version", default=DEFAULT_VERSION)
    gen.add_argument("--overwrite", action="store_true")

    ver = sub.add_parser("verify", help="Verify an existing codeless injector kext")
    ver.add_argument("bundle", type=Path)
    ver.add_argument("--bundle-id", default=DEFAULT_BUNDLE_ID)
    ver.add_argument("--anchor-id", default=DEFAULT_ANCHOR_ID)

    args = parser.parse_args(argv)
    try:
        if args.command == "generate":
            kv = generate_injector(
                cert_path=args.certificate,
                bundle_path=args.bundle,
                bundle_id=args.bundle_id,
                anchor_id=args.anchor_id,
                version=args.version,
                overwrite=args.overwrite,
            )
        else:
            kv = verify_injector_bundle(
                kext_path=args.bundle,
                expected_bundle_id=args.bundle_id,
                expected_anchor_id=args.anchor_id,
            )
    except AnchorError as err:
        sys.stderr.write(f"make_anchor_injector: error: {err}\n")
        return 1

    for k, v in kv.items():
        print(f"{k}={v}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
