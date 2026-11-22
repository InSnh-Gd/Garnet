#!/usr/bin/env bash
# Launch a Vulkan application on macOS against Garnet RADV (VK_EXT_metal_surface / WSI).
# Adapted from RX4Darwin/AMDRadeonRDNA4.kext (userspace/vulkan/radv-app.sh, commit f069d9bf),
# Copyright (c) 2026 Sunneva N. Mariu, licensed under the BSD 3-Clause License
# (see third-party/THIRD-PARTY-LICENSES.txt).
# Compatibility: Bash 3.2 and system tools only.

set -euo pipefail

usage() {
    cat <<'EOF'
Usage: radv-app.sh [--icd PATH] [--print-env] [--no-dyld] [--] COMMAND [ARGS...]
       radv-app.sh [--icd PATH] [--no-dyld] --print-env
EOF
}

fail() {
    printf 'radv-app.sh: %s\n' "$1" >&2
    exit "${2:-1}"
}

repo=$(cd "$(dirname "$0")/../.." && pwd)
icd_arg=""
print_env=0
no_dyld=0

while [ "$#" -gt 0 ]; do
    case "$1" in
        --icd)
            [ "$#" -ge 2 ] || { usage >&2; exit 64; }
            icd_arg=$2
            shift 2
            ;;
        --icd=*)
            icd_arg=${1#--icd=}
            [ -n "$icd_arg" ] || { usage >&2; exit 64; }
            shift
            ;;
        --print-env)
            print_env=1
            shift
            ;;
        --no-dyld)
            no_dyld=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        --)
            shift
            break
            ;;
        -*)
            printf 'radv-app.sh: unknown option: %s\n' "$1" >&2
            usage >&2
            exit 64
            ;;
        *)
            break
            ;;
    esac
done

if [ "$#" -eq 0 ] && [ "$print_env" -ne 1 ]; then
    usage >&2
    exit 64
fi

resolved_icd=""
if [ -n "$icd_arg" ]; then
    [ -f "$icd_arg" ] || fail "specified ICD file does not exist: $icd_arg" 1
    resolved_icd=$icd_arg
elif [ -n "${GARNET_RADV_ICD:-}" ]; then
    [ -f "$GARNET_RADV_ICD" ] || fail "GARNET_RADV_ICD does not exist: $GARNET_RADV_ICD" 1
    resolved_icd=$GARNET_RADV_ICD
else
    for candidate in \
        "$repo/icd/rdna4-production.json" \
        "/Users/Shared/RDNA4HardwareTest/icd/rdna4-production.json" \
        "/Library/GPUBundles/GarnetMetal.bundle/Contents/Resources/radeon_icd.x86_64.json" \
        "/usr/local/share/vulkan/icd.d/radeon_icd.x86_64.json" \
        "/Library/Vulkan/icd.d/radeon_icd.x86_64.json" \
        "/opt/homebrew/share/vulkan/icd.d/radeon_icd.x86_64.json"; do
        if [ -f "$candidate" ]; then
            resolved_icd=$candidate
            break
        fi
    done
fi

[ -n "$resolved_icd" ] || fail "no Garnet RADV ICD manifest found; pass --icd PATH or set GARNET_RADV_ICD" 1

icd_dir=$(cd "$(dirname "$resolved_icd")" && pwd -P)
icd_file="$icd_dir/$(basename "$resolved_icd")"

export VK_ICD_FILENAMES="$icd_file"
export VK_DRIVER_FILES="$icd_file"

if [ "$no_dyld" -ne 1 ] && [ -f "$icd_dir/libvulkan_radeon.dylib" ]; then
    case ":${DYLD_LIBRARY_PATH:-}:" in
        *":$icd_dir:"*) ;;
        *) export DYLD_LIBRARY_PATH="$icd_dir${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}" ;;
    esac
fi

if [ "$print_env" -eq 1 ]; then
    printf 'VK_ICD_FILENAMES=%s\n' "$VK_ICD_FILENAMES"
    printf 'VK_DRIVER_FILES=%s\n' "$VK_DRIVER_FILES"
    if [ -n "${DYLD_LIBRARY_PATH:-}" ]; then
        printf 'DYLD_LIBRARY_PATH=%s\n' "$DYLD_LIBRARY_PATH"
    fi
    if [ "$#" -eq 0 ]; then
        exit 0
    fi
fi

exec "$@"
