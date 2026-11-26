#!/bin/bash
# Compute and verify the source-tree digest and required C ABI exports for libgarnetxlate.dylib.
# Usage:
#   xlate-guard.sh digest         <repo_root>
#   xlate-guard.sh write-manifest <repo_root> <libgarnetxlate.dylib>
#   xlate-guard.sh verify         <repo_root> <libgarnetxlate.dylib>
#
# Environment:
#   RDNA4_ALLOW_STALE_XLATE=1   Bypass the sidecar .patches.sha256 freshness check (never bypasses
#                               the required exported-symbol check).
set -euo pipefail
export LC_ALL=C

REQUIRED_SYMBOLS=(
    garnetx_abi_version
    garnetx_free
    garnetx_translate
    garnetx_translate_linked
    garnetx_lower_argument_resources
    garnetx_translate_with_capabilities
    garnetx_translate_linked_with_capabilities
    garnetx_lower_argument_resources_with_capabilities
    garnetx_lower_icb_stage
    garnetx_detect_stage
    garnetx_list_exported_symbols
    garnetx_translate_linked_libraries
    garnetx_translate_stage3
)

fail() { printf 'xlate-guard.sh: %s\n' "$*" >&2; exit 1; }

sha256_file() {
    if command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | awk '{print $1}'
    elif command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    else
        fail 'neither shasum nor sha256sum found'
    fi
}

sha256_stdin() {
    if command -v shasum >/dev/null 2>&1; then
        shasum -a 256 | awk '{print $1}'
    elif command -v sha256sum >/dev/null 2>&1; then
        sha256sum | awk '{print $1}'
    else
        fail 'neither shasum nor sha256sum found'
    fi
}

compute_digest() {
    local repo=$1
    local xlate_dir="$repo/third-party/metal2vulkan-rdna4"
    [ -d "$xlate_dir/patches" ] && [ -d "$xlate_dir/garnetxlate" ] \
        || fail "missing third-party/metal2vulkan-rdna4 under $repo"
    (
        cd "$xlate_dir"
        find patches garnetxlate -type f ! -path '*/target/*' -print | LC_ALL=C sort | while IFS= read -r rel; do
            printf '%s  %s\n' "$(sha256_file "$rel")" "$rel"
        done
    ) | sha256_stdin
}

list_exports() {
    local lib=$1
    local nm_bin=${XLATE_GUARD_NM:-nm}
    if [ "$(uname -s)" = Darwin ] && [ -z "${XLATE_GUARD_NM:-}" ]; then
        "$nm_bin" -arch x86_64 -gU "$lib"
    else
        "$nm_bin" -gU "$lib"
    fi
}

cmd=${1:-}
case "$cmd" in
    digest)
        [ "$#" -eq 2 ] || fail 'usage: xlate-guard.sh digest <repo_root>'
        compute_digest "$2"
        ;;
    write-manifest)
        [ "$#" -eq 3 ] || fail 'usage: xlate-guard.sh write-manifest <repo_root> <lib>'
        repo=$2; lib=$3
        [ -f "$lib" ] || fail "dylib not found: $lib"
        digest=$(compute_digest "$repo")
        printf '%s  third-party/metal2vulkan-rdna4\n' "$digest" > "${lib}.patches.sha256"
        ;;
    verify)
        [ "$#" -eq 3 ] || fail 'usage: xlate-guard.sh verify <repo_root> <lib>'
        repo=$2; lib=$3
        [ -f "$lib" ] || fail "dylib not found: $lib"
        exports=$(list_exports "$lib") || fail "cannot inspect exports of $lib"
        missing=""
        for sym in "${REQUIRED_SYMBOLS[@]}"; do
            if ! printf '%s\n' "$exports" | awk -v s1="_$sym" -v s2="$sym" '$NF == s1 || $NF == s2 { found=1 } END { exit !found }'; then
                missing="${missing}${missing:+ }${sym}"
            fi
        done
        if [ -n "$missing" ]; then
            fail "stale or incompatible libgarnetxlate.dylib ($lib): missing required symbol(s): ${missing}; rebuild with tools/rdna4/build-xlate.sh"
        fi
        if [ "${RDNA4_ALLOW_STALE_XLATE:-0}" = 1 ]; then
            printf 'xlate-guard.sh: WARNING: bypassing .patches.sha256 freshness check for %s (RDNA4_ALLOW_STALE_XLATE=1)\n' "$lib" >&2
            exit 0
        fi
        manifest="${lib}.patches.sha256"
        expected=$(compute_digest "$repo")
        [ -f "$manifest" ] || fail "missing freshness manifest ${manifest} (expected digest ${expected}); rebuild with tools/rdna4/build-xlate.sh or set RDNA4_ALLOW_STALE_XLATE=1"
        actual=$(awk 'NR==1 {print $1; exit}' "$manifest")
        [ "$actual" = "$expected" ] || fail "stale libgarnetxlate.dylib ($lib): manifest digest ${actual:-<empty>} != current third-party/metal2vulkan-rdna4 digest ${expected}; rebuild with tools/rdna4/build-xlate.sh or set RDNA4_ALLOW_STALE_XLATE=1"
        ;;
    *)
        fail 'usage: xlate-guard.sh {digest|write-manifest|verify} ...'
        ;;
esac
