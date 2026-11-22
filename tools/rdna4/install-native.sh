#!/bin/bash
# Install one matched Garnet payload on the test PC's macOS 26.7.2 (25G332).
#   sudo bash install-native.sh [--check] [--controlled|--vm-load-only] ESP_MOUNT
# --check validates and prints the plan without writing anywhere under the target.
# --controlled installs Garnet.kext (Aux KC), GarnetAccel.kext (Aux KC) and
#   GarnetMetal.bundle (/Library/GPUBundles) with no boot activation jobs; run
#   the root/in-process checks manually from the payload.
# Garnet.kext is standalone (no Lilu dependency) and goes to /Library/Extensions
# (Aux KC) because AMDGarnetPrimaryFramebuffer inherits IOFramebuffer (SysKC).
# Retired RDNA4*/AMDRadeonRDNA4/Navi48* components are backup-and-remove only when
# their exact managed identities match. User-installed Lilu, WhateverGreen and
# unrelated extensions are never rewritten. This script never loads a kext.
set -euo pipefail
umask 077
usage() { printf 'Usage: sudo bash install-native.sh [--check] [--controlled|--vm-load-only] ESP_MOUNT\n'; }
check=0
profile=native
controlled=0
vm_load_only=0
while [ "$#" -gt 0 ]; do
    case "$1" in
        --help) usage; exit 0;;
        --check) check=1; shift;;
        --controlled) controlled=1; profile=controlled; shift;;
        --vm-load-only) vm_load_only=1; profile=vm-load-only; shift;;
        --*) usage >&2; exit 2;;
        *) break;;
    esac
done
[ "$#" -eq 1 ] || { usage >&2; exit 2; }
work=$(cd "$(dirname "$0")" && pwd)
. "$work/deploy-common.sh"
[ "$controlled" -eq 0 ] || [ "$vm_load_only" -eq 0 ] || fail '--controlled and --vm-load-only are mutually exclusive'
[ "$(uname -s)" = Darwin ] || fail 'Run in installed macOS'
[ "$(id -u)" -eq 0 ] || fail 'Run with sudo'
[ "$(sw_vers -buildVersion)" = 25G332 ] || fail 'Expected macOS build 25G332'
vmm=0
case " $(sysctl -n machdep.cpu.features 2>/dev/null || :) " in *' VMM '*) vmm=1;; esac
[ "$check" -eq 1 ] || [ "$vmm" -eq 0 ] || fail 'Actual installation in a VM is disabled; use --check'
[ "$vm_load_only" -eq 0 ] || [ "$check" -eq 1 ] || fail '--vm-load-only is preflight-only; candidate kexts are not installed or loaded in a VM'

esp=$(cd "$1" && pwd -P)
oc="$esp/EFI/OC"
config="$oc/config.plist"
[ -f "$config" ] && [ ! -L "$config" ] && [ -f "$oc/OpenCore.efi" ] || fail 'Expected an OpenCore ESP'
plutil -lint "$config" >/dev/null

if [ "$check" -eq 1 ]; then
    stage=$(mktemp -d "${TMPDIR:-/tmp}/rdna4-install-check.XXXXXXXX")
else
    mkdir -p "$R/private/tmp"
    stage=$(mktemp -d "$R/private/tmp/rdna4-install.XXXXXXXX")
fi
cleanup_stage() { rm -rf "$stage"; }
trap cleanup_stage EXIT

manifest=$(delivery_manifest "$work")
required=("$GARNET_BUNDLE" "$ACCEL_BUNDLE" "$METAL_BUNDLE.tar"
    activate-native.sh com.garnet.native.plist com.garnet.native-translate.plist
    native-translate native-bundle-check publish-metal mtlprobe-direct garnettest
    MANIFEST.txt SHA256SUMS "$(basename "$manifest")")
for file in "${required[@]}"; do
    [ -f "$work/$file" ] || [ -d "$work/$file" ] || fail "Missing payload: $file"
done
verify_checksum_coverage() {  # payload, scratch
    local payload=$1 scratch=$2 line hash path
    : > "$scratch/checksum-paths.txt"
    while IFS= read -r line || [ -n "$line" ]; do
        [ -n "$line" ] || fail 'Payload SHA256SUMS contains an empty line'
        hash=${line%% *}
        [ "${#hash}" -eq 64 ] || fail "Payload SHA256SUMS has a malformed hash: $line"
        case "$hash" in *[!0-9a-f]*) fail "Payload SHA256SUMS has a non-lowercase hash: $line";; esac
        path=${line#"$hash  "}
        [ "$path" != "$line" ] || fail "Payload SHA256SUMS has a malformed path: $line"
        case "$path" in ./*) ;; *) fail "Payload SHA256SUMS has a non-relative path: $path";; esac
        case "/$path/" in */../*) fail "Payload SHA256SUMS path escapes the payload: $path";; esac
        printf '%s\n' "$path" >> "$scratch/checksum-paths.txt"
    done < "$payload/SHA256SUMS"
    [ -s "$scratch/checksum-paths.txt" ] || fail 'Payload SHA256SUMS is empty'
    (cd "$payload" && find . -type f ! -path './SHA256SUMS' -print | sort) \
        > "$scratch/payload-files.txt"
    sort "$scratch/checksum-paths.txt" > "$scratch/checksum-paths.sorted"
    cmp -s "$scratch/payload-files.txt" "$scratch/checksum-paths.sorted" \
        || fail 'Payload checksum manifest does not cover every payload file'
}
verify_checksum_coverage "$work" "$stage"
(cd "$work" && shasum -a 256 -c SHA256SUMS >/dev/null 2>&1) \
    || fail 'Payload checksum verification failed'
for kext in "$GARNET_BUNDLE" "$ACCEL_BUNDLE"; do
    codesign --verify --deep --strict "$work/$kext" || fail "Payload signature failed: $kext"
done
plutil -lint "$work/com.garnet.native.plist" >/dev/null
plutil -lint "$work/com.garnet.native-translate.plist" >/dev/null
codesign --verify --strict "$work/native-translate" || fail 'native-translate signature failed'

verify_delivery_manifest "$work" "$manifest"

garnet_entries=$(kernel_indices_bundle "$config" "$GARNET_BUNDLE") \
    || fail 'Cannot read the OpenCore Kernel.Add list'
garnet_count=0
[ -z "$garnet_entries" ] || garnet_count=$(printf '%s\n' "$garnet_entries" | wc -l | tr -d ' ')
[ "$garnet_count" -le 1 ] || fail "OpenCore config contains duplicate $GARNET_BUNDLE Kernel.Add entries"

# Refuse to overwrite a same-named, mismatched bundle. This is the boundary
# between a managed component and an unrelated file that must be preserved.
managed_bundle() {  # path, id, executable, label
    [ -e "$1" ] || [ -L "$1" ] || return 0
    verify_bundle_identity "$1" "$2" "$3" "$4"
}
managed_bundle "$oc/Kexts/$GARNET_BUNDLE" "$GARNET_BUNDLE_ID" "$GARNET_EXECUTABLE" \
    'residual ESP Garnet'
managed_bundle "$R/Library/Extensions/$GARNET_BUNDLE" "$GARNET_BUNDLE_ID" "$GARNET_EXECUTABLE" \
    'existing Library Garnet'
managed_bundle "$R/Library/Extensions/$ACCEL_BUNDLE" "$ACCEL_BUNDLE_ID" "$ACCEL_EXECUTABLE" \
    'existing Library GarnetAccel'
managed_bundle "$R/Library/GPUBundles/$METAL_BUNDLE" "$METAL_BUNDLE_ID" GarnetMetal \
    'existing Library GarnetMetal'
managed_bundle "$oc/Kexts/AMDRadeonRDNA4.kext" com.hackintosh.AMDRadeonRDNA4 AMDRadeonRDNA4 \
    'legacy ESP AMDRadeonRDNA4'
managed_bundle "$oc/Kexts/RDNA4Bringup.kext" com.rdna4.bringup RDNA4Bringup \
    'legacy ESP RDNA4Bringup'
managed_bundle "$oc/Kexts/RDNA4FB.kext" com.hackintosh.RDNA4FB RDNA4FB \
    'legacy ESP RDNA4FB'
managed_bundle "$oc/Kexts/Navi48Bringup.kext" com.navi48.bringup Navi48Bringup \
    'legacy ESP Navi48Bringup'
verify_legacy_bundles

# Inspect the current Aux KC without changing it. Core/retired plugins in this KC
# are stale by definition; GarnetAccel is the only extension that belongs here.
aux_before="$stage/aux-before.tsv"
aux_list "$aux" "$stage/aux-before.plist" > "$aux_before" \
    || fail 'Aux KC inspect failed during preflight; nothing was changed'
aux_residual=
while IFS=$'\t' read -r id path; do
    [ -n "$id" ] || continue
    case " $AUX_OURS " in
        *" $id "*) aux_residual="$aux_residual$id:$path"$'\n';;
        *)
            [ -d "$path" ] && [ ! -L "$path" ] \
                || fail "Cannot preserve foreign Aux KC extension: $id ($path)";;
    esac
done < "$aux_before"

next="$stage/config-next.plist"
cp "$config" "$next"

# Remove/disable retired RDNA4/Navi48 entries and Aux-KC Garnet/GarnetAccel
# entries from OpenCore Kernel.Add. User Lilu, WhateverGreen and other external
# entries stay untouched.
kernel_delete_bundle "$next" AMDRadeonRDNA4.kext || fail 'Cannot remove AMDRadeonRDNA4.kext'
kernel_delete_bundle "$next" RDNA4FB.kext || fail 'Cannot remove RDNA4FB.kext'
kernel_delete_bundle "$next" Navi48FB.kext || fail 'Cannot remove Navi48FB.kext'
kernel_delete_bundle "$next" Navi48Accel.kext || fail 'Cannot remove Navi48Accel.kext'
kernel_delete_bundle "$next" "$GARNET_BUNDLE" || fail "Cannot remove $GARNET_BUNDLE from Kernel.Add"
kernel_disable_bundle "$next" RDNA4Bringup.kext || fail 'Cannot disable RDNA4Bringup.kext in Kernel.Add'
kernel_disable_bundle "$next" Navi48Bringup.kext || fail 'Cannot disable Navi48Bringup.kext in Kernel.Add'
kernel_disable_bundle "$next" RDNA4Accel.kext || fail 'Cannot disable RDNA4Accel.kext in Kernel.Add'
kernel_disable_bundle "$next" "$ACCEL_BUNDLE" || fail "Cannot disable $ACCEL_BUNDLE in Kernel.Add"
note "config: $GARNET_BUNDLE managed via Aux KC (excluded from Kernel.Add)"

old_args=$($buddy -c "Print :NVRAM:Add:$guid:boot-args" "$next" 2>/dev/null) || old_args=
args=$(profile_args "$old_args" "$profile")
$buddy -c "Set :NVRAM:Add:$guid:boot-args $args" "$next"
$buddy -c "Delete :NVRAM:Add:$guid:csr-active-config" "$next" 2>/dev/null || :
plutil -insert "NVRAM.Add.$guid.csr-active-config" -data AwgAAA== "$next"
$buddy -c 'Print :NVRAM:Delete' "$next" >/dev/null 2>&1 || $buddy -c 'Add :NVRAM:Delete dict' "$next"
$buddy -c "Print :NVRAM:Delete:$guid" "$next" >/dev/null 2>&1 || $buddy -c "Add :NVRAM:Delete:$guid array" "$next"
for key in boot-args csr-active-config; do
    found=0
    for ((i=0; ; i++)); do
        item=$($buddy -c "Print :NVRAM:Delete:$guid:$i" "$next" 2>/dev/null) || break
        [ "$item" != "$key" ] || found=1
    done
    [ "$found" -eq 1 ] || $buddy -c "Add :NVRAM:Delete:$guid: string $key" "$next"
done
plutil -lint "$next" >/dev/null
validate_kernel_config "$next" "$GARNET_BUNDLE"

extracted="$stage/bundle"
mkdir "$extracted"
tar -xf "$work/$METAL_BUNDLE.tar" -C "$extracted"
verify_bundle_identity "$extracted/$METAL_BUNDLE" "$METAL_BUNDLE_ID" GarnetMetal \
    "payload $METAL_BUNDLE"
codesign --verify --deep --strict "$extracted/$METAL_BUNDLE" \
    || fail "$METAL_BUNDLE signature failed"
"$work/native-bundle-check" "$extracted/$METAL_BUNDLE" >/dev/null \
    || fail "$METAL_BUNDLE load-only check failed"

if [ "$check" -eq 1 ]; then
    printf 'PREFLIGHT PASS profile=%s controlled=%s ESP=%s\n' "$profile" "$controlled" "$esp"
    printf 'MANIFEST=%s\n' "$manifest"
    printf 'GARNET_BUNDLE_ID=%s VERSION=%s EXE_SHA256=%s\n' \
        "$MF_GARNET_BUNDLE_ID" "$MF_GARNET_VERSION" "$MF_GARNET_EXECUTABLE_SHA256"
    printf 'ACCEL_BUNDLE_ID=%s VERSION=%s EXE_SHA256=%s\n' \
        "$MF_ACCEL_BUNDLE_ID" "$MF_ACCEL_VERSION" "$MF_ACCEL_EXECUTABLE_SHA256"
    printf 'BOOT_ARGS=%s\n' "$args"
    printf 'KERNEL_REMOVE=%s\n' "$GARNET_BUNDLE"
    printf 'AUX_KEEP=%s %s\n' "$GARNET_BUNDLE" "$ACCEL_BUNDLE"
    printf 'AUX_REBUILD=remove-retired preserve-foreign add-%s add-%s\n' "$GARNET_BUNDLE" "$ACCEL_BUNDLE"
    [ -z "$aux_residual" ] || printf '%s' "$aux_residual" | while IFS= read -r line; do
        [ -z "$line" ] || printf 'AUX_REMOVE=%s\n' "$line"
    done
    for p in "$oc/Kexts/AMDRadeonRDNA4.kext" "$oc/Kexts/RDNA4Bringup.kext" \
        "$oc/Kexts/RDNA4FB.kext" "$oc/Kexts/Navi48Bringup.kext" \
        "$oc/Kexts/$GARNET_BUNDLE"; do
        [ ! -e "$p" ] || printf 'LEGACY_REMOVE=%s\n' "$p"
    done
    while IFS= read -r p; do [ -n "$p" ] && printf 'LEGACY_REMOVE=%s\n' "$p"; done < <(legacy_present)
    if [ "$controlled" -eq 1 ]; then
        for d in com.garnet.native com.garnet.native-translate; do
            p="$R/Library/LaunchDaemons/$d.plist"
            [ ! -e "$p" ] || printf 'LEGACY_REMOVE=%s\n' "$p"
        done
        printf 'AUTOMATIC_ACTIVATION=disabled controlled=yes\n'
    else
        printf 'AUTOMATIC_ACTIVATION=boot-job controlled=no\n'
    fi
    printf 'CHANGES_TARGET=no\n'
    trap - EXIT
    rm -rf "$stage"
    exit 0
fi

prior_loaded_jobs=()
for d in $LEGACY_DAEMONS com.garnet.native com.garnet.native-translate; do
    launch_job_loaded "$d" && prior_loaded_jobs+=("$d")
done

mkdir -p "$support/backups"
backup=$(mktemp -d "$support/backups/install.XXXXXXXX")
cp -p "$config" "$backup/config-before.plist"
[ ! -f "$aux" ] || cp -p "$aux" "$backup/AuxiliaryKernelExtensions.kc"
cp "$manifest" "$backup/build-manifest.txt"

rollback_targets=()
rollback_backups=()
rollback_dirs=()
record_path() {  # path
    local path=$1 n
    [ ! -L "$path" ] || fail "Refusing symbolic-link managed path: $path"
    n=${#rollback_targets[@]}
    rollback_targets+=("$path")
    if [ -e "$path" ]; then
        mkdir -p "$backup/paths"
        rollback_backups+=("$backup/paths/$n-${path##*/}")
        cp -pR "$path" "$backup/paths/$n-${path##*/}"
    else
        rollback_backups+=("")
    fi
}
ensure_dir() {  # path
    local path=$1 p
    local -a missing=()
    p=$path
    while [ ! -d "$p" ]; do
        missing+=("$p")
        [ "$p" != / ] || break
        p=$(dirname "$p")
    done
    mkdir -p "$path"
    # Bash 3.2 treats an empty array expansion as unset under nounset.
    if [ "${#missing[@]}" -gt 0 ]; then
        for p in "${missing[@]}"; do rollback_dirs+=("$p"); done
    fi
}
install_item() {  # source target
    local source=$1 target=$2
    [ ! -L "$target" ] || fail "Refusing symbolic-link target: $target"
    rm -rf "$target"
    cp -R "$source" "$target"
    chown -R root:wheel "$target"
    chmod -R go-w "$target"
}

record_path "$oc/Kexts/$GARNET_BUNDLE"
record_path "$oc/Kexts/AMDRadeonRDNA4.kext"
record_path "$oc/Kexts/RDNA4Bringup.kext"
record_path "$oc/Kexts/RDNA4FB.kext"
record_path "$oc/Kexts/Navi48Bringup.kext"
record_path "$R/Library/Extensions/$GARNET_BUNDLE"
record_path "$R/Library/Extensions/$ACCEL_BUNDLE"
record_path "$R/Library/GPUBundles/$METAL_BUNDLE"
for name in activate-native.sh native-bundle-check publish-metal mtlprobe-direct garnettest native-translate profile; do
    record_path "$support/$name"
done
record_path "$R/Library/LaunchDaemons/com.garnet.native.plist"
record_path "$R/Library/LaunchDaemons/com.garnet.native-translate.plist"
for path in "$R/private/tmp/rdna4m" "$R/private/var/tmp/rdna4m-spv" \
    "$R/Library/Logs/RDNA4Translate/translate.stdout" "$R/Library/Logs/RDNA4Translate/translate.stderr"; do
    record_path "$path"
done

rollback() {
    result=$?
    trap - EXIT
    if [ "$committed" -eq 0 ]; then
        [ "$result" -ne 0 ] || result=1
        set +e
        for ((j=${#rollback_targets[@]}-1; j>=0; j--)); do
            rm -rf "${rollback_targets[j]}"
            [ -z "${rollback_backups[j]}" ] || cp -pR "${rollback_backups[j]}" "${rollback_targets[j]}"
        done
        if [ -f "$backup/legacy-map.tsv" ]; then
            while IFS=$'\t' read -r original saved; do
                [ -n "$original" ] || continue
                rm -rf "$original"
                mkdir -p "${original%/*}"
                mv "$saved" "$original"
            done < "$backup/legacy-map.tsv"
        fi
        cp -p "$backup/config-before.plist" "$config"
        if [ -f "$backup/AuxiliaryKernelExtensions.kc" ]; then
            cp -p "$backup/AuxiliaryKernelExtensions.kc" "$aux"
        else
            rm -f "$aux"
        fi
        if [ "${#prior_loaded_jobs[@]}" -gt 0 ]; then
            for d in "${prior_loaded_jobs[@]}"; do
                p="$R/Library/LaunchDaemons/$d.plist"
                if [ -f "$p" ] && ! launch_job_loaded "$d"; then
                    launchctl bootstrap system "$p" >/dev/null 2>&1 \
                        || note "rollback: could not reload $d from $p"
                fi
            done
        fi
        # ensure_dir records deepest-first; remove in that same order.
        for ((j=0; j<${#rollback_dirs[@]}; j++)); do
            rmdir "${rollback_dirs[j]}" >/dev/null 2>&1 || :
        done
        printf 'Installation failed (exit %s). ESP, config, boot-args, Aux KC and userspace restored. Backup: %s\n' \
            "$result" "$backup" >&2
    fi
    rm -rf "$stage"
    exit "$result"
}
committed=0
trap rollback EXIT

ensure_dir "$support"
ensure_dir "$R/Library/Logs/RDNA4"
ensure_dir "$R/Library/Logs/RDNA4Translate"
ensure_dir "$R/Library/GPUBundles"
ensure_dir "$R/Library/LaunchDaemons"
ensure_dir "$R/Library/Extensions"
ensure_dir "$R/Library/KernelCollections"
ensure_dir "$oc/Kexts"

install_item "$work/$GARNET_BUNDLE" "$R/Library/Extensions/$GARNET_BUNDLE"
install_item "$work/$ACCEL_BUNDLE" "$R/Library/Extensions/$ACCEL_BUNDLE"
install_item "$extracted/$METAL_BUNDLE" "$R/Library/GPUBundles/$METAL_BUNDLE"
chmod -R a+rX "$R/Library/Extensions/$GARNET_BUNDLE" "$R/Library/Extensions/$ACCEL_BUNDLE"
for name in activate-native.sh native-bundle-check publish-metal mtlprobe-direct garnettest native-translate; do
    install_item "$work/$name" "$support/$name"
done
printf '%s\n' "$profile" > "$stage/profile"
install_item "$stage/profile" "$support/profile"
chmod 644 "$support/profile"
chmod -R a+rX "$R/Library/GPUBundles/$METAL_BUNDLE"
chmod 755 "${support%/*}" "$support" "$R/Library/Logs/RDNA4" "$R/Library/Logs/RDNA4Translate" \
    "$support/activate-native.sh" "$support/native-bundle-check" "$support/publish-metal" \
    "$support/mtlprobe-direct" "$support/garnettest" "$support/native-translate"

rm -rf "$oc/Kexts/AMDRadeonRDNA4.kext" "$oc/Kexts/RDNA4Bringup.kext" \
    "$oc/Kexts/RDNA4FB.kext" "$oc/Kexts/Navi48Bringup.kext" \
    "$oc/Kexts/$GARNET_BUNDLE"
if [ "$controlled" -eq 1 ]; then
    for d in com.garnet.native com.garnet.native-translate; do
        p="$R/Library/LaunchDaemons/$d.plist"
        if [ -e "$p" ]; then
            launchctl bootout "system/$d" >/dev/null 2>&1 || :
            rm -f "$p"
            note "controlled: removed automatic activation job $d"
        fi
    done
else
    install_item "$work/com.garnet.native.plist" "$R/Library/LaunchDaemons/com.garnet.native.plist"
    install_item "$work/com.garnet.native-translate.plist" \
        "$R/Library/LaunchDaemons/com.garnet.native-translate.plist"
    chmod 644 "$R/Library/LaunchDaemons/com.garnet.native.plist" \
        "$R/Library/LaunchDaemons/com.garnet.native-translate.plist"
fi

# The Aux KC is rebuilt only after Garnet and GarnetAccel exist at their final paths.
# Retired extensions are excluded; foreign extensions are carried forward.
[ ! -L "$aux" ] || fail 'Aux KC must not be a symbolic link'
rebuild_aux "$stage" "$stage/AuxiliaryKernelExtensions.kc" \
    "/Library/Extensions/$GARNET_BUNDLE" "/Library/Extensions/$ACCEL_BUNDLE"
validate_aux "$stage/AuxiliaryKernelExtensions.kc" "$stage"
chown root:wheel "$stage/AuxiliaryKernelExtensions.kc"
chmod 644 "$stage/AuxiliaryKernelExtensions.kc"
[ ! -L "$aux.rdna4-next" ] || fail 'Aux KC staging path must not be a symbolic link'
cp "$stage/AuxiliaryKernelExtensions.kc" "$aux.rdna4-next"
mv "$aux.rdna4-next" "$aux"
note "aux: rebuilt with $GARNET_BUNDLE and $ACCEL_BUNDLE; retired extensions excluded; foreign extensions preserved"

cp "$next" "$oc/config-native-next.plist"
mv "$oc/config-native-next.plist" "$config"
validate_kernel_config "$config" "$GARNET_BUNDLE"

remove_legacy "$backup"
ensure_dir "$R/private/tmp/rdna4m"
ensure_dir "$R/private/var/tmp/rdna4m-spv"
chown root:wheel "$R/private/tmp/rdna4m"; chmod 1777 "$R/private/tmp/rdna4m"
chown nobody:nobody "$R/private/var/tmp/rdna4m-spv"; chmod 755 "$R/private/var/tmp/rdna4m-spv"
for path in "$R/Library/Logs/RDNA4Translate/translate.stdout" "$R/Library/Logs/RDNA4Translate/translate.stderr"; do
    [ ! -L "$path" ] || fail "Refusing symbolic-link translator log: $path"
    touch "$path"; chown nobody:nobody "$path"; chmod 644 "$path"
done
sync
committed=1
trap - EXIT
rm -rf "$stage"
if [ "$controlled" -eq 1 ]; then
    printf 'INSTALLED profile=controlled BACKUP=%s\nBOOT_ARGS=%s\nAUTOMATIC_ACTIVATION=disabled\n' "$backup" "$args"
    printf 'Run the controlled root/in-process payload checks manually; this installer loaded nothing.\n'
else
    printf 'INSTALLED profile=%s BACKUP=%s\nBOOT_ARGS=%s\n' "$profile" "$backup" "$args"
    printf 'Reboot to apply the EFI, Auxiliary KC and boot activation job.\n'
fi
