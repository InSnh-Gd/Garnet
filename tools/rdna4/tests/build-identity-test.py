#!/usr/bin/env python3
"""Host tests for tools/rdna4/build_identity.py and build-all label checks."""

import hashlib
import os
import pathlib
import plistlib
import shutil
import subprocess
import sys
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[3]
SCRIPT_DIR = REPO / "tools" / "rdna4"
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

import build_identity  # noqa: E402


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def make_kext(
    path: pathlib.Path,
    bundle_id: str,
    executable: str,
    version: str,
    libraries=None,
    binary_bytes: bytes = b"\xcf\xfa\xed\xfe\x07\x00\x00\x01garnet-binary\n",
    extra_plist=None,
):
    contents = path / "Contents"
    macos = contents / "MacOS"
    macos.mkdir(parents=True, exist_ok=True)
    plist = {
        "CFBundleIdentifier": bundle_id,
        "CFBundleExecutable": executable,
        "CFBundleVersion": version,
        "CFBundleShortVersionString": version,
    }
    if libraries is not None:
        plist["OSBundleLibraries"] = libraries
    if extra_plist:
        plist.update(extra_plist)
    with (contents / "Info.plist").open("wb") as stream:
        plistlib.dump(plist, stream)
    exe_path = macos / executable
    exe_path.write_bytes(binary_bytes)
    exe_path.chmod(0o755)
    return contents / "Info.plist", exe_path


def parse_kv(text: str) -> dict:
    parsed = {}
    for line in text.splitlines():
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        parsed[key] = value
    return parsed


class BuildIdentityTests(unittest.TestCase):
    def setUp(self):
        scratch_root = REPO / "temp" / "scratch"
        scratch_root.mkdir(parents=True, exist_ok=True)
        self._tmpdir = tempfile.TemporaryDirectory(
            prefix="build-identity-test.",
            dir=str(scratch_root),
        )
        self.root = pathlib.Path(self._tmpdir.name)

    def tearDown(self):
        self._tmpdir.cleanup()

    def run_cli(self, *args: str) -> subprocess.CompletedProcess:
        return subprocess.run(
            [sys.executable, "-B", str(SCRIPT_DIR / "build_identity.py"), *args],
            text=True,
            capture_output=True,
            check=False,
        )

    def test_valid_garnet_and_accel_and_manifest(self):
        garnet_kext = self.root / "Garnet.kext"
        g_plist, g_exe = make_kext(
            garnet_kext,
            "org.garnet.driver",
            "Garnet",
            "0.0.673",
            libraries={"com.apple.iokit.IOPCIFamily": "2.9"},
        )
        accel_kext = self.root / "GarnetAccel.kext"
        a_plist, a_exe = make_kext(
            accel_kext,
            "org.garnet.accel",
            "GarnetAccel",
            "0.0.8",
            libraries={"com.apple.iokit.IOAcceleratorFamily2": "400.0"},
        )

        g_res = self.run_cli("garnet", str(garnet_kext))
        self.assertEqual(g_res.returncode, 0, g_res.stderr)
        g_kv = parse_kv(g_res.stdout)
        self.assertEqual(g_kv["garnet.bundle"], "Garnet.kext")
        self.assertEqual(g_kv["garnet.identifier"], "org.garnet.driver")
        self.assertEqual(g_kv["garnet.executable"], "Garnet")
        self.assertEqual(g_kv["garnet.version"], "0.0.673")
        self.assertEqual(g_kv["garnet.standalone"], "true")
        self.assertEqual(g_kv["garnet.legacy_identity"], "absent")
        self.assertEqual(g_kv["garnet.info_plist_sha256"], sha256_bytes(g_plist.read_bytes()))
        self.assertEqual(g_kv["garnet.executable_sha256"], sha256_bytes(g_exe.read_bytes()))

        a_res = self.run_cli("accel", str(accel_kext))
        self.assertEqual(a_res.returncode, 0, a_res.stderr)
        a_kv = parse_kv(a_res.stdout)
        self.assertEqual(a_kv["accel.bundle"], "GarnetAccel.kext")
        self.assertEqual(a_kv["accel.identifier"], "org.garnet.accel")
        self.assertEqual(a_kv["accel.executable"], "GarnetAccel")
        self.assertEqual(a_kv["accel.version"], "0.0.8")
        self.assertEqual(a_kv["accel.standalone"], "true")
        self.assertEqual(a_kv["accel.legacy_identity"], "absent")
        self.assertEqual(a_kv["accel.info_plist_sha256"], sha256_bytes(a_plist.read_bytes()))
        self.assertEqual(a_kv["accel.executable_sha256"], sha256_bytes(a_exe.read_bytes()))

        g_file = self.root / "garnet.id"
        a_file = self.root / "accel.id"
        g_file.write_text(g_res.stdout)
        a_file.write_text(a_res.stdout)
        m_res = self.run_cli(
            "manifest",
            "--garnet-identity",
            str(g_file),
            "--accel-identity",
            str(a_file),
        )
        self.assertEqual(m_res.returncode, 0, m_res.stderr)
        m_kv = parse_kv(m_res.stdout)
        self.assertEqual(m_kv["FORMAT"], "garnet-delivery-v1")
        self.assertEqual(m_kv["GARNET_BUNDLE_ID"], "org.garnet.driver")
        self.assertEqual(m_kv["GARNET_VERSION"], "0.0.673")
        self.assertEqual(m_kv["GARNET_EXECUTABLE_SHA256"], g_kv["garnet.executable_sha256"])
        self.assertEqual(m_kv["GARNET_INFO_SHA256"], g_kv["garnet.info_plist_sha256"])
        self.assertEqual(m_kv["GARNET_STANDALONE"], "true")
        self.assertEqual(m_kv["ACCEL_BUNDLE_ID"], "org.garnet.accel")
        self.assertEqual(m_kv["ACCEL_VERSION"], "0.0.8")
        self.assertEqual(m_kv["ACCEL_EXECUTABLE_SHA256"], a_kv["accel.executable_sha256"])
        self.assertEqual(m_kv["ACCEL_INFO_SHA256"], a_kv["accel.info_plist_sha256"])
        self.assertEqual(m_kv["ACCEL_STANDALONE"], "true")
        self.assertNotIn("PCIBARCOMPAT_BUNDLE_ID", m_kv)

    def test_valid_pcibarcompat_and_optional_manifest(self):
        garnet_kext = self.root / "Garnet.kext"
        make_kext(garnet_kext, "org.garnet.driver", "Garnet", "0.0.673")
        accel_kext = self.root / "GarnetAccel.kext"
        make_kext(accel_kext, "org.garnet.accel", "GarnetAccel", "0.0.8")
        compat_kext = self.root / "GarnetPCIBarCompat.kext"
        c_plist, c_exe = make_kext(
            compat_kext,
            "org.garnet.pcibarcompat",
            "GarnetPCIBarCompat",
            "1.0.0",
            libraries={
                "com.apple.iokit.IOPCIFamily": "1.1",
                "com.apple.kpi.iokit": "20.0",
                "com.apple.kpi.libkern": "20.0",
                "com.apple.kpi.mach": "20.0",
                "com.apple.kpi.unsupported": "20.0",
            },
            extra_plist={"OSBundleRequired": "Root"},
        )
        c_res = self.run_cli("pcibarcompat", str(compat_kext))
        self.assertEqual(c_res.returncode, 0, c_res.stderr)
        c_kv = parse_kv(c_res.stdout)
        self.assertEqual(c_kv["pcibarcompat.bundle"], "GarnetPCIBarCompat.kext")
        self.assertEqual(c_kv["pcibarcompat.identifier"], "org.garnet.pcibarcompat")
        self.assertEqual(c_kv["pcibarcompat.executable"], "GarnetPCIBarCompat")
        self.assertEqual(c_kv["pcibarcompat.version"], "1.0.0")
        self.assertEqual(c_kv["pcibarcompat.standalone"], "true")
        self.assertEqual(c_kv["pcibarcompat.legacy_identity"], "absent")
        self.assertEqual(c_kv["pcibarcompat.info_plist_sha256"], sha256_bytes(c_plist.read_bytes()))
        self.assertEqual(c_kv["pcibarcompat.executable_sha256"], sha256_bytes(c_exe.read_bytes()))

        g_file = self.root / "g.id"
        a_file = self.root / "a.id"
        c_file = self.root / "c.id"
        g_file.write_text(self.run_cli("garnet", str(garnet_kext)).stdout)
        a_file.write_text(self.run_cli("accel", str(accel_kext)).stdout)
        c_file.write_text(c_res.stdout)
        m_res = self.run_cli(
            "manifest",
            "--garnet-identity",
            str(g_file),
            "--accel-identity",
            str(a_file),
            "--pcibarcompat-identity",
            str(c_file),
        )
        self.assertEqual(m_res.returncode, 0, m_res.stderr)
        m_kv = parse_kv(m_res.stdout)
        self.assertEqual(m_kv["PCIBARCOMPAT_BUNDLE_ID"], "org.garnet.pcibarcompat")
        self.assertEqual(m_kv["PCIBARCOMPAT_VERSION"], "1.0.0")
        self.assertEqual(m_kv["PCIBARCOMPAT_EXECUTABLE_SHA256"], c_kv["pcibarcompat.executable_sha256"])
        self.assertEqual(m_kv["PCIBARCOMPAT_INFO_SHA256"], c_kv["pcibarcompat.info_plist_sha256"])
        self.assertEqual(m_kv["PCIBARCOMPAT_STANDALONE"], "true")

        # Rejects Lilu dependency, wrong bundle ID, and legacy token in pcibarcompat.
        bad_lilu = self.root / "bad-pcibar-lilu" / "GarnetPCIBarCompat.kext"
        make_kext(
            bad_lilu,
            "org.garnet.pcibarcompat",
            "GarnetPCIBarCompat",
            "1.0.0",
            libraries={"as.vit9696.Lilu": "1.7.2"},
        )
        self.assertNotEqual(self.run_cli("pcibarcompat", str(bad_lilu)).returncode, 0)

        bad_id = self.root / "bad-pcibar-id" / "GarnetPCIBarCompat.kext"
        make_kext(bad_id, "org.wrong.pcibarcompat", "GarnetPCIBarCompat", "1.0.0")
        self.assertNotEqual(self.run_cli("pcibarcompat", str(bad_id)).returncode, 0)

        bad_tok = self.root / "bad-pcibar-tok" / "GarnetPCIBarCompat.kext"
        make_kext(
            bad_tok,
            "org.garnet.pcibarcompat",
            "GarnetPCIBarCompat",
            "1.0.0",
            binary_bytes=b"\xcf\xfa\xed\xfeNavi48\n",
        )
        self.assertNotEqual(self.run_cli("pcibarcompat", str(bad_tok)).returncode, 0)

        # Rejects non-standalone or mismatched pcibarcompat identity in manifest.
        c_file.write_text(c_res.stdout.replace("pcibarcompat.standalone=true", "pcibarcompat.standalone=false"))
        self.assertNotEqual(
            self.run_cli(
                "manifest",
                "--garnet-identity",
                str(g_file),
                "--accel-identity",
                str(a_file),
                "--pcibarcompat-identity",
                str(c_file),
            ).returncode,
            0,
        )

    def test_rejects_lilu_dependency_and_wrong_identity(self):
        lilu_garnet = self.root / "bad-lilu" / "Garnet.kext"
        make_kext(
            lilu_garnet,
            "org.garnet.driver",
            "Garnet",
            "0.0.673",
            libraries={"as.vit9696.Lilu": "1.7.2"},
        )
        res = self.run_cli("garnet", str(lilu_garnet))
        self.assertNotEqual(res.returncode, 0)
        self.assertTrue("Lilu" in res.stderr or "legacy" in res.stderr, res.stderr)

        wrong_name = self.root / "RDNA4Bringup.kext"
        make_kext(wrong_name, "org.garnet.driver", "Garnet", "0.0.673")
        res = self.run_cli("garnet", str(wrong_name))
        self.assertNotEqual(res.returncode, 0)

        wrong_id = self.root / "wrong-id" / "Garnet.kext"
        make_kext(wrong_id, "com.rdna4.bringup", "Garnet", "0.0.673")
        res = self.run_cli("garnet", str(wrong_id))
        self.assertNotEqual(res.returncode, 0)

        wrong_accel_exec = self.root / "wrong-exec" / "GarnetAccel.kext"
        make_kext(wrong_accel_exec, "org.garnet.accel", "RDNA4Accel", "0.0.8")
        res = self.run_cli("accel", str(wrong_accel_exec))
        self.assertNotEqual(res.returncode, 0)

    def test_rejects_every_legacy_token_in_plist_or_binary(self):
        for token in build_identity.LEGACY_IDENTITY_TOKENS:
            case_dir = self.root / ("tok-" + token.decode("ascii", errors="replace"))
            gk = case_dir / "Garnet.kext"
            make_kext(
                gk,
                "org.garnet.driver",
                "Garnet",
                "0.0.673",
                binary_bytes=b"\xcf\xfa\xed\xfe payload " + token + b"\n",
            )
            res = self.run_cli("garnet", str(gk))
            self.assertNotEqual(res.returncode, 0, f"binary token not rejected: {token!r}")

            ak = case_dir / "GarnetAccel.kext"
            make_kext(
                ak,
                "org.garnet.accel",
                "GarnetAccel",
                "0.0.8",
                extra_plist={"LegacyMarker": token.decode("ascii")},
            )
            res = self.run_cli("accel", str(ak))
            self.assertNotEqual(res.returncode, 0, f"plist token not rejected: {token!r}")

    def test_manifest_rejects_missing_or_mismatched_fields(self):
        g_file = self.root / "g.id"
        a_file = self.root / "a.id"
        g_file.write_text(
            "garnet.bundle=Garnet.kext\n"
            "garnet.identifier=com.rdna4.bringup\n"
            "garnet.executable=Garnet\n"
            "garnet.version=0.0.673\n"
            "garnet.standalone=true\n"
            "garnet.legacy_identity=absent\n"
            "garnet.info_plist_sha256=" + "a" * 64 + "\n"
            "garnet.executable_sha256=" + "b" * 64 + "\n"
        )
        a_file.write_text(
            "accel.bundle=GarnetAccel.kext\n"
            "accel.identifier=org.garnet.accel\n"
            "accel.executable=GarnetAccel\n"
            "accel.version=0.0.8\n"
            "accel.standalone=true\n"
            "accel.legacy_identity=absent\n"
            "accel.info_plist_sha256=" + "c" * 64 + "\n"
            "accel.executable_sha256=" + "d" * 64 + "\n"
        )
        res = self.run_cli(
            "manifest",
            "--garnet-identity",
            str(g_file),
            "--accel-identity",
            str(a_file),
        )
        self.assertNotEqual(res.returncode, 0)

        g_file.write_text(g_file.read_text().replace("com.rdna4.bringup", "org.garnet.driver"))
        a_file.write_text(a_file.read_text().replace("accel.standalone=true", "accel.standalone=false"))
        res2 = self.run_cli(
            "manifest",
            "--garnet-identity",
            str(g_file),
            "--accel-identity",
            str(a_file),
        )
        self.assertNotEqual(res2.returncode, 0)

    def test_input_determinism_and_exclusions(self):
        tree = self.root / "mksdk"
        (tree / "include").mkdir(parents=True)
        subprocess.run(["git", "-C", str(tree), "init", "-q"], check=True)
        header = tree / "include" / "a.h"
        header.write_text("#define GARNET 1\n")
        subprocess.run(["git", "-C", str(tree), "add", "include/a.h"], check=True)
        subprocess.run(
            ["git", "-C", str(tree), "-c", "user.name=Test", "-c", "user.email=test@example.com", "commit", "-q", "-m", "init"],
            check=True,
        )

        res1 = self.run_cli("input", "MKSDK", str(tree), "--exclude", ".git")
        self.assertEqual(res1.returncode, 0, res1.stderr)
        kv1 = parse_kv(res1.stdout)
        self.assertEqual(kv1["MKSDK.kind"], "directory")
        self.assertEqual(kv1["MKSDK.file_count"], "1")
        self.assertIn("MKSDK.git_head", kv1)

        # Updating mtime alone must not alter the digest.
        os.utime(header, (1700000000, 1700000000))
        res2 = self.run_cli("input", "MKSDK", str(tree), "--exclude", ".git")
        self.assertEqual(res2.returncode, 0, res2.stderr)
        self.assertEqual(parse_kv(res2.stdout)["MKSDK.sha256"], kv1["MKSDK.sha256"])

        # Changing excluded .git metadata must not change the digest, changing included file must.
        (tree / ".git" / "description").write_text("changed description\n")
        res3 = self.run_cli("input", "MKSDK", str(tree), "--exclude", ".git")
        self.assertEqual(res3.returncode, 0, res3.stderr)
        self.assertEqual(parse_kv(res3.stdout)["MKSDK.sha256"], kv1["MKSDK.sha256"])

        header.write_text("#define GARNET 2\n")
        res4 = self.run_cli("input", "MKSDK", str(tree), "--exclude", ".git")
        self.assertNotEqual(parse_kv(res4.stdout)["MKSDK.sha256"], kv1["MKSDK.sha256"])

        unborn = self.root / "unborn"
        unborn.mkdir()
        (unborn / ".git").mkdir()
        bad_git = self.run_cli("input", "BADGIT", str(unborn), "--exclude", ".git")
        self.assertNotEqual(bad_git.returncode, 0)

        missing = self.run_cli("input", "MKSDK", str(self.root / "missing"))
        self.assertNotEqual(missing.returncode, 0)

    def test_local_source_tracks_declared_ignored_and_excludes_ignored_temp(self):
        repo = self.root / "repo"
        repo.mkdir()
        subprocess.run(["git", "-C", str(repo), "init", "-q"], check=True)
        (repo / ".gitignore").write_text("temp/\nsrc/garnet/firmware/\n*.o\n")
        (repo / "src/garnet/src").mkdir(parents=True)
        (repo / "src/garnet/src/Garnet.cpp").write_text("// tracked\n")
        (repo / "mesa-patches").mkdir()
        (repo / "mesa-patches/0001-test.patch").write_text("diff --git a/x b/x\n")
        subprocess.run(["git", "-C", str(repo), "add", ".gitignore", "src/garnet/src/Garnet.cpp", "mesa-patches/0001-test.patch"], check=True)

        (repo / "src/garnet/src/Untracked.cpp").write_text("// untracked source\n")
        (repo / "src/garnet/firmware").mkdir(parents=True)
        fw = repo / "src/garnet/firmware/gc.bin"
        fw.write_bytes(b"fw-v1")
        (repo / "src/garnet-metal/spvcache").mkdir(parents=True)
        (repo / "src/garnet-metal/spvcache/0.spv").write_bytes(b"spv-v1")
        (repo / "temp/scratch").mkdir(parents=True)
        (repo / "temp/scratch/noise.txt").write_text("ignored scratch\n")

        sidecar = self.root / "local-source.txt"
        res1 = self.run_cli("local-source", "--repo", str(repo), "--output", str(sidecar))
        self.assertEqual(res1.returncode, 0, res1.stderr)
        kv1 = parse_kv(res1.stdout)
        self.assertEqual(kv1["local_source.scope"], "git-tracked+untracked-nonignored+declared-ignored-inputs")
        self.assertEqual(kv1["local_source.patch_count"], "1")
        manifest_text = sidecar.read_text()
        self.assertIn("src/garnet/firmware/gc.bin", manifest_text)
        self.assertIn("src/garnet-metal/spvcache/0.spv", manifest_text)
        self.assertIn("src/garnet/src/Untracked.cpp", manifest_text)
        self.assertNotIn("temp/scratch/noise.txt", manifest_text)

        # Ignored scratch changes must not alter the digest.
        (repo / "temp/scratch/noise.txt").write_text("changed noise\n")
        res2 = self.run_cli("local-source", "--repo", str(repo), "--output", str(sidecar))
        self.assertEqual(parse_kv(res2.stdout)["local_source.digest"], kv1["local_source.digest"])

        # Declared ignored firmware changes MUST alter the digest.
        fw.write_bytes(b"fw-v2")
        res3 = self.run_cli("local-source", "--repo", str(repo), "--output", str(sidecar))
        self.assertNotEqual(parse_kv(res3.stdout)["local_source.digest"], kv1["local_source.digest"])

        # Non-git directory must fail.
        nongit = self.root / "nongit"
        nongit.mkdir()
        res4 = self.run_cli("local-source", "--repo", str(nongit), "--output", str(self.root / "bad.txt"))
        self.assertNotEqual(res4.returncode, 0)

    def test_build_all_label_self_check(self):
        env = dict(os.environ)
        env["TMPDIR"] = str(self.root)
        res = subprocess.run(
            ["bash", str(SCRIPT_DIR / "build-all.sh"), "--label-self-check"],
            cwd=str(REPO),
            env=env,
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertEqual(res.returncode, 0, res.stderr + res.stdout)
        self.assertIn("build-all label self-check passed", res.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=2)
