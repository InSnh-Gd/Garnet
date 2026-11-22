#!/bin/bash
# Remove this Garnet delivery and retired pre-Garnet artefacts.
#   sudo bash uninstall-native.sh [--check] [--legacy-only] ESP_MOUNT
# --check prints the plan and changes nothing. --legacy-only removes only the
# retired components and keeps the current Garnet install.
# Garnet.kext and GarnetAccel.kext are removed from the Aux KC (/Library/Extensions)
# while every foreign extension is preserved (any residual ESP Garnet.kext is also
# removed). Removed files are moved into a timestamped backup so a failure can
# restore ESP, config, boot-args, Aux KC and userspace state.
set -euo pipefail
umask 077
usage() { printf 'Usage: sudo bash uninstall-native.sh [--check] [--legacy-only] ESP_MOUNT\n'; }
check=0
legacy_only=0
while [ "$#" -gt 0 ]; do
    case "$1" in
        --help) usage; exit 0;;
        --check) check=1; shift;;
        --legacy-only) legacy_only=1; shift;;
        --*) usage >&2; exit 2;;
        *) break;;
    esac
done
[ "$#" -eq 1 ] || { usage >&2; exit 2; }
work=$(cd "$(dirname "$0")" && pwd)
. "$work/deploy-common.sh"
[ "$(uname -s)" = Darwin ] || fail 'Run in installed macOS'
[ "$(id -u)" -eq 0 ] || fail 'Run with sudo'
vmm=0
case " $(sysctl -n machdep.cpu.features 2>/dev/null || :) " in *' VMM '*) vmm=1;; esac
[ "$check" -eq 1 ] || [ "$vmm" -eq 0 ] || fail 'Actual uninstall in a VM is disabled; use --check'

esp=$(cd "$1" && pwd -P)
oc="$esp/EFI/OC"
config="$oc/config.plist"
[ -f "$config" ] && [ ! -L "$config" ] || fail 'Expected an OpenCore ESP'
plutil -lint "$config" >/dev/null
if [ "$check" -eq 1 ]; then
    stage=$(mktemp -d "${TMPDIR:-/tmp}/rdna4-uninstall-check.XXXXXXXX")
else
    mkdir -p "$R/private/tmp"
    stage=$(mktemp -d "$R/private/tmp/rdna4-uninstall.XXXXXXXX")
fi
cleanup_stage() { rm -rf "$stage"; }
trap cleanup_stage EXIT

managed_bundle() {  # path, id, executable, label
    [ -e "$1" ] || [ -L "$1" ] || return 0
    verify_bundle_identity "$1" "$2" "$3" "$4"
}
managed_bundle "$oc/Kexts/$GARNET_BUNDLE" "$GARNET_BUNDLE_ID" "$GARNET_EXECUTABLE" \
    'residual ESP Garnet'
managed_bundle "$R/Library/Extensions/$GARNET_BUNDLE" "$GARNET_BUNDLE_ID" "$GARNET_EXECUTABLE" \
    'Library Garnet'
managed_bundle "$R/Library/Extensions/$ACCEL_BUNDLE" "$ACCEL_BUNDLE_ID" "$ACCEL_EXECUTABLE" \
    'Library GarnetAccel'
managed_bundle "$R/Library/GPUBundles/$METAL_BUNDLE" "$METAL_BUNDLE_ID" GarnetMetal \
    'Library GarnetMetal'
managed_bundle "$oc/Kexts/AMDRadeonRDNA4.kext" com.hackintosh.AMDRadeonRDNA4 AMDRadeonRDNA4 \
    'legacy ESP AMDRadeonRDNA4'
managed_bundle "$oc/Kexts/RDNA4Bringup.kext" com.rdna4.bringup RDNA4Bringup \
    'legacy ESP RDNA4Bringup'
managed_bundle "$oc/Kexts/RDNA4FB.kext" com.hackintosh.RDNA4FB RDNA4FB \
    'legacy ESP RDNA4FB'
managed_bundle "$oc/Kexts/Navi48Bringup.kext" com.navi48.bringup Navi48Bringup \
    'legacy ESP Navi48Bringup'
verify_legacy_bundles

current_paths() {
    if [ "$legacy_only" -eq 1 ]; then
        printf '%s\n' "$oc/Kexts/AMDRadeonRDNA4.kext" "$oc/Kexts/RDNA4Bringup.kext" \
            "$oc/Kexts/RDNA4FB.kext" "$oc/Kexts/Navi48Bringup.kext"
        return 0
    fi
    printf '%s\n' \
        "$R/Library/Extensions/$ACCEL_BUNDLE" \
        "$R/Library/Extensions/$GARNET_BUNDLE" \
        "$R/Library/GPUBundles/$METAL_BUNDLE" \
        "$support/activate-native.sh" "$support/native-bundle-check" "$support/publish-metal" \
        "$support/mtlprobe-direct" "$support/garnettest" "$support/native-translate" "$support/profile" \
        "$R/Library/LaunchDaemons/com.garnet.native.plist" \
        "$R/Library/LaunchDaemons/com.garnet.native-translate.plist" \
        "$oc/Kexts/$GARNET_BUNDLE" \
        "$oc/Kexts/AMDRadeonRDNA4.kext" "$oc/Kexts/RDNA4Bringup.kext" \
        "$oc/Kexts/RDNA4FB.kext" "$oc/Kexts/Navi48Bringup.kext"
}

next="$stage/config-next.plist"
cp "$config" "$next"
if [ "$legacy_only" -eq 1 ]; then
    kernel_delete_bundle "$next" AMDRadeonRDNA4.kext || fail 'Cannot remove AMDRadeonRDNA4.kext'
    kernel_delete_bundle "$next" RDNA4FB.kext || fail 'Cannot remove RDNA4FB.kext'
    kernel_delete_bundle "$next" Navi48FB.kext || fail 'Cannot remove Navi48FB.kext'
    kernel_disable_bundle "$next" RDNA4Bringup.kext || fail 'Cannot disable RDNA4Bringup.kext'
    kernel_disable_bundle "$next" RDNA4Accel.kext || fail 'Cannot disable RDNA4Accel.kext'
    kernel_disable_bundle "$next" Navi48Bringup.kext || fail 'Cannot disable Navi48Bringup.kext'
    kernel_disable_bundle "$next" Navi48Accel.kext || fail 'Cannot disable Navi48Accel.kext'
else
    kernel_delete_bundle "$next" "$GARNET_BUNDLE" || fail "Cannot remove $GARNET_BUNDLE"
    kernel_delete_bundle "$next" AMDRadeonRDNA4.kext || fail 'Cannot remove AMDRadeonRDNA4.kext'
    kernel_delete_bundle "$next" RDNA4FB.kext || fail 'Cannot remove RDNA4FB.kext'
    kernel_delete_bundle "$next" Navi48FB.kext || fail 'Cannot remove Navi48FB.kext'
    kernel_disable_bundle "$next" "$ACCEL_BUNDLE" || fail "Cannot disable $ACCEL_BUNDLE"
    kernel_disable_bundle "$next" RDNA4Bringup.kext || fail 'Cannot disable RDNA4Bringup.kext'
    kernel_disable_bundle "$next" RDNA4Accel.kext || fail 'Cannot disable RDNA4Accel.kext'
    kernel_disable_bundle "$next" Navi48Bringup.kext || fail 'Cannot disable Navi48Bringup.kext'
    kernel_disable_bundle "$next" Navi48Accel.kext || fail 'Cannot disable Navi48Accel.kext'
fi
old_args=$($buddy -c "Print :NVRAM:Add:$guid:boot-args" "$next" 2>/dev/null) || old_args=
if [ "$legacy_only" -eq 1 ]; then
    args=$(migrate_args "$old_args")
else
    args=$(strip_args "$old_args")
fi
[ -z "$old_args" ] || $buddy -c "Set :NVRAM:Add:$guid:boot-args $args" "$next"
plutil -lint "$next" >/dev/null

if [ "$check" -eq 1 ]; then
    printf 'UNINSTALL PLAN legacy_only=%s ESP=%s\nBOOT_ARGS=%s\n' "$legacy_only" "$esp" "$args"
    current_paths | while IFS= read -r p; do [ ! -e "$p" ] || printf 'REMOVE=%s\n' "$p"; done
    for d in com.garnet.native com.garnet.native-translate; do
        p="$R/Library/LaunchDaemons/$d.plist"
        [ ! -e "$p" ] || printf 'STOP=%s\n' "$d"
    done
    [ "$legacy_only" -eq 1 ] || printf 'KERNEL_REMOVE=%s\nKERNEL_DISABLE=%s\n' "$GARNET_BUNDLE" "$ACCEL_BUNDLE"
    printf 'AUX_REBUILD=preserve-foreign remove='
    if [ "$legacy_only" -eq 1 ]; then printf 'retired-RDNA4-and-Navi48\n'; else printf 'managed-Garnet-and-retired\n'; fi
    while IFS= read -r p; do [ -n "$p" ] && printf 'LEGACY_REMOVE=%s\n' "$p"; done < <(legacy_present)
    printf 'CHANGES_TARGET=no\n'
    trap - EXIT
    rm -rf "$stage"
    exit 0
fi

prior_loaded_jobs=()
for d in $LEGACY_DAEMONS com.garnet.native com.garnet.native-translate; do
    launch_job_loaded "$d" && prior_loaded_jobs+=("$d")
done

mkdir -p "$R/Library/Application Support/RDNA4-backups"
backup=$(mktemp -d "$R/Library/Application Support/RDNA4-backups/uninstall.XXXXXXXX")
cp -p "$config" "$backup/config-before.plist"
[ ! -f "$aux" ] || cp -p "$aux" "$backup/AuxiliaryKernelExtensions.kc"
removed_map="$backup/removed.tsv"
: > "$removed_map"
remove_item() {  # path
    local path=$1 n dest
    [ -e "$path" ] || [ -L "$path" ] || return 0
    [ ! -L "$path" ] || fail "Refusing symbolic-link managed path: $path"
    n=$(wc -l < "$removed_map" | tr -d ' ')
    dest="$backup/items/$n-${path##*/}"
    mkdir -p "${dest%/*}"
    printf '%s\t%s\n' "$path" "$dest" >> "$removed_map"
    mv "$path" "$dest"
    note "removed: $path -> $dest"
}

# Rebuild before moving GarnetAccel.kext: kmutil needs the source path. Nothing
# in the target has changed yet, so a rebuild failure leaves the system intact.
if [ -f "$aux" ]; then
    [ ! -L "$aux" ] || fail 'Aux KC must not be a symbolic link'
    saved_aux_ours=$AUX_OURS
    [ "$legacy_only" -eq 0 ] || AUX_OURS='com.rdna4.accelprobe com.hackintosh.RDNA4FB com.hackintosh.AMDRadeonRDNA4 com.rdna4.bringup com.navi48.accelprobe com.navi48.bringup'
    rc=0
    rebuild_aux "$stage" "$stage/aux.kc" || rc=$?
    AUX_OURS=$saved_aux_ours
    case "$rc" in
        0) ;;
        2) ;;
        *) fail "Aux KC rebuild failed ($rc); nothing else was changed. Backup: $backup";;
    esac
fi

committed=0
rollback() {
    result=$?
    trap - EXIT
    if [ "$committed" -eq 0 ]; then
        [ "$result" -ne 0 ] || result=1
        set +e
        if [ -f "$removed_map" ]; then
            while IFS=$'\t' read -r original saved; do
                [ -n "$original" ] || continue
                mkdir -p "${original%/*}"
                mv "$saved" "$original"
            done < "$removed_map"
        fi
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
        printf 'Uninstall failed (exit %s). ESP, config, boot-args, Aux KC and userspace restored. Backup: %s\n' \
            "$result" "$backup" >&2
    fi
    rm -rf "$stage"
    exit "$result"
}
trap rollback EXIT

if [ -f "$aux" ]; then
    if [ "$rc" -eq 0 ]; then
        [ ! -L "$aux.rdna4-next" ] || fail 'Aux KC staging path must not be a symbolic link'
        cp "$stage/aux.kc" "$aux.rdna4-next"
        chown root:wheel "$aux.rdna4-next"; chmod 644 "$aux.rdna4-next"
        mv "$aux.rdna4-next" "$aux"
        note 'aux: rebuilt, foreign extensions preserved'
    else
        rm -f "$aux"
        note 'aux: no extension left, removed (backup kept)'
    fi
fi

if [ "$legacy_only" -eq 0 ]; then
    for d in com.garnet.native com.garnet.native-translate; do
        launch_job_loaded "$d" && launchctl bootout "system/$d" >/dev/null 2>&1 || :
    done
fi

while IFS= read -r p; do
    remove_item "$p"
done < <(current_paths)
if [ "$legacy_only" -eq 0 ]; then
    for p in "$R/private/tmp/rdna4m" "$R/private/var/tmp/rdna4m-spv" \
        "$R"/private/tmp/rdna4m-* "$R"/private/var/tmp/rdna4m-*; do
        [ -e "$p" ] || [ -L "$p" ] || continue
        remove_item "$p"
    done
fi

cp "$next" "$oc/config-uninstall-next.plist"
mv "$oc/config-uninstall-next.plist" "$config"
remove_legacy "$backup"
sync
committed=1
trap - EXIT
rm -rf "$stage"
printf 'UNINSTALLED legacy_only=%s BACKUP=%s\nBOOT_ARGS=%s\nReboot to apply.\n' \
    "$legacy_only" "$backup" "$args"
