#!/bin/bash
# Check the real bundle, then reject foreign backends and incomplete translators without GPU work.
set -euo pipefail
[ "$#" -eq 2 ] || { printf 'Usage: native-bundle-check-test.sh CHECKER BUNDLE\n' >&2; exit 64; }
checker=$1
bundle=$2
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../.." && pwd)
scratch_root=${TMPDIR:-$repo/temp/scratch}
mkdir -p "$scratch_root"
work=$(mktemp -d "${scratch_root%/}/rdna4-bundle-check.XXXXXXXX")
trap 'rm -rf "$work"' EXIT
"$checker" "$bundle" > "$work/current.log" 2>&1
grep -Fq 'PASS translator ABI: 3' "$work/current.log"
grep -Fq 'PASS load-only verification; no device instantiated' "$work/current.log"
cp -R "$bundle" "$work/GarnetMetal.bundle"
library="$work/GarnetMetal.bundle/Contents/Resources/libvulkan_radeon.dylib"
cat > "$work/backend.c" <<'C'
void *vk_icdGetInstanceProcAddr(void) { return 0; }
#ifndef FOREIGN_BACKEND
void radv_darwin_set_service_class(void) {}
#ifndef MISSING_LIMITS
void radv_darwin_get_argument_limits_v1(void) {}
#endif
void radv_darwin_buffer_copy(void) {}
void radv_darwin_buffer_fill(void) {}
#ifndef MISSING_WAIT
void radv_darwin_buffer_wait(void) {}
#endif
#endif
C
cat > "$work/translator.c" <<'C'
#include <stdint.h>
uint32_t garnetx_abi_version(void) { return TEST_ABI; }
void garnetx_translate(void) {}
void garnetx_free(void) {}
void garnetx_translate_linked(void) {}
#ifndef MISSING_CAPABILITIES
void garnetx_translate_with_capabilities(void) {}
void garnetx_translate_linked_with_capabilities(void) {}
void garnetx_lower_argument_resources_with_capabilities(void) {}
#endif
C
reject() {
    local marker=$1 rc=0
    "$checker" "$work/GarnetMetal.bundle" > "$work/rejected.log" 2>&1 || rc=$?
    [ "$rc" -eq 4 ]
    grep -Fq "$marker" "$work/rejected.log"
    ! grep -Fq 'PASS NSBundle load:' "$work/rejected.log"
}
for mode in FOREIGN_BACKEND MISSING_LIMITS MISSING_WAIT; do
    clang -arch x86_64 -dynamiclib -D"$mode" "$work/backend.c" -o "$library"
    codesign --force --sign - "$library"
    case "$mode" in
        FOREIGN_BACKEND) reject radv_darwin_set_service_class;;
        MISSING_LIMITS) reject radv_darwin_get_argument_limits_v1;;
        MISSING_WAIT) reject radv_darwin_buffer_wait;;
    esac
done
cp "$bundle/Contents/Resources/libvulkan_radeon.dylib" "$library"
library="$work/GarnetMetal.bundle/Contents/Resources/libgarnetxlate.dylib"
clang -arch x86_64 -dynamiclib -DTEST_ABI=2 "$work/translator.c" -o "$library"
codesign --force --sign - "$library"
reject 'translator ABI expected 3, got 2'
clang -arch x86_64 -dynamiclib -DTEST_ABI=3 -DMISSING_CAPABILITIES "$work/translator.c" -o "$library"
codesign --force --sign - "$library"
reject 'garnetx_translate_with_capabilities'
printf 'PASS bundle verifier: native backend accepted; foreign/incomplete backends, ABI 2 and missing capability exports refused before bundle load\n'
