#!/usr/bin/env python3
"""Host tests for AMFIAnchor Schema-1 injector generation and local CA script."""

from __future__ import annotations

import os
from pathlib import Path
import plistlib
import subprocess
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[3]
TOOLS_DIR = REPO / "tools" / "rdna4"
if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

import make_anchor_injector  # noqa: E402


class AnchorInjectorTests(unittest.TestCase):
    def setUp(self):
        # Place CA outside repository so make-signing-ca.sh repository guard passes!
        self._tmpdir = tempfile.TemporaryDirectory(prefix="garnet-anchor-test.")
        self.root = Path(self._tmpdir.name)

    def tearDown(self):
        self._tmpdir.cleanup()

    def run_cli(self, *args: str) -> subprocess.CompletedProcess:
        return subprocess.run(
            [sys.executable, "-B", str(TOOLS_DIR / "make_anchor_injector.py"), *args],
            text=True,
            capture_output=True,
            check=False,
        )

    def test_make_signing_ca_and_generate_verify_injector(self):
        ca_dir = self.root / "local-ca"
        res = subprocess.run(
            ["bash", str(TOOLS_DIR / "make-signing-ca.sh"), str(ca_dir), "Garnet Test"],
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertEqual(res.returncode, 0, res.stderr + res.stdout)
        for name in ("root.cert.pem", "intermediate.cert.pem", "leaf.cert.pem", "leaf.key.pem"):
            self.assertTrue((ca_dir / name).is_file(), f"missing {name}")

        # Verify platform marker OID 1.2.840.113635.100.6.22 is present on intermediate and leaf certs.
        openssl_bin = os.environ.get("GARNET_OPENSSL_BIN", "openssl")
        for cert_name in ("intermediate.cert.pem", "leaf.cert.pem"):
            txt = subprocess.check_output(
                [openssl_bin, "x509", "-in", str(ca_dir / cert_name), "-noout", "-text"],
                text=True,
            )
            self.assertIn("1.2.840.113635.100.6.22", txt, f"missing Apple platform OID in {cert_name}")
            self.assertIn("Code Signing", txt, f"missing Code Signing EKU in {cert_name}")

        # Refuses creating CA inside the git repository.
        in_repo = REPO / "temp" / "scratch" / "forbidden-ca-inside-repo"
        bad_ca = subprocess.run(
            ["bash", str(TOOLS_DIR / "make-signing-ca.sh"), str(in_repo)],
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertNotEqual(bad_ca.returncode, 0)
        self.assertFalse(in_repo.exists())

        # Generate codeless GarnetAnchorInjector.kext from public root.cert.pem.
        out_kext = self.root / "GarnetAnchorInjector.kext"
        gen = self.run_cli("generate", "--certificate", str(ca_dir / "root.cert.pem"), "--bundle", str(out_kext))
        self.assertEqual(gen.returncode, 0, gen.stderr)
        kv = dict(line.split("=", 1) for line in gen.stdout.splitlines() if "=" in line)
        self.assertEqual(kv["anchor.bundle"], "GarnetAnchorInjector.kext")
        self.assertEqual(kv["anchor.bundle_id"], "org.garnet.anchor-injector")
        self.assertEqual(kv["anchor.id"], "org.garnet.code-signing")
        self.assertEqual(len(kv["anchor.skid"]), 40)

        # Verify Info.plist structure directly.
        plist_bytes = (out_kext / "Contents" / "Info.plist").read_bytes()
        self.assertNotIn(b"PRIVATE KEY", plist_bytes)
        self.assertFalse((out_kext / "Contents" / "MacOS").exists())
        info = plistlib.loads(plist_bytes)
        self.assertNotIn("CFBundleExecutable", info)
        self.assertEqual(info["OSBundleRequired"], "Root")
        self.assertEqual(info["OSBundleLibraries"], {"org.b00t0x.AMFIAnchor": "0.1.0"})
        pers = info["IOKitPersonalities"]["org.garnet.anchor-injector"]
        self.assertEqual(pers["CFBundleIdentifier"], "org.b00t0x.AMFIAnchor")
        self.assertEqual(pers["IOClass"], "AMFIAnchorProvider")
        cfg = pers["AMFIAnchorConfig"]
        self.assertEqual(cfg["SchemaVersion"], 1)
        anchor = cfg["Anchors"]["org.garnet.code-signing"]
        self.assertTrue(anchor["Enabled"])
        self.assertEqual(len(anchor["SKID"]), 20)
        self.assertTrue(64 <= len(anchor["SPKI"]) <= 1024)

        # Verify CLI subcommand succeeds.
        ver = self.run_cli("verify", str(out_kext))
        self.assertEqual(ver.returncode, 0, ver.stderr)

        # Reject private key input.
        bad_priv = self.run_cli("generate", "--certificate", str(ca_dir / "root.key.pem"), "--bundle", str(self.root / "BadPriv.kext"))
        self.assertNotEqual(bad_priv.returncode, 0)
        self.assertFalse((self.root / "BadPriv.kext").exists())

        # Reject leaf cert (CA:FALSE).
        bad_leaf = self.run_cli("generate", "--certificate", str(ca_dir / "leaf.cert.pem"), "--bundle", str(self.root / "BadLeaf.kext"))
        self.assertNotEqual(bad_leaf.returncode, 0)
        self.assertFalse((self.root / "BadLeaf.kext").exists())

        # Reject symlink cert.
        sym_cert = self.root / "sym-root.pem"
        sym_cert.symlink_to(ca_dir / "root.cert.pem")
        bad_sym = self.run_cli("generate", "--certificate", str(sym_cert), "--bundle", str(self.root / "BadSym.kext"))
        self.assertNotEqual(bad_sym.returncode, 0)

        # Reject tampered SKID (all zeros) on verify.
        info["IOKitPersonalities"]["org.garnet.anchor-injector"]["AMFIAnchorConfig"]["Anchors"]["org.garnet.code-signing"]["SKID"] = b"\x00" * 20
        (out_kext / "Contents" / "Info.plist").write_bytes(plistlib.dumps(info))
        bad_ver = self.run_cli("verify", str(out_kext))
        self.assertNotEqual(bad_ver.returncode, 0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
