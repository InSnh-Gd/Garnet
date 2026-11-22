#!/bin/bash
# Integration coverage for the Garnet delivery scripts, including macOS Bash 3.2.
# Target commands are stubs and all writes stay below one scratch root; no kext is installed/loaded.
set -euo pipefail
if [ "${1:-}" = --help ]; then
    printf 'Usage: bash install-native-test.sh\n'
    printf 'Tests install/uninstall preflight, migration and rollback on scratch roots.\n'
    exit 0
fi
[ "$#" -eq 0 ] || exit 64
source_dir=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$source_dir/../../.." && pwd)
if [ -z "${TMPDIR:-}" ]; then
    TMPDIR="$repo/temp/scratch"
    mkdir -p "$TMPDIR"
    export TMPDIR
fi
work=$(mktemp -d "${TMPDIR%/}/garnet-install-test.XXXXXXXX")
if [ "${RDNA4_KEEP_TEST_WORK:-0}" = 1 ]; then
    printf 'TEST_WORK=%s\n' "$work"
else
    trap 'rm -rf "$work"' EXIT
fi
mkdir "$work/bin"
for name in uname id sw_vers sysctl plutil codesign kmutil launchctl chown sync; do
    cp "$source_dir/deploy-stub.sh" "$work/bin/$name"
    chmod 700 "$work/bin/$name"
done
cp "$source_dir/plistbuddy-stub.py" "$work/bin/PlistBuddy"
chmod 700 "$work/bin/PlistBuddy"
export PATH="$work/bin:$PATH"
export RDNA4_PLISTBUDDY="$work/bin/PlistBuddy"
guid=7C436110-AB2A-4BBB-A880-FE41995C9F82
unset RDNA4_DEPLOY_STUB_FAIL RDNA4_DEPLOY_TEST_VMM RDNA4_DEPLOY_TEST_LOADED_JOBS

cat > "$work/check.py" <<'PY'
import hashlib
import os
from pathlib import Path
import plistlib
import shutil
import sys
import tarfile

mode, base_arg = sys.argv[1], Path(sys.argv[2]).resolve()
root, payload = base_arg / "root", base_arg / "payload"
esp = root / "ESP/EFI/OC"
config_path = esp / "config.plist"
guid = "7C436110-AB2A-4BBB-A880-FE41995C9F82"
support = root / "Library/Application Support/RDNA4/native"
aux = root / "Library/KernelCollections/AuxiliaryKernelExtensions.kc"

garnet = "Garnet.kext"
garnet_id = "org.garnet.driver"
accel = "GarnetAccel.kext"
accel_id = "org.garnet.accel"
metal = "GarnetMetal.bundle"
metal_id = "org.garnet.metal"

old_display = "AMDRadeonRDNA4.kext"
old_display_id = "com.hackintosh.AMDRadeonRDNA4"
old_fb = "RDNA4FB.kext"
old_fb_id = "com.hackintosh.RDNA4FB"
old_bringup = "RDNA4Bringup.kext"
old_bringup_id = "com.rdna4.bringup"
old_accel = "RDNA4Accel.kext"
old_accel_id = "com.rdna4.accelprobe"
lilu_id = "as.vit9696.Lilu"

installed_paths = [
    "Library/Extensions/Garnet.kext",
    "Library/Extensions/GarnetAccel.kext",
    "Library/GPUBundles/GarnetMetal.bundle",
    "Library/Application Support/RDNA4/native/activate-native.sh",
    "Library/Application Support/RDNA4/native/native-bundle-check",
    "Library/Application Support/RDNA4/native/publish-metal",
    "Library/Application Support/RDNA4/native/mtlprobe-direct",
    "Library/Application Support/RDNA4/native/garnettest",
    "Library/Application Support/RDNA4/native/native-translate",
    "Library/Application Support/RDNA4/native/profile",
]
legacy_paths = [
    "Library/LaunchDaemons/com.navi44.native.plist",
    "Library/LaunchDaemons/com.navi44.translate.plist",
    "Library/LaunchDaemons/com.navi48.translate.plist",
    "Library/LaunchDaemons/com.rdna4.translate.plist",
    "Library/LaunchDaemons/com.rdna4.native.plist",
    "Library/LaunchDaemons/com.rdna4.native-translate.plist",
    "Library/Extensions/Navi48Accel.kext",
    "Library/Extensions/Navi48Bringup.kext",
    "Library/GPUBundles/Navi48Metal.bundle",
    "Library/Extensions/RDNA4Accel.kext",
    "Library/Extensions/RDNA4Bringup.kext",
    "Library/Extensions/RDNA4FB.kext",
    "Library/Extensions/AMDRadeonRDNA4.kext",
    "Library/GPUBundles/RDNA4Metal.bundle",
    "usr/local/navi48",
    "usr/local/rdna4",
    "Library/Application Support/Navi44/native",
    "Library/Application Support/RDNA4/native/rdna4test",
]
caches = ["private/tmp/n48m", "private/tmp/n48m-headless-no", "private/var/tmp/n48m-spv"]


def dump(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(plistlib.dumps(data))


def load(path):
    return plistlib.loads(path.read_bytes())


def kext(path, identifier, executable, version="0.0.1", libraries=None):
    info = {
        "CFBundleIdentifier": identifier,
        "CFBundleExecutable": executable,
        "CFBundleVersion": version,
    }
    if libraries:
        info["OSBundleLibraries"] = libraries
    dump(path / "Contents/Info.plist", info)
    binary = path / "Contents/MacOS" / executable
    binary.parent.mkdir(parents=True, exist_ok=True)
    binary.write_text(identifier + "-fixture\n")
    return binary


def entry(name, enabled=True):
    executable = Path(name).stem
    return {
        "BundlePath": name,
        "Enabled": enabled,
        "ExecutablePath": "Contents/MacOS/" + executable,
        "PlistPath": "Contents/Info.plist",
    }


def config_entries():
    return load(config_path)["Kernel"]["Add"]


def by_name(entries, name):
    return [item for item in entries if item["BundlePath"] == name]


def aux_ids():
    return {item["CFBundleIdentifier"]: item["_PrelinkBundlePath"]
            for item in load(aux)["_PrelinkInfoDictionary"]}


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_checksums(payload_dir):
    files = sorted(path for path in payload_dir.rglob("*")
                   if path.is_file() and path.name != "SHA256SUMS")
    with (payload_dir / "SHA256SUMS").open("w") as stream:
        for path in files:
            stream.write(f"{sha(path)}  ./{path.relative_to(payload_dir).as_posix()}\n")


def write_manifest(payload_dir, garnet_standalone="true", accel_standalone="true", fmt="garnet-delivery-v1"):
    manifest = payload_dir / "MANIFEST.txt"
    manifest.write_text(
        f"FORMAT={fmt}\n"
        "# generated by the Garnet build delivery script\n"
        f"GARNET_BUNDLE_ID={garnet_id}\n"
        "GARNET_VERSION=0.0.673\n"
        f"GARNET_EXECUTABLE_SHA256={sha(payload_dir / garnet / 'Contents/MacOS/Garnet')}\n"
        f"GARNET_INFO_SHA256={sha(payload_dir / garnet / 'Contents/Info.plist')}\n"
        f"GARNET_STANDALONE={garnet_standalone}\n"
        f"ACCEL_BUNDLE_ID={accel_id}\n"
        "ACCEL_VERSION=0.0.8\n"
        f"ACCEL_EXECUTABLE_SHA256={sha(payload_dir / accel / 'Contents/MacOS/GarnetAccel')}\n"
        f"ACCEL_INFO_SHA256={sha(payload_dir / accel / 'Contents/Info.plist')}\n"
        f"ACCEL_STANDALONE={accel_standalone}\n"
        "UPSTREAM_COMMIT=fixture\n"
    )


def snapshot(excludes=(), metadata=False):
    rows = []
    for path in sorted(root.rglob("*")):
        rel = path.relative_to(root).as_posix()
        if any(rel == item or rel.startswith(item + "/") for item in excludes):
            continue
        digest = sha(path) if path.is_file() else "-"
        info = path.lstat()
        row = f"{rel}\t{info.st_mode:o}\t{digest}"
        if metadata:
            row += f"\t{info.st_uid}\t{info.st_gid}\t{info.st_mtime_ns}"
        rows.append(row)
    return "\n".join(rows) + "\n"


def protected_signature():
    rows = []
    for base in (
            esp / "Kexts/Lilu.kext",
            esp / "Kexts/WhateverGreen.kext",
            root / "Library/Extensions/Foreign.kext"):
        if not base.exists():
            continue
        for path in [base, *sorted(base.rglob("*"))]:
            info = path.lstat()
            if path.is_symlink():
                digest = "link:" + os.readlink(path)
            elif path.is_file():
                digest = sha(path)
            else:
                digest = "-"
            rows.append(
                f"{path.relative_to(root).as_posix()}\t{info.st_mode:o}\t"
                f"{info.st_uid}\t{info.st_gid}\t{info.st_mtime_ns}\t{digest}")
    return "\n".join(rows) + "\n"


def fixture():
    for rel in ("private/tmp", "private/var/tmp", "Library/Extensions",
                "Library/LaunchDaemons", "Library/KernelCollections"):
        (root / rel).mkdir(parents=True, exist_ok=True)
    payload.mkdir(parents=True)
    for name in ("install-native.sh", "uninstall-native.sh", "deploy-common.sh",
                 "activate-native.sh", "com.garnet.native.plist",
                 "com.garnet.native-translate.plist"):
        shutil.copy2(Path(sys.argv[3]) / name, payload / name)
    for name in ("native-translate", "native-bundle-check", "publish-metal",
                 "mtlprobe-direct", "garnettest"):
        path = payload / name
        path.write_text("#!/bin/sh\nexit 0\n")
        path.chmod(0o755)

    kext(payload / garnet, garnet_id, "Garnet", "0.0.673",
         {"com.apple.iokit.IOPCIFamily": "2.9"})
    kext(payload / accel, accel_id, "GarnetAccel", "0.0.8",
         {"com.apple.iokit.IOAcceleratorFamily2": "400.0"})
    bundle = base_arg / metal
    kext(bundle, metal_id, "GarnetMetal", "43")
    (bundle / "fixture").write_text("metal fixture\n")
    with tarfile.open(payload / (metal + ".tar"), "w") as archive:
        archive.add(bundle, arcname=bundle.name)

    esp.mkdir(parents=True)
    (esp / "OpenCore.efi").write_text("OpenCore fixture\n")
    lilu = esp / "Kexts/Lilu.kext"
    lilu_bin = kext(lilu, lilu_id, "Lilu", "1.7.2")
    kext(esp / "Kexts/WhateverGreen.kext", "as.vit9696.WhateverGreen",
         "WhateverGreen", "1.6.9")
    kext(esp / "Kexts" / old_display, old_display_id, "AMDRadeonRDNA4")
    kext(esp / "Kexts" / old_fb, old_fb_id, "RDNA4FB")
    kext(esp / "Kexts" / old_bringup, old_bringup_id, "RDNA4Bringup")
    kext(esp / "Kexts/Navi48Bringup.kext", "com.navi48.bringup", "Navi48Bringup")

    entries = [
        entry("WhateverGreen.kext"),
        entry("Lilu.kext"),
        entry(garnet, False),
        entry(old_bringup),
        entry(old_display),
        entry(old_fb),
        entry(accel),
        entry(old_accel),
    ]
    dump(config_path, {
        "Kernel": {"Add": entries},
        "NVRAM": {
            "Add": {guid: {
                "boot-args": "-v keepsyms=1 npci=0x2000 navi48bringup=1 navi48-stage=17 navi48-apps-all=1 rdna4-apps-all=1 amfi=0x80",
                "csr-active-config": b"\0\0\0\0",
            }},
            "Delete": {guid: []},
        },
    })

    kext(root / "Library/Extensions" / old_fb, old_fb_id, "RDNA4FB")
    kext(root / "Library/Extensions" / old_display, old_display_id, "AMDRadeonRDNA4")
    kext(root / "Library/Extensions" / old_bringup, old_bringup_id, "RDNA4Bringup")
    kext(root / "Library/Extensions" / old_accel, old_accel_id, "RDNA4Accel")
    kext(root / "Library/Extensions/Navi48Bringup.kext", "com.navi48.bringup", "Navi48Bringup")
    kext(root / "Library/Extensions/Navi48Accel.kext", "com.navi48.accelprobe", "Navi48Accel")
    kext(root / "Library/GPUBundles/Navi48Metal.bundle", "com.navi48.metal", "Navi48Metal")
    kext(root / "Library/GPUBundles/RDNA4Metal.bundle", "com.rdna4.metal", "RDNA4Metal")
    foreign = root / "Library/Extensions/Foreign.kext"
    kext(foreign, "com.example.foreign", "Foreign")
    aux_residual = esp / "Kexts" / garnet
    dump(aux, {"_PrelinkInfoDictionary": [
        {"CFBundleIdentifier": identifier, "_PrelinkBundlePath": str(path)}
        for identifier, path in (
            ("com.navi48.accelprobe", root / "Library/Extensions/Navi48Accel.kext"),
            (old_fb_id, root / "Library/Extensions" / old_fb),
            (old_display_id, root / "Library/Extensions" / old_display),
            (old_bringup_id, root / "Library/Extensions" / old_bringup),
            (old_accel_id, root / "Library/Extensions" / old_accel),
            (garnet_id, aux_residual),
            ("com.example.foreign", foreign),
        )
    ]})

    for rel in legacy_paths:
        path = root / rel
        if path.exists():
            continue
        if rel.endswith(".plist"):
            dump(path, {"Label": path.stem})
        elif rel.endswith("rdna4test"):
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("old rdna4test\n")
            path.chmod(0o755)
        else:
            path.mkdir(parents=True)
            (path / "x").write_text("legacy fixture\n")
    for rel in caches:
        (root / rel).mkdir(parents=True)
    (root / "private/tmp/n48m/1").mkdir()
    (root / "private/tmp/n48m/1/a.air").write_text("cache\n")

    for name in ("com.garnet.native.plist", "com.garnet.native-translate.plist"):
        dump(root / "Library/LaunchDaemons" / name, {"Label": Path(name).stem})
    support.mkdir(parents=True, exist_ok=True)
    for name in ("activate-native.sh", "native-bundle-check", "publish-metal",
                 "mtlprobe-direct", "garnettest", "native-translate"):
        path = support / name
        path.write_text("old userspace fixture\n")
        path.chmod(0o755)
    (support / "profile").write_text("native\n")
    (root / "Library/Logs/RDNA4Translate").mkdir(parents=True)
    (root / "Library/Logs/RDNA4Translate/translate.stdout").write_text("old stdout\n")
    (root / "Library/Logs/RDNA4Translate/translate.stderr").write_text("old stderr\n")

    write_manifest(payload)
    write_checksums(payload)

    shutil.copy2(config_path, base_arg / "config-before.plist")
    shutil.copy2(aux, base_arg / "aux-before.kc")
    (base_arg / "lilu-before.sha").write_text(sha(lilu_bin) + "\n")
    (base_arg / "protected-before").write_text(protected_signature())


if mode == "fixture":
    fixture()
elif mode == "snapshot":
    sys.stdout.write(snapshot(
        metadata=os.environ.get("RDNA4_TEST_SNAPSHOT_METADATA") == "1"))
elif mode == "rollback":
    expected = (base_arg / "before").read_text()
    excludes = [item for item in sys.argv[3:] if item]
    actual = snapshot(excludes)
    if actual != expected:
        import difflib
        sys.stdout.writelines(difflib.unified_diff(
            expected.splitlines(True), actual.splitlines(True),
            fromfile="before", tofile="after"))
        sys.exit(1)
elif mode == "check-plan":
    text = (base_arg / "output").read_text()
    boot = next(line[10:] for line in text.splitlines() if line.startswith("BOOT_ARGS="))
    tokens = boot.split()
    for token in ("garnet=1", "garnet-interrupts=0", "garnet-stage=17", "garnet-native=1", "garnet-metal=1"):
        assert token in tokens, (token, boot)
    for forbidden in ("garnet-metal-disp=1", "garnet-metal-ws=1", "garnet-apps=1",
                      "garnet-multisession=1", "amfi=0x80", "navi48", "rdna4"):
        assert not any(token.startswith(forbidden) for token in tokens), (forbidden, boot)
    assert "KERNEL_REMOVE=Garnet.kext" in text
    assert "AUTOMATIC_ACTIVATION=disabled" in text
    assert "CHANGES_TARGET=no" in text
    assert "AUX_KEEP=Garnet.kext GarnetAccel.kext" in text
    assert "AUX_REMOVE=" + garnet_id in text
    assert "AUX_REMOVE=" + old_display_id in text
    assert "AUX_REMOVE=" + old_fb_id in text
    assert "AUX_REMOVE=" + old_bringup_id in text
    assert "LEGACY_REMOVE=" + str(esp / "Kexts" / old_display) in text
    assert "LEGACY_REMOVE=" + str(esp / "Kexts" / old_fb) in text
    assert "LEGACY_REMOVE=" + str(esp / "Kexts" / old_bringup) in text
elif mode == "installed":
    lilu_required = sys.argv[3] if len(sys.argv) > 3 else "with-lilu"
    config = load(config_path)
    entries = config["Kernel"]["Add"]
    before = load(base_arg / "config-before.plist")["Kernel"]["Add"]
    whatever = by_name(entries, "WhateverGreen.kext")
    assert len(whatever) == 1 and whatever[0] == by_name(before, "WhateverGreen.kext")[0]
    if lilu_required == "with-lilu":
        lilu = by_name(entries, "Lilu.kext")
        assert len(lilu) == 1 and lilu[0] == by_name(before, "Lilu.kext")[0]
        assert protected_signature() == (base_arg / "protected-before").read_text()
        assert sha(esp / "Kexts/Lilu.kext/Contents/MacOS/Lilu") == \
            (base_arg / "lilu-before.sha").read_text().strip()
    assert not by_name(entries, garnet)
    assert not by_name(entries, old_display)
    assert not by_name(entries, old_fb)
    bringup = by_name(entries, old_bringup)
    assert len(bringup) == 1 and bringup[0]["Enabled"] is False
    accel_entries = by_name(entries, accel)
    assert len(accel_entries) == 1 and accel_entries[0]["Enabled"] is False
    old_accel_entries = by_name(entries, old_accel)
    assert len(old_accel_entries) == 1 and old_accel_entries[0]["Enabled"] is False
    tokens = config["NVRAM"]["Add"][guid]["boot-args"].split()
    for token in ("garnet=1", "garnet-interrupts=0", "garnet-stage=17", "garnet-native=1", "garnet-metal=1"):
        assert token in tokens, (token, tokens)
    assert "garnet-metal-disp=1" not in tokens and "garnet-metal-ws=1" not in tokens
    assert "garnet-apps=1" not in tokens and "garnet-multisession=1" not in tokens
    assert config["NVRAM"]["Add"][guid]["csr-active-config"] == b"\x03\x08\x00\x00"
    assert {"boot-args", "csr-active-config"} <= set(config["NVRAM"]["Delete"][guid])
    for rel in installed_paths:
        assert (root / rel).exists(), ("missing installed path", rel)
    assert support.stat().st_mode & 0o777 == 0o755
    assert support.parent.stat().st_mode & 0o777 == 0o755, "nobody cannot traverse the support parent"
    assert (root / "Library/Logs/RDNA4").stat().st_mode & 0o777 == 0o755
    assert (root / "Library/Logs/RDNA4Translate").stat().st_mode & 0o777 == 0o755
    for gone in (esp / "Kexts" / garnet, esp / "Kexts" / old_display, esp / "Kexts" / old_fb,
                 esp / "Kexts" / old_bringup, esp / "Kexts/Navi48Bringup.kext",
                 root / "Library/LaunchDaemons/com.garnet.native.plist",
                 root / "Library/LaunchDaemons/com.garnet.native-translate.plist"):
        assert not gone.exists(), ("should be removed", gone)
    assert (support / "profile").read_text() == "controlled\n"
    for rel in legacy_paths + caches:
        assert not (root / rel).exists(), ("legacy remains", rel)
    ids = aux_ids()
    assert set(ids) == {garnet_id, accel_id, "com.example.foreign"}, ids
    assert (root / "Library/Extensions/Foreign.kext/Contents/Info.plist").is_file()
    backup, = (support / "backups").glob("install.*")
    assert (backup / "config-before.plist").read_bytes() == (base_arg / "config-before.plist").read_bytes()
    assert (backup / "AuxiliaryKernelExtensions.kc").read_bytes() == (base_arg / "aux-before.kc").read_bytes()
    assert any(path.name.endswith("-" + old_display) for path in backup.rglob("*"))
    trace = (base_arg / "trace").read_text()
    assert "launchctl bootout system/com.garnet.native" in trace
    assert "launchctl bootout system/com.garnet.native-translate" in trace
elif mode == "uninstall-plan":
    text = (base_arg / "output").read_text()
    for rel in installed_paths:
        assert "REMOVE=" + str(root / rel) in text, rel
    assert "KERNEL_REMOVE=Garnet.kext" in text
    assert "KERNEL_DISABLE=GarnetAccel.kext" in text
    assert "CHANGES_TARGET=no" in text
elif mode == "uninstalled":
    config = load(config_path)
    entries = config["Kernel"]["Add"]
    assert not by_name(entries, garnet)
    assert not by_name(entries, old_display)
    assert not by_name(entries, old_fb)
    assert by_name(entries, "Lilu.kext")
    assert by_name(entries, "WhateverGreen.kext")
    assert by_name(entries, old_bringup)[0]["Enabled"] is False
    assert by_name(entries, accel)[0]["Enabled"] is False
    assert config["NVRAM"]["Add"][guid]["boot-args"].split() == [
        "-v", "keepsyms=1", "npci=0x2000"]
    for rel in installed_paths:
        assert not (root / rel).exists(), ("uninstall left path", rel)
    assert (esp / "Kexts/Lilu.kext").is_dir()
    assert (esp / "Kexts/WhateverGreen.kext").is_dir()
    assert (root / "Library/Extensions/Foreign.kext").is_dir()
    assert set(aux_ids()) == {"com.example.foreign"}
    backup, = (root / "Library/Application Support/RDNA4-backups").glob("uninstall.*")
    assert any(path.name.endswith("-" + garnet) for path in backup.rglob("*"))
    assert any(path.name.endswith("-" + accel) for path in backup.rglob("*"))
    assert sha(esp / "Kexts/Lilu.kext/Contents/MacOS/Lilu") == \
        (base_arg / "lilu-before.sha").read_text().strip()
    assert protected_signature() == (base_arg / "protected-before").read_text()
elif mode == "legacy-only":
    config = load(config_path)
    entries = config["Kernel"]["Add"]
    assert not by_name(entries, garnet)
    assert not by_name(entries, old_display)
    assert not by_name(entries, old_fb)
    assert config["NVRAM"]["Add"][guid]["boot-args"] == "-v garnet=1 garnet-stage=17 garnet-metal=1"
    for rel in legacy_paths + caches:
        assert not (root / rel).exists(), ("legacy remains", rel)
    backup, = (root / "Library/Application Support/RDNA4-backups").glob("uninstall.*")
    assert (backup / "config-before.plist").is_file()
elif mode == "foreign-current":
    component = sys.argv[3]
    targets = {
        "garnet": (esp / "Kexts/Garnet.kext", "Garnet"),
        "residual-garnet": (root / "Library/Extensions/Garnet.kext", "Garnet"),
        "accel": (root / "Library/Extensions/GarnetAccel.kext", "GarnetAccel"),
        "metal": (root / "Library/GPUBundles/GarnetMetal.bundle", "GarnetMetal"),
        "esp-old-display": (esp / "Kexts/AMDRadeonRDNA4.kext", "AMDRadeonRDNA4"),
        "esp-old-bringup": (esp / "Kexts/RDNA4Bringup.kext", "RDNA4Bringup"),
        "esp-old-fb": (esp / "Kexts/RDNA4FB.kext", "RDNA4FB"),
        "esp-navi48-bringup": (esp / "Kexts/Navi48Bringup.kext", "Navi48Bringup"),
    }
    bundle, executable = targets[component]
    shutil.rmtree(bundle, ignore_errors=True)
    kext(bundle, "com.example.foreign", executable)
elif mode == "foreign-payload-garnet":
    target = payload / garnet
    shutil.rmtree(target)
    kext(target, "com.example.foreign", "Garnet", "0.0.673")
    write_checksums(payload)
elif mode == "add-lilu-dep":
    bundle_name = sys.argv[3]
    info_path = payload / bundle_name / "Contents/Info.plist"
    info = load(info_path)
    info["OSBundleLibraries"] = {lilu_id: "1.7.2"}
    dump(info_path, info)
    write_manifest(payload)
    write_checksums(payload)
elif mode == "omit-manifest-checksum":
    sums = payload / "SHA256SUMS"
    lines = [line for line in sums.read_text().splitlines(keepends=True)
             if not line.endswith("  ./MANIFEST.txt\n")]
    assert len(lines) == len(sums.read_text().splitlines()) - 1
    sums.write_text("".join(lines))
elif mode == "manifest-not-standalone":
    write_manifest(payload, garnet_standalone="false")
    write_checksums(payload)
elif mode == "drop-lilu-from-esp":
    shutil.rmtree(esp / "Kexts/Lilu.kext")
    cfg = load(config_path)
    cfg["Kernel"]["Add"] = [item for item in cfg["Kernel"]["Add"] if item["BundlePath"] != "Lilu.kext"]
    dump(config_path, cfg)
    shutil.copy2(config_path, base_arg / "config-before.plist")
elif mode == "uncommitted-exit":
    script = payload / (sys.argv[3] + "-native.sh")
    text = script.read_text()
    assert text.count("trap rollback EXIT\n") == 1
    script.write_text(text.replace("trap rollback EXIT\n", "trap rollback EXIT\nexit 0\n"))
    write_checksums(payload)
else:
    sys.exit("unknown assertion mode: " + mode)
PY

check() { python3 "$work/check.py" "$1" "$base" "${@:2}"; }
fresh() {
    base="$work/$1"
    printf '%s\n' "$base" > "$work/last-base"
    python3 "$work/check.py" fixture "$base" "$source_dir/.."
    export RDNA4_DEPLOY_ROOT=$(cd "$base/root" && pwd -P)
    export RDNA4_DEPLOY_TEST_TRACE="$base/trace"
    : > "$RDNA4_DEPLOY_TEST_TRACE"
    unset RDNA4_DEPLOY_STUB_FAIL RDNA4_DEPLOY_TEST_VMM RDNA4_DEPLOY_TEST_LOADED_JOBS
}
before() { check snapshot > "$base/before"; }
before_check() { RDNA4_TEST_SNAPSHOT_METADATA=1 check snapshot > "$base/before"; }
after_check() { RDNA4_TEST_SNAPSHOT_METADATA=1 check snapshot > "$base/after"; }
assert_check_unchanged() {
    after_check
    cmp "$base/before" "$base/after"
}
deploy() {
    local script=$1
    shift
    bash "$base/payload/$script-native.sh" "$@" "$base/root/ESP" > "$base/output" 2>&1
}
expect_failure() {  # needle, command...
    local needle=$1 rc=0
    shift
    "$@" || rc=$?
    [ "$rc" -ne 0 ] || { printf 'command unexpectedly succeeded\n' >&2; return 1; }
    grep -Fq "$needle" "$base/output"
}

case_check() {
    fresh check
    before_check
    deploy install --check --controlled
    assert_check_unchanged
    check check-plan
}
case_install() {
    fresh install
    deploy install --controlled
    ! grep -Fq 'Installation failed' "$base/output"
    [ -f "$base/root/Library/GPUBundles/GarnetMetal.bundle/Contents/MacOS/GarnetMetal" ]
    check installed with-lilu
}
case_no_lilu_required() {
    fresh no-lilu-required
    check drop-lilu-from-esp
    before_check
    deploy install --check --controlled
    assert_check_unchanged
    deploy install --controlled
    check installed without-lilu
}
case_install_uncommitted_exit() {
    fresh install-uncommitted-exit
    check uncommitted-exit install
    before
    expect_failure 'Installation failed (exit 1)' deploy install --controlled
    check rollback 'Library/Application Support/RDNA4/native/backups'
}
case_uninstall_uncommitted_exit() {
    fresh uninstall-uncommitted-exit
    deploy install --controlled
    check uncommitted-exit uninstall
    before
    expect_failure 'Uninstall failed (exit 1)' deploy uninstall
    check rollback 'Library/Application Support/RDNA4-backups'
}
case_missing_garnet() {
    fresh missing-garnet
    rm -rf "$base/payload/Garnet.kext"
    before_check
    expect_failure 'Missing payload: Garnet.kext' deploy install --check --controlled
    assert_check_unchanged
}
case_lilu_dependency_rejected() {
    fresh lilu-dep-garnet
    check add-lilu-dep Garnet.kext
    before
    expect_failure 'Payload Garnet.kext must not declare a Lilu dependency' \
        deploy install --controlled
    check snapshot > "$base/after"
    cmp "$base/before" "$base/after"

    fresh lilu-dep-accel
    check add-lilu-dep GarnetAccel.kext
    before
    expect_failure 'Payload GarnetAccel.kext must not declare a Lilu dependency' \
        deploy install --controlled
    check snapshot > "$base/after"
    cmp "$base/before" "$base/after"
}
case_tampered_payload() {
    fresh tampered
    printf 'tampered\n' >> "$base/payload/native-translate"
    before
    expect_failure 'Payload checksum verification failed' deploy install --controlled
    check snapshot > "$base/after"
    cmp "$base/before" "$base/after"
}
case_install_rollback() {
    fresh install-rollback
    before
    export RDNA4_DEPLOY_STUB_FAIL=sync
    expect_failure 'Installation failed (exit 9)' deploy install --controlled
    unset RDNA4_DEPLOY_STUB_FAIL
    check rollback 'Library/Application Support/RDNA4/native/backups'
    grep -Fq 'ESP, config, boot-args, Aux KC and userspace restored' "$base/output"
}
case_aux_inspect_rollback() {
    fresh aux-inspect-rollback
    before
    export RDNA4_DEPLOY_STUB_FAIL=kmutil-inspect-second
    expect_failure 'Aux KC inspect failed; no existing extension was discarded' \
        deploy install --controlled
    unset RDNA4_DEPLOY_STUB_FAIL
    check rollback 'Library/Application Support/RDNA4/native/backups'
    grep -Fq 'ESP, config, boot-args, Aux KC and userspace restored' "$base/output"
    [ "$(cat "$base/trace.inspect-count")" = 2 ]
    [ -f "$base/root/Library/Extensions/Foreign.kext/Contents/Info.plist" ]
}
case_loaded_job_rollback() {
    fresh loaded-job-rollback
    export RDNA4_DEPLOY_TEST_LOADED_JOBS='com.garnet.native com.rdna4.native com.navi44.native'
    before
    export RDNA4_DEPLOY_STUB_FAIL=sync
    expect_failure 'Installation failed (exit 9)' deploy install --controlled
    unset RDNA4_DEPLOY_STUB_FAIL RDNA4_DEPLOY_TEST_LOADED_JOBS
    check rollback 'Library/Application Support/RDNA4/native/backups'
    grep -Fq 'launchctl bootout system/com.garnet.native' "$base/trace"
    grep -Fq 'launchctl bootout system/com.rdna4.native' "$base/trace"
    grep -Fq 'launchctl bootout system/com.navi44.native' "$base/trace"
    grep '^launchctl bootstrap ' "$base/trace" > "$base/bootstrap"
    [ "$(wc -l < "$base/bootstrap" | tr -d ' ')" = 3 ]
    grep -Fq 'com.garnet.native.plist' "$base/bootstrap"
    grep -Fq 'com.rdna4.native.plist' "$base/bootstrap"
    grep -Fq 'com.navi44.native.plist' "$base/bootstrap"
    ! grep -F 'com.garnet.native-translate.plist' "$base/bootstrap"
}
case_bad_legacy_identity() {
    fresh bad-legacy-identity
    python3 - "$base/root/Library/Extensions/Navi48Accel.kext/Contents/Info.plist" <<'PY'
import plistlib
import sys
path = sys.argv[1]
with open(path, "rb") as stream:
    info = plistlib.load(stream)
info["CFBundleIdentifier"] = "com.example.not-ours"
with open(path, "wb") as stream:
    plistlib.dump(info, stream)
PY
    before_check
    expect_failure 'legacy Navi48Accel CFBundleIdentifier is com.example.not-ours' \
        deploy install --check --controlled
    assert_check_unchanged
}
case_bad_current_component() {
    local component expected
    for component in garnet residual-garnet accel metal \
        esp-old-display esp-old-bringup esp-old-fb esp-navi48-bringup; do
        fresh "bad-current-$component"
        check foreign-current "$component"
        before
        case "$component" in
            garnet) expected='residual ESP Garnet CFBundleIdentifier is com.example.foreign, expected org.garnet.driver';;
            residual-garnet) expected='existing Library Garnet CFBundleIdentifier is com.example.foreign, expected org.garnet.driver';;
            accel) expected='existing Library GarnetAccel CFBundleIdentifier is com.example.foreign, expected org.garnet.accel';;
            metal) expected='existing Library GarnetMetal CFBundleIdentifier is com.example.foreign, expected org.garnet.metal';;
            esp-old-display) expected='legacy ESP AMDRadeonRDNA4 CFBundleIdentifier is com.example.foreign, expected com.hackintosh.AMDRadeonRDNA4';;
            esp-old-bringup) expected='legacy ESP RDNA4Bringup CFBundleIdentifier is com.example.foreign, expected com.rdna4.bringup';;
            esp-old-fb) expected='legacy ESP RDNA4FB CFBundleIdentifier is com.example.foreign, expected com.hackintosh.RDNA4FB';;
            esp-navi48-bringup) expected='legacy ESP Navi48Bringup CFBundleIdentifier is com.example.foreign, expected com.navi48.bringup';;
        esac
        expect_failure "$expected" deploy install --controlled
        check snapshot > "$base/after"
        cmp "$base/before" "$base/after"
    done

    fresh bad-current-uninstall-metal
    deploy install --controlled
    check foreign-current metal
    before
    expect_failure \
        'Library GarnetMetal CFBundleIdentifier is com.example.foreign, expected org.garnet.metal' \
        deploy uninstall
    check snapshot > "$base/after"
    cmp "$base/before" "$base/after"
}
case_manifest_inputs() {
    fresh omitted-manifest-checksum
    check omit-manifest-checksum
    before_check
    expect_failure 'Payload checksum manifest does not cover every payload file' \
        deploy install --check --controlled
    assert_check_unchanged

    fresh bad-manifest-standalone
    check manifest-not-standalone
    before_check
    expect_failure 'Manifest does not record standalone Garnet kexts' \
        deploy install --check --controlled
    assert_check_unchanged
}
case_bad_payload_identity() {
    fresh foreign-payload-garnet
    check foreign-payload-garnet
    before_check
    expect_failure \
        'payload Garnet.kext CFBundleIdentifier is com.example.foreign, expected org.garnet.driver' \
        deploy install --check --controlled
    assert_check_unchanged
}
case_aux_preservation() {
    fresh aux-drop-foreign-install
    before
    export RDNA4_DEPLOY_STUB_FAIL=kmutil-create-drop-foreign
    expect_failure 'Aux KC rebuild did not preserve the requested extension set' \
        deploy install --controlled
    unset RDNA4_DEPLOY_STUB_FAIL
    check rollback 'Library/Application Support/RDNA4/native/backups'
    [ -f "$base/root/Library/Extensions/Foreign.kext/Contents/Info.plist" ]

    fresh aux-drop-foreign-uninstall
    deploy install --controlled
    : > "$base/trace"
    export RDNA4_DEPLOY_TEST_LOADED_JOBS=com.garnet.native
    before
    export RDNA4_DEPLOY_STUB_FAIL=kmutil-create-drop-foreign
    expect_failure 'Aux KC rebuild did not preserve the requested extension set' \
        deploy uninstall
    unset RDNA4_DEPLOY_STUB_FAIL RDNA4_DEPLOY_TEST_LOADED_JOBS
    ! grep -Fq 'launchctl bootout system/com.garnet.native' "$base/trace"
    check rollback 'Library/Application Support/RDNA4-backups'
    [ -f "$base/root/Library/Extensions/Foreign.kext/Contents/Info.plist" ]
}
case_uninstall_check() {
    fresh uninstall-check
    deploy install --controlled
    check installed with-lilu
    before_check
    deploy uninstall --check
    assert_check_unchanged
    check uninstall-plan
}
case_uninstall() {
    fresh uninstall
    deploy install --controlled
    deploy uninstall
    check uninstalled
}
case_uninstall_rollback() {
    fresh uninstall-rollback
    deploy install --controlled
    before
    export RDNA4_DEPLOY_STUB_FAIL=sync
    expect_failure 'Uninstall failed (exit 9)' deploy uninstall
    unset RDNA4_DEPLOY_STUB_FAIL
    check rollback 'Library/Application Support/RDNA4-backups'
}
case_uninstall_preboot_failure() {
    fresh uninstall-preboot
    deploy install
    : > "$base/trace"
    export RDNA4_DEPLOY_TEST_LOADED_JOBS=com.garnet.native
    before
    export RDNA4_DEPLOY_STUB_FAIL=kmutil-create
    expect_failure 'Aux KC rebuild failed (9)' deploy uninstall
    unset RDNA4_DEPLOY_STUB_FAIL RDNA4_DEPLOY_TEST_LOADED_JOBS
    ! grep -Fq 'launchctl bootout system/com.garnet.native' "$base/trace"
    check rollback 'Library/Application Support/RDNA4-backups'
}
case_vm_refusal() {
    fresh vm-refusal
    before_check
    export RDNA4_DEPLOY_TEST_VMM=1
    expect_failure 'Actual installation in a VM is disabled' deploy install --controlled
    unset RDNA4_DEPLOY_TEST_VMM
    assert_check_unchanged
    cmp "$base/config-before.plist" "$base/root/ESP/EFI/OC/config.plist"
}
case_legacy_only() {
    fresh legacy-only
    deploy install --controlled
    python3 - "$base/root/ESP/EFI/OC/config.plist" "$guid" <<'PY'
import plistlib
import sys
path, guid = sys.argv[1:]
with open(path, "rb") as stream:
    config = plistlib.load(stream)
config["NVRAM"]["Add"][guid]["boot-args"] = "-v navi48bringup=1 navi48-stage=17 rdna4-metal=1"
with open(path, "wb") as stream:
    plistlib.dump(config, stream)
PY
    deploy uninstall --legacy-only
    check legacy-only
}

base=
failures=0
run_case() {
    local name=$1 fn=$2 rc=0
    case "$name" in ${RDNA4_DEPLOY_TEST_CASE:-*}) ;; *) return 0;; esac
    set +e
    (set -e; "$fn") > "$work/case-output" 2>&1
    rc=$?
    set -e
    if [ "$rc" -eq 0 ]; then
        printf 'CASE_PASS %s\n' "$name"
    else
        local failed_base=
        [ ! -f "$work/last-base" ] || failed_base=$(cat "$work/last-base")
        printf 'CASE_FAIL %s exit=%s\n' "$name" "$rc"
        cat "$work/case-output"
        [ -z "$failed_base" ] || [ ! -f "$failed_base/output" ] || cat "$failed_base/output"
        failures=$((failures + 1))
    fi
}
run_case a-install-check case_check
run_case b-controlled-install-order-deps-cleanup case_install
run_case b2-install-uncommitted-exit case_install_uncommitted_exit
run_case c-no-lilu-required case_no_lilu_required
run_case d-missing-garnet case_missing_garnet
run_case e-lilu-dependency-rejected case_lilu_dependency_rejected
run_case f-tampered-payload case_tampered_payload
run_case g-install-rollback case_install_rollback
run_case h-aux-inspect-rollback case_aux_inspect_rollback
run_case i-loaded-job-rollback case_loaded_job_rollback
run_case j-bad-legacy-identity case_bad_legacy_identity
run_case j2-bad-current-component case_bad_current_component
run_case k-uninstall-check case_uninstall_check
run_case l-uninstall case_uninstall
run_case m-uninstall-rollback case_uninstall_rollback
run_case m2-uninstall-uncommitted-exit case_uninstall_uncommitted_exit
run_case n-uninstall-preboot-failure case_uninstall_preboot_failure
run_case o-vm-refusal case_vm_refusal
run_case p-legacy-only case_legacy_only
run_case q-manifest-inputs case_manifest_inputs
run_case r-payload-identity case_bad_payload_identity
run_case s-aux-preservation case_aux_preservation
printf 'DEPLOY_SUITE cases=22 filter=%s failures=%s hardware_commands=stubbed installation=not_performed\n' "${RDNA4_DEPLOY_TEST_CASE:-all}" "$failures"
[ "$failures" -eq 0 ]
