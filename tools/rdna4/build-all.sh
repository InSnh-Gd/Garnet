#!/bin/bash
# Build one matched standalone Garnet payload for install-native.sh; never install or load it.
# Usage: MKSDK=... C64_LAYOUT_OUT=... C64_WORK=... [other inputs] build-all.sh OUT_DIR
set -euo pipefail
export LC_ALL=C
export PYTHONDONTWRITEBYTECODE=1
fail() { printf 'build-all.sh: %s\n' "$*" >&2; exit 1; }
[ "$#" -eq 1 ] && [ -n "$1" ] || fail 'Usage: build-all.sh OUT_DIR'
repo=$(cd "$(dirname "$0")/../.." && pwd -P)
helper="$repo/tools/rdna4/build_identity.py"
normalize_input_label() {
    printf '%s' "$1" | tr '[:lower:]' '[:upper:]'
}
record_input() {
    local label target
    label=$(normalize_input_label "$1")
    target=$2
    shift 2
    python3 "$helper" input "$label" "$target" "$@" > "$work/input-$label.txt" \
        || fail "cannot record input identity: $label"
    cat "$work/input-$label.txt" >> "$input_identities"
}
if [ "$1" = --label-self-check ]; then
    command -v python3 >/dev/null 2>&1 || fail 'Required tool not found: python3'
    command -v tr >/dev/null 2>&1 || fail 'Required tool not found: tr'
    scratch_base=${TMPDIR:-$repo/temp/scratch}
    mkdir -p "$scratch_base"
    work=$(mktemp -d "${scratch_base%/}/garnet-build-labels.XXXXXX") \
        || fail 'cannot create label self-check directory'
    trap 'rm -rf "$work"' EXIT
    input_identities="$work/input-identities.txt"
    : > "$input_identities"
    printf 'fixture\n' > "$work/fixture"
    for name in Garnet GarnetAccel GarnetPCIBarCompat; do
        record_input "OUTPUT_${name}_EXECUTABLE" "$work/fixture"
        record_input "OUTPUT_${name}_INFO_PLIST" "$work/fixture"
    done
    for name in native-translate native-bundle-check publish-metal \
        mtlprobe-direct garnettest; do
        label=${name//-/_}
        record_input "OUTPUT_TOOL_${label}" "$work/fixture"
    done
    for label in OUTPUT_METAL_BUNDLE OUTPUT_METAL_EXECUTABLE OUTPUT_METAL_INFO \
        OUTPUT_METAL_RADV OUTPUT_METAL_XLATE OUTPUT_METAL_SPVCACHE \
        OUTPUT_METAL_UNSIGNED_EXECUTABLE PAYLOAD_COLLECTOR; do
        record_input "$label" "$work/fixture"
    done
    if grep -Ev '^[A-Z][A-Z0-9_]*\.' "$input_identities" >/dev/null; then
        fail 'label self-check produced a non-uppercase identity key'
    fi
    printf 'build-all label self-check passed\n'
    exit 0
fi
[ "$(uname -s)" = Darwin ] || fail 'macOS is required'
resolve_output() {
    local path=$1 suffix=
    case "$path" in /*) ;; *) path="$PWD/$path";; esac
    while [ ! -d "$path" ]; do
        [ ! -e "$path" ] && [ ! -L "$path" ] || fail "Not a directory: $path"
        suffix="/$(basename "$path")$suffix"
        path=$(dirname "$path")
    done
    path=$(cd "$path" && pwd -P)
    case "$suffix/" in */../*|*/./*) fail 'OUT_DIR must not contain unresolved . or .. components';; esac
    printf '%s%s\n' "${path%/}" "$suffix"
}
out=$(resolve_output "$1")
case "$out/" in "$repo/"*) fail 'OUT_DIR must be outside the repository';; esac

require_dir() { [ -n "$2" ] && [ -d "$2" ] || fail "$1 must name an existing directory: $2"; }
require_file() { [ -n "$2" ] && [ -f "$2" ] || fail "$1 must name an existing file: $2"; }
absolute_file() { printf '%s/%s\n' "$(cd "$(dirname "$1")" && pwd -P)" "$(basename "$1")"; }
plist_value() { /usr/libexec/PlistBuddy -c "Print :$2" "$1"; }
require_dir MKSDK "${MKSDK:-}"
require_dir C64_LAYOUT_OUT "${C64_LAYOUT_OUT:-}"
require_dir C64_WORK "${C64_WORK:-}"
require_dir C64_WORK/kc "$C64_WORK/kc"
require_dir C64_WORK/kexts "$C64_WORK/kexts"
[ -z "${RDNA4_FB_KEXT:-}" ] || fail 'RDNA4_FB_KEXT is obsolete; standalone Garnet builds directly from src/garnet'
[ -z "${RDNA4_DISPLAY_SRC:-}" ] || fail 'RDNA4_DISPLAY_SRC is obsolete; standalone Garnet builds directly from src/garnet'
[ -z "${RDNA4_LILU_KEXT:-}" ] || fail 'RDNA4_LILU_KEXT is obsolete; standalone Garnet requires no Lilu dependency'
MKSDK=$(cd "$MKSDK" && pwd -P)
C64_LAYOUT_OUT=$(cd "$C64_LAYOUT_OUT" && pwd -P)
C64_WORK=$(cd "$C64_WORK" && pwd -P)
CHIPS=${CHIPS:-navi44}
for chip in $CHIPS; do
    case "$chip" in navi44|navi48) ;; *) fail "CHIPS contains unsupported chip: $chip";; esac
done
RDNA4_RELEASE=${RDNA4_RELEASE:-0}
case "$RDNA4_RELEASE" in
    0|1) ;;
    *) fail "RDNA4_RELEASE must be 0 or 1: $RDNA4_RELEASE";;
esac
if [ "$RDNA4_RELEASE" = 1 ]; then
    [ "$CHIPS" = navi44 ] || fail "RDNA4_RELEASE=1 strictly requires CHIPS=navi44 (got: $CHIPS)"
    RDNA4_BUILD_PCIBARCOMPAT=${RDNA4_BUILD_PCIBARCOMPAT:-1}
    [ "$RDNA4_BUILD_PCIBARCOMPAT" = 1 ] || fail "RDNA4_RELEASE=1 requires RDNA4_BUILD_PCIBARCOMPAT=1"
else
    RDNA4_BUILD_PCIBARCOMPAT=${RDNA4_BUILD_PCIBARCOMPAT:-0}
fi
case "$RDNA4_BUILD_PCIBARCOMPAT" in
    0|1) ;;
    *) fail "RDNA4_BUILD_PCIBARCOMPAT must be 0 or 1: $RDNA4_BUILD_PCIBARCOMPAT";;
esac
if [ -n "${RDNA4_RADV_LIB:-}" ]; then
    require_file RDNA4_RADV_LIB "$RDNA4_RADV_LIB"
    RDNA4_RADV_LIB=$(absolute_file "$RDNA4_RADV_LIB")
else
    require_dir 'RDNA4_MESA_SRC (or set RDNA4_RADV_LIB)' "${RDNA4_MESA_SRC:-}"
fi
if [ -n "${RDNA4_MESA_SRC:-}" ]; then
    require_dir RDNA4_MESA_SRC "$RDNA4_MESA_SRC"
    RDNA4_MESA_SRC=$(cd "$RDNA4_MESA_SRC" && pwd -P)
fi
if [ -n "${RDNA4_XLATE_LIB:-}" ]; then
    require_file RDNA4_XLATE_LIB "$RDNA4_XLATE_LIB"
    RDNA4_XLATE_LIB=$(absolute_file "$RDNA4_XLATE_LIB")
else
    require_dir 'RDNA4_M2V_SRC (or set RDNA4_XLATE_LIB)' "${RDNA4_M2V_SRC:-}"
    RDNA4_M2V_SRC=$(cd "$RDNA4_M2V_SRC" && pwd -P)
fi
if [ -z "${RDNA4_VK_INC:-}" ]; then
    [ -n "${RDNA4_MESA_SRC:-}" ] || fail 'Set RDNA4_VK_INC to Vulkan include flags, or set RDNA4_MESA_SRC for its include directory'
    RDNA4_VK_INC="-I$RDNA4_MESA_SRC/include"
fi
# build.sh consumes a whitespace-separated flag string, without shell re-parsing.
read -r -a vk_flags <<< "$RDNA4_VK_INC"
[ "${#vk_flags[@]}" -gt 0 ] || fail 'RDNA4_VK_INC must contain Vulkan include flags'
for command in make clang codesign file find strings tar shasum git xcrun zsh python3 tr; do
    command -v "$command" >/dev/null 2>&1 || fail "Required tool not found: $command"
done
[ -x /usr/libexec/PlistBuddy ] || fail 'Required tool not found: /usr/libexec/PlistBuddy'
collector="$repo/tools/rdna4/collect-installed.sh"
require_file 'Build identity helper' "$helper"
require_file 'Collector script' "$collector"
if [ "$RDNA4_RELEASE" = 1 ]; then
    fp_exporter="$repo/tools/ioaccel-layout/export_fingerprint.py"
    amd_license=${RDNA4_AMD_FIRMWARE_LICENSE:-$repo/src/garnet/firmware/LICENSE.amdgpu}
    default_denylist="$repo/tools/rdna4/default-app-denylist.txt"
    vulkan_icd_src="$repo/tools/rdna4/optional/vulkan/radeon_icd.x86_64.json"
    radv_app_src="$repo/tools/rdna4/radv-app.sh"
    require_file 'Fingerprint exporter' "$fp_exporter"
    require_file 'AMD firmware redistribution license (LICENSE.amdgpu)' "$amd_license"
    [ -s "$amd_license" ] || fail "LICENSE.amdgpu must be non-empty: $amd_license"
    require_file 'Default app denylist' "$default_denylist"
    require_file 'Optional Vulkan ICD template' "$vulkan_icd_src"
    require_file 'Optional radv-app wrapper' "$radv_app_src"
fi
printf '#include <vulkan/vulkan.h>\n' | clang -arch x86_64 "${vk_flags[@]}" -E -x c - >/dev/null \
    || fail 'RDNA4_VK_INC does not resolve Vulkan headers'
[ ! -e "$out/payload" ] && [ ! -L "$out/payload" ] || fail 'OUT_DIR/payload already exists; choose a fresh OUT_DIR'
mkdir -p "$out/work"
out=$(cd "$out" && pwd -P)
work="$out/work"
export TMPDIR="${TMPDIR:-$work}"
require_dir TMPDIR "$TMPDIR"
input_identities="$work/input-identities.txt"
: > "$input_identities"
python3 "$helper" local-source --repo "$repo" \
    --output "$work/LOCAL-SOURCE-MANIFEST.txt" > "$work/local-source-identity.txt" \
    || fail 'cannot record deterministic local source snapshot'

metal_cache="$repo/src/garnet-metal/spvcache"
require_dir 'Metal shader cache' "$metal_cache"
metal_spv_count=$(find "$metal_cache" -maxdepth 1 -type f -name '*.spv' | wc -l | tr -d ' ')
metal_meta_count=$(find "$metal_cache" -maxdepth 1 -type f -name '*.meta.json' | wc -l | tr -d ' ')
[ "$metal_spv_count" -gt 0 ] && [ "$metal_spv_count" -eq "$metal_meta_count" ] || \
    fail 'Metal shader cache must contain matching .spv and .meta.json files'
record_input MKSDK "$MKSDK"
record_input C64_LAYOUT_OUT "$C64_LAYOUT_OUT"
record_input C64_WORK "$C64_WORK"
if [ -n "${RDNA4_MESA_SRC:-}" ]; then
    record_input RDNA4_MESA_SRC "$RDNA4_MESA_SRC"
fi
if [ -n "${RDNA4_M2V_SRC:-}" ]; then
    record_input RDNA4_M2V_SRC "$RDNA4_M2V_SRC" --exclude target
fi
printf 'build-all.sh: starting sequential build (global build concurrency limit 2)\n' >&2

if [ -z "${RDNA4_RADV_LIB:-}" ]; then
    bash "$repo/tools/rdna4/build-radv.sh" "$RDNA4_MESA_SRC" "$work/radv" > "$work/radv.stdout"
    cat "$work/radv.stdout" >&2
    RDNA4_RADV_LIB=$(tail -n 1 "$work/radv.stdout")
    require_file 'RADV build output' "$RDNA4_RADV_LIB"
    RDNA4_RADV_LIB=$(absolute_file "$RDNA4_RADV_LIB")
fi
if [ -z "${RDNA4_XLATE_LIB:-}" ]; then
    bash "$repo/tools/rdna4/build-xlate.sh" "$RDNA4_M2V_SRC" "$work/xlate" > "$work/xlate.stdout"
    cat "$work/xlate.stdout" >&2
    RDNA4_XLATE_LIB=$(tail -n 1 "$work/xlate.stdout")
    require_file 'XLATE build output' "$RDNA4_XLATE_LIB"
    RDNA4_XLATE_LIB=$(absolute_file "$RDNA4_XLATE_LIB")
fi
bash "$repo/tools/rdna4/xlate-guard.sh" verify "$repo" "$RDNA4_XLATE_LIB" \
    || fail 'RDNA4_XLATE_LIB failed symbol/freshness verification'
record_input RDNA4_RADV_LIB "$RDNA4_RADV_LIB"
record_input RDNA4_XLATE_LIB "$RDNA4_XLATE_LIB"
export RDNA4_RADV_LIB RDNA4_XLATE_LIB RDNA4_VK_INC C64_WORK
make -C "$repo/src/garnet" all "CHIPS=$CHIPS" "MKSDK=$MKSDK" "BUILD=$work/garnet" >&2
C64_MKSDK="$MKSDK" make -C "$repo/src/garnet-accel" all "MKSDK=$MKSDK" \
    "C64_LAYOUT_OUT=$C64_LAYOUT_OUT" "C64_WORK=$C64_WORK" "BUILD=$work/accel" >&2
if [ "$RDNA4_BUILD_PCIBARCOMPAT" = 1 ]; then
    make -C "$repo/src/garnet-pci-compat" all "MKSDK=$MKSDK" "BUILD=$work/pcibarcompat" SIGN=1 >&2
fi
RDNA4_RELEASE="$RDNA4_RELEASE" zsh "$repo/src/garnet-metal/build.sh" >&2
bash "$repo/tools/rdna4/build-native-tools.sh" "$work/tools" >&2

payload="$out/payload"
mkdir "$payload"
cp -R "$work/garnet/Garnet.kext" "$payload/"
release_layout_id=
release_short_id=
if [ "$RDNA4_RELEASE" = 1 ]; then
    release_layout_id=$(python3 "$fp_exporter" \
        --layout-out "$C64_LAYOUT_OUT" \
        --gates-dir "$repo/src/garnet-accel/gates" \
        --out "$work/layout-fingerprint.json" \
        --variants-index "$work/variants-index.json" \
        --verified-build "${RDNA4_VERIFIED_BUILD:-25G332}" \
        --print-layout-id) || fail 'Failed to export GarnetAccel layout fingerprint'
    [ -n "$release_layout_id" ] || fail 'Empty layoutId from export_fingerprint.py'
    release_short_id=${release_layout_id:0:16}
    accel_rel="variants/$release_short_id/GarnetAccel.kext"
    accel_kext="$payload/$accel_rel"
    mkdir -p "$payload/variants/$release_short_id" "$payload/optional/vulkan"
    cp -R "$work/accel/GarnetAccel.kext" "$accel_kext"
    cp "$work/layout-fingerprint.json" "$payload/layout-fingerprint.json"
    cp "$work/layout-fingerprint.json" "$payload/variants/$release_short_id/layout-fingerprint.json"
    cp "$work/variants-index.json" "$payload/variants/index.json"
    cp "$amd_license" "$payload/LICENSE.amdgpu"
    cp "$default_denylist" "$payload/default-app-denylist.txt"
    cp "$vulkan_icd_src" "$payload/optional/vulkan/radeon_icd.x86_64.json"
    cp "$radv_app_src" "$payload/optional/vulkan/radv-app.sh"
    chmod 755 "$payload/optional/vulkan/radv-app.sh"
else
    accel_rel=GarnetAccel.kext
    accel_kext="$payload/$accel_rel"
    cp -R "$work/accel/GarnetAccel.kext" "$accel_kext"
fi
if [ "$RDNA4_BUILD_PCIBARCOMPAT" = 1 ]; then
    mkdir -p "$payload/optional"
    cp -R "$work/pcibarcompat/GarnetPCIBarCompat.kext" "$payload/optional/"
fi
cp "$repo/src/garnet-metal/build/GarnetMetal.bundle.tar" "$payload/"
cp "$work/LOCAL-SOURCE-MANIFEST.txt" "$payload/"
tools=(native-translate native-bundle-check publish-metal mtlprobe-direct garnettest)
for name in "${tools[@]}"; do cp "$work/tools/$name" "$payload/"; done
for name in install-native.sh uninstall-native.sh deploy-common.sh activate-native.sh \
    collect-installed.sh com.garnet.native.plist com.garnet.native-translate.plist; do
    cp "$repo/tools/rdna4/$name" "$payload/"
done
require_file 'Payload collector' "$payload/collect-installed.sh"
record_input PAYLOAD_COLLECTOR "$payload/collect-installed.sh"

check_identity() {
    local plist="$1/Contents/Info.plist"
    [ "$(plist_value "$plist" CFBundleIdentifier)" = "$2" ] || fail "Wrong CFBundleIdentifier: $1"
    [ "$(plist_value "$plist" CFBundleExecutable)" = "$3" ] || fail "Wrong CFBundleExecutable: $1"
    require_file 'Bundle executable' "$1/Contents/MacOS/$3"
}
check_identity "$payload/Garnet.kext" org.garnet.driver Garnet
check_identity "$accel_kext" org.garnet.accel GarnetAccel
if [ "$RDNA4_BUILD_PCIBARCOMPAT" = 1 ]; then
    check_identity "$payload/optional/GarnetPCIBarCompat.kext" org.garnet.pcibarcompat GarnetPCIBarCompat
fi
ensure_signed() {
    local kext=$1
    if codesign --verify --deep --strict "$kext" >/dev/null 2>&1; then
        printf 'build-all.sh: signature verified: %s\n' "$kext" >&2
        return 0
    fi
    printf 'build-all.sh: ad-hoc signing: %s\n' "$kext" >&2
    codesign --force --deep --sign - "$kext" || return 1
    codesign --verify --deep --strict "$kext"
}
ensure_signed "$payload/Garnet.kext" || fail 'Cannot sign payload kext: Garnet'
ensure_signed "$accel_kext" || fail 'Cannot sign payload kext: GarnetAccel'
python3 "$helper" garnet "$payload/Garnet.kext" > "$work/payload-garnet-identity.txt" \
    || fail 'payload Garnet.kext has the wrong identity'
python3 "$helper" accel "$accel_kext" > "$work/payload-accel-identity.txt" \
    || fail 'payload GarnetAccel.kext has the wrong identity'
if [ "$RDNA4_BUILD_PCIBARCOMPAT" = 1 ]; then
    ensure_signed "$payload/optional/GarnetPCIBarCompat.kext" \
        || fail 'Cannot sign payload kext: GarnetPCIBarCompat'
    python3 "$helper" pcibarcompat "$payload/optional/GarnetPCIBarCompat.kext" \
        > "$work/payload-pcibarcompat-identity.txt" \
        || fail 'payload GarnetPCIBarCompat.kext has the wrong identity'
fi
for legacy in AMDRadeonRDNA4.kext RDNA4Bringup.kext RDNA4Accel.kext RDNA4FB.kext; do
    [ ! -e "$payload/$legacy" ] && [ ! -L "$payload/$legacy" ] || \
        fail "legacy $legacy must not be present in the payload"
done
extracted=$(mktemp -d "$work/bundle-check.XXXXXXXX")
trap 'rm -rf "$extracted"' EXIT
tar -xf "$payload/GarnetMetal.bundle.tar" -C "$extracted"
bundle="$extracted/GarnetMetal.bundle"
check_identity "$bundle" org.garnet.metal GarnetMetal
require_file 'Bundled RADV' "$bundle/Contents/Resources/libvulkan_radeon.dylib"
require_file 'Bundled XLATE' "$bundle/Contents/Resources/libgarnetxlate.dylib"
require_dir 'Bundled Metal shader cache' "$bundle/Contents/Resources/spvcache"
bundle_spv_count=$(find "$bundle/Contents/Resources/spvcache" -maxdepth 1 -type f -name '*.spv' | wc -l | tr -d ' ')
bundle_meta_count=$(find "$bundle/Contents/Resources/spvcache" -maxdepth 1 -type f -name '*.meta.json' | wc -l | tr -d ' ')
[ "$bundle_spv_count" -eq "$metal_spv_count" ] && [ "$bundle_meta_count" -eq "$metal_meta_count" ] || \
    fail 'Bundled Metal shader cache does not match its source asset set'
record_input OUTPUT_METAL_BUNDLE "$payload/GarnetMetal.bundle.tar"
record_input OUTPUT_METAL_EXECUTABLE "$bundle/Contents/MacOS/GarnetMetal"
record_input OUTPUT_METAL_INFO "$bundle/Contents/Info.plist"
record_input OUTPUT_METAL_RADV "$bundle/Contents/Resources/libvulkan_radeon.dylib"
record_input OUTPUT_METAL_XLATE "$bundle/Contents/Resources/libgarnetxlate.dylib"
record_input OUTPUT_METAL_SPVCACHE "$bundle/Contents/Resources/spvcache"
mkdir -p "$work/unsigned"
cp "$bundle/Contents/MacOS/GarnetMetal" "$work/unsigned/GarnetMetal"
codesign --remove-signature "$work/unsigned/GarnetMetal" \
    || fail 'Cannot remove the Metal executable signature for identity comparison'
record_input OUTPUT_METAL_UNSIGNED_EXECUTABLE "$work/unsigned/GarnetMetal"
record_input OUTPUT_GARNET_EXECUTABLE "$payload/Garnet.kext/Contents/MacOS/$(plist_value "$payload/Garnet.kext/Contents/Info.plist" CFBundleExecutable)"
record_input OUTPUT_GARNET_INFO_PLIST "$payload/Garnet.kext/Contents/Info.plist"
record_input OUTPUT_GARNETACCEL_EXECUTABLE "$accel_kext/Contents/MacOS/$(plist_value "$accel_kext/Contents/Info.plist" CFBundleExecutable)"
record_input OUTPUT_GARNETACCEL_INFO_PLIST "$accel_kext/Contents/Info.plist"
if [ "$RDNA4_BUILD_PCIBARCOMPAT" = 1 ]; then
    executable=$(plist_value "$payload/optional/GarnetPCIBarCompat.kext/Contents/Info.plist" CFBundleExecutable)
    record_input OUTPUT_GARNETPCIBARCOMPAT_EXECUTABLE "$payload/optional/GarnetPCIBarCompat.kext/Contents/MacOS/$executable"
    record_input OUTPUT_GARNETPCIBARCOMPAT_INFO_PLIST "$payload/optional/GarnetPCIBarCompat.kext/Contents/Info.plist"
fi
mkdir -p "$work/signed-ref"
cp "$RDNA4_RADV_LIB" "$work/signed-ref/libvulkan_radeon.dylib"
codesign --force --sign - "$work/signed-ref/libvulkan_radeon.dylib"
cmp -s "$work/signed-ref/libvulkan_radeon.dylib" "$bundle/Contents/Resources/libvulkan_radeon.dylib" \
    || fail 'Bundled RADV differs from ad-hoc signed RDNA4_RADV_LIB'
cp "$RDNA4_XLATE_LIB" "$work/signed-ref/libgarnetxlate.dylib"
codesign --force --sign - "$work/signed-ref/libgarnetxlate.dylib"
cmp -s "$work/signed-ref/libgarnetxlate.dylib" "$bundle/Contents/Resources/libgarnetxlate.dylib" \
    || fail 'Bundled XLATE differs from ad-hoc signed RDNA4_XLATE_LIB'
codesign --verify --deep --strict "$bundle"

check_legacy() {
    # Materialize strings so grep cannot hide a strings failure or cause SIGPIPE.
    strings "$1" > "$work/strings.txt"
    if grep -E 'Navi48Bringup|Navi48Metal|com\.navi48|n48m|AMDRadeonRDNA4|RDNA4FB|RDNA4Bringup|RDNA4Accel|RDNA4Metal|as\.vit9696\.Lilu' "$work/strings.txt" >&2; then
        fail "Legacy identifiers in $1"
    else
        [ "$?" -eq 1 ] || fail "Cannot scan strings from $1"
    fi
}
for kext_check in "$payload/Garnet.kext" "$accel_kext"; do
    executable=$(plist_value "$kext_check/Contents/Info.plist" CFBundleExecutable)
    require_file 'Kext executable' "$kext_check/Contents/MacOS/$executable"
    check_legacy "$kext_check/Contents/MacOS/$executable"
done
if [ "$RDNA4_BUILD_PCIBARCOMPAT" = 1 ]; then
    executable=$(plist_value "$payload/optional/GarnetPCIBarCompat.kext/Contents/Info.plist" CFBundleExecutable)
    require_file 'Kext executable' "$payload/optional/GarnetPCIBarCompat.kext/Contents/MacOS/$executable"
    check_legacy "$payload/optional/GarnetPCIBarCompat.kext/Contents/MacOS/$executable"
fi
check_legacy "$bundle/Contents/MacOS/GarnetMetal"
check_legacy "$RDNA4_RADV_LIB"
for name in "${tools[@]}"; do
    check_legacy "$payload/$name"
    label=${name//-/_}
    record_input "OUTPUT_TOOL_${label}" "$payload/$name"
done
"$payload/native-bundle-check" "$bundle" >&2
python3 "$helper" local-source --repo "$repo" \
    --output "$work/LOCAL-SOURCE-MANIFEST.after.txt" > "$work/local-source-after-identity.txt" \
    || fail 'cannot record post-build local source snapshot'
before_source_digest=$(sed -n 's/^local_source.digest=//p' "$work/local-source-identity.txt")
after_source_digest=$(sed -n 's/^local_source.digest=//p' "$work/local-source-after-identity.txt")
[ -n "$before_source_digest" ] && [ "$before_source_digest" = "$after_source_digest" ] || \
    fail 'local source changed while the payload was being built'

# Checksum documents cannot contain their own hashes. MANIFEST lists the content
# files; SHA256SUMS additionally authenticates MANIFEST and excludes only itself.
while IFS= read -r -d '' path; do
    case "$path" in *$'\n'*|*$'\r'*) fail "Payload filename contains a line break: $path";; esac
done < <(find "$payload" -type f -print0)
(cd "$payload" && find . -type f -print | sort) > "$work/payload-files.txt"
head=$(git -C "$repo" rev-parse HEAD)
description=$(git -C "$repo" describe --always --tags)
dirty=
[ -z "$(git -C "$repo" status --porcelain --untracked-files=normal)" ] || dirty=-dirty
local_source_sha=$(shasum -a 256 "$work/LOCAL-SOURCE-MANIFEST.txt" | awk '{print $1}')
[ -n "$local_source_sha" ] || fail 'Cannot hash LOCAL-SOURCE-MANIFEST.txt'
input_identities_sha=$(shasum -a 256 "$input_identities" | awk '{print $1}')
[ -n "$input_identities_sha" ] || fail 'Cannot hash input identities'
diff_sha=$(git -C "$repo" diff --binary | shasum -a 256 | awk '{print $1}')
status_sha=$(git -C "$repo" status --porcelain=v1 --untracked-files=normal | shasum -a 256 | awk '{print $1}')
[ -n "$diff_sha" ] && [ -n "$status_sha" ] || fail 'Cannot hash repository dirty state'
os_version=$(sw_vers -productVersion)
os_build=$(sw_vers -buildVersion)
clang_version=$(clang --version | sed -n '1p')
make_version=$(make --version | sed -n '1p')
python_version=$(python3 --version 2>&1)
zsh_version=$(zsh --version)
cargo_version=unavailable
if command -v cargo >/dev/null 2>&1; then cargo_version=$(cargo --version); fi
ninja_version=unavailable
if command -v ninja >/dev/null 2>&1; then ninja_version=$(ninja --version); fi
meson_version=unavailable
if command -v meson >/dev/null 2>&1; then meson_version=$(meson --version); fi
if [ "$RDNA4_RELEASE" = 1 ]; then
    python3 "$helper" manifest \
        --garnet-identity "$work/payload-garnet-identity.txt" \
        --accel-identity "$work/payload-accel-identity.txt" \
        --pcibarcompat-identity "$work/payload-pcibarcompat-identity.txt" \
        --release-mode 1 \
        --target-silicon navi44 \
        --target-pci-ids 1002:7590 \
        --default-layout-id "$release_layout_id" \
        --amd-firmware-license LICENSE.amdgpu \
        --default-accel-variant-path "$accel_rel" > "$work/delivery-manifest.txt" \
        || fail 'delivery release manifest identity check failed'
elif [ "$RDNA4_BUILD_PCIBARCOMPAT" = 1 ]; then
    python3 "$helper" manifest \
        --garnet-identity "$work/payload-garnet-identity.txt" \
        --accel-identity "$work/payload-accel-identity.txt" \
        --pcibarcompat-identity "$work/payload-pcibarcompat-identity.txt" > "$work/delivery-manifest.txt" \
        || fail 'delivery manifest identity check failed'
else
    python3 "$helper" manifest \
        --garnet-identity "$work/payload-garnet-identity.txt" \
        --accel-identity "$work/payload-accel-identity.txt" > "$work/delivery-manifest.txt" \
        || fail 'delivery manifest identity check failed'
fi
{
    cat "$work/delivery-manifest.txt"
    printf '#\n# Payload and build provenance\n'
    printf 'REPOSITORY=%s%s\nREPOSITORY_HEAD=%s%s\nCHIPS=%s\n' \
        "$description" "$dirty" "$head" "$dirty" "$CHIPS"
    cat "$work/local-source-identity.txt"
    printf 'LOCAL_SOURCE_MANIFEST=LOCAL-SOURCE-MANIFEST.txt\n'
    printf 'LOCAL_SOURCE_MANIFEST_SHA256=%s\n' "$local_source_sha"
    printf 'LOCAL_SOURCE_STABLE=PASS\n'
    printf 'INPUT_IDENTITIES_SHA256=%s\n' "$input_identities_sha"
    printf 'REPOSITORY_DIFF_SHA256=%s\n' "$diff_sha"
    printf 'REPOSITORY_STATUS_SHA256=%s\n' "$status_sha"
    printf 'BUILD_UTC=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    printf 'BUILD_HOST_OS=macOS %s (%s)\nBUILD_HOST_ARCH=%s\n' \
        "$os_version" "$os_build" "$(uname -m)"
    printf 'BUILD_CLANG=%s\nBUILD_MAKE=%s\nBUILD_PYTHON=%s\nBUILD_ZSH=%s\n' \
        "$clang_version" "$make_version" "$python_version" "$zsh_version"
    printf 'BUILD_CARGO=%s\nBUILD_NINJA=%s\nBUILD_MESON=%s\n' \
        "$cargo_version" "$ninja_version" "$meson_version"
    printf 'BUILD_GARNET_OPTIONS=CHIPS=%s BUILD=work/garnet\n' "$CHIPS"
    printf 'BUILD_ACCEL_OPTIONS=CHIPS=%s BUILD=work/accel C64_LAYOUT_OUT=%s C64_WORK=%s\n' \
        "$CHIPS" "$C64_LAYOUT_OUT" "$C64_WORK"
    if [ "$RDNA4_BUILD_PCIBARCOMPAT" = 1 ]; then
        printf 'BUILD_PCIBARCOMPAT_OPTIONS=BUILD=work/pcibarcompat STAGED=optional/GarnetPCIBarCompat.kext LOAD_ORDER=after-IOPCIFamily-before-PCI-config-and-Garnet-via-OpenCore-Kernel-Add-Root DISABLE_BOOT_ARG=garnet-rebar-compat=0\n'
    fi
    printf 'BUILD_METAL_OPTIONS=RDNA4_LAZY=%s RDNA4_9D=%s\n' \
        "${RDNA4_LAZY:-1}" "${RDNA4_9D:-1}"
    printf 'BUILD_METAL_SPVCACHE_SOURCE=%s spv=%s meta=%s\n' \
        "$metal_cache" "$metal_spv_count" "$metal_meta_count"
    printf 'KEXT_SIGNING=strict-verified-after-ad-hoc-fallback\n'
    printf 'RADV_INPUT=%s\nXLATE_INPUT=%s\n' "$RDNA4_RADV_LIB" "$RDNA4_XLATE_LIB"
    for kext_ver_entry in "Garnet:$payload/Garnet.kext" "GarnetAccel:$accel_kext"; do
        name=${kext_ver_entry%%:*}
        kext_ver_path=${kext_ver_entry#*:}
        version=$(plist_value "$kext_ver_path/Contents/Info.plist" CFBundleVersion)
        [ -n "$version" ] || fail "Missing CFBundleVersion: $name"
        printf '%s_CFBUNDLEVERSION=%s\n' "$name" "$version"
    done
    if [ "$RDNA4_BUILD_PCIBARCOMPAT" = 1 ]; then
        version=$(plist_value "$payload/optional/GarnetPCIBarCompat.kext/Contents/Info.plist" CFBundleVersion)
        [ -n "$version" ] || fail 'Missing CFBundleVersion: GarnetPCIBarCompat'
        printf 'GarnetPCIBarCompat_CFBUNDLEVERSION=%s\n' "$version"
    fi
    printf '#\n# Input and output build identities\n'
    cat "$input_identities"
    cat "$work/payload-garnet-identity.txt"
    cat "$work/payload-accel-identity.txt"
    if [ "$RDNA4_BUILD_PCIBARCOMPAT" = 1 ]; then
        cat "$work/payload-pcibarcompat-identity.txt"
    fi
    printf '#\n# Payload files\n'
    printf '\nSHA256 content files (SHA256SUMS also covers MANIFEST.txt):\n'
    while IFS= read -r path; do (cd "$payload" && shasum -a 256 "$path"); done < "$work/payload-files.txt"
} > "$payload/MANIFEST.txt"
(cd "$payload" && find . -type f ! -path './SHA256SUMS' -print | sort) > "$work/checksum-files.txt"
(
    cd "$payload"
    while IFS= read -r path; do shasum -a 256 "$path"; done < "$work/checksum-files.txt" > SHA256SUMS
    shasum -a 256 -c SHA256SUMS >&2
)
if [ "$RDNA4_RELEASE" = 1 ]; then
    python3 "$helper" verify-release-payload "$payload" >&2 \
        || fail 'Release 1 payload verification failed'
fi
printf '%s\n' "$payload"
