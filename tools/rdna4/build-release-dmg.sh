#!/bin/bash
# Build a self-contained, ad-hoc signed macOS .dmg installer for Garnet Release 1 (Navi 44).
# Usage:
#   bash tools/rdna4/build-release-dmg.sh <release_payload_dir> <out_dir> [version]
set -euo pipefail
umask 022

usage() {
    cat <<'EOF'
Usage: build-release-dmg.sh <release_payload_dir> <out_dir> [version]
  <release_payload_dir>  Verified RDNA4_RELEASE=1 payload directory
  <out_dir>              Output directory for .dmg and .dmg.sha256
  [version]              Optional override (defaults to GARNET_VERSION in MANIFEST.txt)
EOF
}

if [ "${1:-}" = "-h" ] || [ "${1:-}" = "--help" ]; then
    usage
    exit 0
fi
[ "$#" -ge 2 ] && [ "$#" -le 3 ] || { usage >&2; exit 2; }
[ "$(uname -s)" = Darwin ] || { echo "build-release-dmg.sh: must run on macOS (requires swiftc, codesign, hdiutil)" >&2; exit 1; }

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/../.." && pwd)
PAYLOAD_DIR=$(cd "$1" && pwd)
OUT_DIR=$2
mkdir -p "$OUT_DIR"
OUT_DIR=$(cd "$OUT_DIR" && pwd)

MANIFEST="$PAYLOAD_DIR/MANIFEST.txt"
[ -f "$MANIFEST" ] || { echo "Missing MANIFEST.txt in $PAYLOAD_DIR" >&2; exit 1; }

VERSION=${3:-$(awk -F= '$1=="GARNET_VERSION"{print $2; exit}' "$MANIFEST")}
[ -n "$VERSION" ] || { echo "Cannot determine GARNET_VERSION from $MANIFEST" >&2; exit 1; }

echo "build-release-dmg: verifying Release 1 payload $PAYLOAD_DIR..."
python3 -B "$REPO_ROOT/tools/rdna4/build_identity.py" verify-release-payload "$PAYLOAD_DIR"

for req_file in \
    "$REPO_ROOT/LICENSE" \
    "$REPO_ROOT/third-party/THIRD-PARTY-LICENSES.txt" \
    "$PAYLOAD_DIR/LICENSE.amdgpu"
do
    [ -s "$req_file" ] || { echo "Required license file missing or empty: $req_file" >&2; exit 1; }
done

STAGE=$(mktemp -d "${TMPDIR:-/tmp}/garnet-dmg.XXXXXXXX")
cleanup() { rm -rf "$STAGE"; }
trap cleanup EXIT

APP_BUILD="$STAGE/app-build"
echo "build-release-dmg: building x86_64 Garnet.app + garnetctl (with embedded Release 1 payload)..."
make -C "$REPO_ROOT/src/garnet-app" clean all \
    GARNET_EMBED_PAYLOAD="$PAYLOAD_DIR" \
    BUILD="$APP_BUILD"

codesign --verify --deep --strict "$APP_BUILD/Garnet.app"
codesign --verify --strict "$APP_BUILD/Garnet.app/Contents/Resources/garnetctl"
(cd "$APP_BUILD/Garnet.app/Contents/Resources/Payload" && shasum -a 256 -c SHA256SUMS >/dev/null) \
    || { echo "build-release-dmg: embedded Payload SHA256SUMS verification failed after app signing" >&2; exit 1; }

DMG_ROOT="$STAGE/dmg-root"
mkdir -p "$DMG_ROOT"
cp -R "$APP_BUILD/Garnet.app" "$DMG_ROOT/Garnet.app"
ln -s /Applications "$DMG_ROOT/Applications"
cp "$REPO_ROOT/LICENSE" "$DMG_ROOT/LICENSE"
cp "$PAYLOAD_DIR/LICENSE.amdgpu" "$DMG_ROOT/LICENSE.amdgpu"
cp "$REPO_ROOT/third-party/THIRD-PARTY-LICENSES.txt" "$DMG_ROOT/THIRD-PARTY-LICENSES.txt"

cat > "$DMG_ROOT/INSTALL-QUICKSTART.txt" <<EOF
Garnet RDNA 4 Driver v${VERSION} — Release 1 (Radeon RX 9060 XT / Navi 44, macOS 26.x x86_64)
=========================================================================================

1. Hardware & BIOS Prerequisites
   - Supported GPU in Release 1: AMD Radeon RX 9060 XT (Navi 44, PCI ID 1002:7590) on bare-metal x86_64 macOS 26.x.
   - Motherboard BIOS: Enable "Above 4G Decoding" and "Re-Size BAR Support", Disable CSM.

2. Gatekeeper Launch (Ad-Hoc Signed App)
   - Drag Garnet.app to /Applications (or run directly from this volume after removing quarantine):
       xattr -dr com.apple.quarantine /Applications/Garnet.app
   - Or right-click Garnet.app -> Open.

3. Strict Kext Separation (DO NOT mix these paths!)
   - OpenCore EFI/OC/Kexts/:  GarnetPCIBarCompat.kext (enabled for Full-BAR (8/16 GiB) default; disable/omit in 256 MiB Small-BAR fallback),
     and in AMFI trust-anchor mode (no amfi=0x80): Lilu.kext -> AMFIAnchor.kext -> GarnetAnchorInjector.kext (all before AMFIPass.kext if used).
   - OpenCore EFI/OC/Tools/:  GarnetRecover.efi (registered in Misc -> Tools for safe boot & one-click system-disk uninstall; zero ESP writes).
   - macOS /Library/Extensions/ (AuxKC): ONLY Garnet.kext + GarnetAccel.kext (installed & managed by Garnet.app / garnetctl).
   - Garnet never auto-mounts your EFI System Partition; OpenCore Assistant only updates or restores a config.plist you explicitly pick (with automatic backup).

4. Quick Install Steps
   a) Open Garnet.app -> "Install / Uninstall" -> check Preflight (and Full-BAR vs 256M Small-BAR recommendation).
   b) Mount your EFI partition, then open Garnet.app -> "OpenCore Assistant": select the recommended BAR & AMFI mode and click
      "Configure OpenCore..." (or use read-only Validate / Export). It preserves a one-time config.plist.garnet-pre-garnet baseline,
      copies GarnetPCIBarCompat.kext (Full-BAR), GarnetRecover.efi (Misc -> Tools), and GarnetAnchorInjector.kext (anchor mode),
      sets SecureBootModel = Disabled and csr-active-config = <03080000>, and validates the result.
   c) If Preflight showed "SIP SETUP NEEDED", reboot once so the updated csr-active-config takes effect.
   d) Click "Install / Upgrade Driver..." (Vulkan ICD stays OFF by default unless checked; upgrades from v0.1.0 automatically migrate app-denylist.txt to /Library/Application Support/Garnet/native and clean up legacy runtime files), then reboot.

5. Emergency Recovery & Kill-Switches
   - OpenCore Picker -> "Garnet: Safe Boot & Recovery" (GarnetRecover.efi):
       [1] Safe Boot Once (sets garnet=0 garnet-aux=0 garnet-rebar-compat=0 for this boot, keeps driver installed)
       [2] Safe Boot & Uninstall Garnet (uninstalls system-disk files on next macOS boot, stays on safe desktop; then mount EFI & use Restore OpenCore)
   - Manual boot-arg kill-switches:
       garnet=0               Disable Garnet primary driver completely
       garnet-aux=0           Disable GarnetAccel / framebuffer integration
       garnet-rebar-compat=0  Disable GarnetPCIBarCompat Full-BAR hook without removing the kext
       garnet-apps-all=0      Disable global app Metal acceleration while keeping core desktop & allowlisted tools

-----------------------------------------------------------------------------------------
快速安装提示（简体中文）
- 仅支持 x86_64 macOS 26.x 物理机 + RX 9060 XT (Navi 44, 1002:7590)；主板 BIOS 开启 Above 4G Decoding 与 Re-Size BAR，关闭 CSM。
  （注：RX 9060 XT 16GB 为实机已验证型号；RX 9060 XT 8GB 代码已就绪，待实机验证。）
- 首次打开如遇 Gatekeeper 提示，执行：xattr -dr com.apple.quarantine /Applications/Garnet.app 或右键“打开”。
- 严格区分路径：GarnetPCIBarCompat.kext（Full-BAR 默认启用）与信任锚模式下的 Lilu -> AMFIAnchor -> GarnetAnchorInjector.kext 仅放 EFI/OC/Kexts/；
  GarnetRecover.efi 放 EFI/OC/Tools/；Garnet.kext 与 GarnetAccel.kext 仅由 Garnet.app 装入系统盘 /Library/Extensions/ 并重建 AuxKC，严禁放入 OpenCore EFI。
- 手动挂载 EFI 分区后，可在“OpenCore Assistant”点击“Configure OpenCore...”自动保存首次基线备份并配置 config.plist（含恢复菜单项与可选无 amfi=0x80 信任锚模式），
  应急排障或回滚可在 OpenCore 启动菜单选择“Garnet: Safe Boot & Recovery”，安全进桌面后挂载 EFI 点击“Restore OpenCore...”还原配置；程序绝不自动挂载 ESP。
EOF

find "$DMG_ROOT" \( -name '.DS_Store' -o -name '._*' \) -delete
xattr -cr "$DMG_ROOT" 2>/dev/null || true

DMG_NAME="Garnet-${VERSION}-Navi44-macOS26.dmg"
DMG_PATH="$OUT_DIR/$DMG_NAME"
rm -f "$DMG_PATH" "$DMG_PATH.sha256"

echo "build-release-dmg: creating compressed UDZO image $DMG_PATH..."
hdiutil create \
    -volname "Garnet-${VERSION}-Navi44" \
    -srcfolder "$DMG_ROOT" \
    -ov -format UDZO \
    "$DMG_PATH"

echo "build-release-dmg: verifying DMG..."
hdiutil verify "$DMG_PATH"

(
    cd "$OUT_DIR"
    shasum -a 256 "$DMG_NAME" > "${DMG_NAME}.sha256"
)

echo "build-release-dmg: PASS"
echo "  DMG:    $DMG_PATH"
echo "  SHA256: $(cat "$DMG_PATH.sha256")"
