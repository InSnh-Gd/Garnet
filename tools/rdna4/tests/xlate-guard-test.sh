#!/bin/bash
# Unit test for tools/rdna4/xlate-guard.sh (symbol and patch-manifest freshness guard).
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../.." && pwd -P)
guard="$repo/tools/rdna4/xlate-guard.sh"
tmp_base="${TMPDIR:-$repo/temp/scratch}"
mkdir -p "$tmp_base"
work=$(mktemp -d "${tmp_base%/}/xlate-guard-test.XXXXXX")
trap 'rm -rf "$work"' EXIT

cat > "$work/nm-full" <<'SH'
#!/usr/bin/env bash
cat <<'EOF'
0000000000001000 T _garnetx_abi_version
0000000000001010 T _garnetx_free
0000000000001020 T _garnetx_translate
0000000000001030 T _garnetx_translate_linked
0000000000001040 T _garnetx_lower_argument_resources
0000000000001050 T _garnetx_translate_with_capabilities
0000000000001060 T _garnetx_translate_linked_with_capabilities
0000000000001070 T _garnetx_lower_argument_resources_with_capabilities
0000000000001080 T _garnetx_lower_icb_stage
0000000000001090 T _garnetx_detect_stage
00000000000010a0 T _garnetx_list_exported_symbols
00000000000010b0 T _garnetx_translate_linked_libraries
00000000000010c0 T _garnetx_translate_stage3
EOF
SH

cat > "$work/nm-stale-pre-m3" <<'SH'
#!/usr/bin/env bash
cat <<'EOF'
0000000000001000 T _garnetx_abi_version
0000000000001010 T _garnetx_free
0000000000001020 T _garnetx_translate
0000000000001030 T _garnetx_translate_linked
0000000000001040 T _garnetx_lower_argument_resources
0000000000001050 T _garnetx_translate_with_capabilities
0000000000001060 T _garnetx_translate_linked_with_capabilities
0000000000001070 T _garnetx_lower_argument_resources_with_capabilities
EOF
SH
chmod 755 "$work/nm-full" "$work/nm-stale-pre-m3"

lib="$work/libgarnetxlate.dylib"
printf 'fake-dylib\n' > "$lib"

# 1. Missing manifest must be rejected even with full exports
if XLATE_GUARD_NM="$work/nm-full" bash "$guard" verify "$repo" "$lib" 2>"$work/err1"; then
    echo "FAIL: missing .patches.sha256 should have failed" >&2; exit 1
fi
grep -q 'missing freshness manifest' "$work/err1"

# 2. Write manifest -> must pass with full exports
bash "$guard" write-manifest "$repo" "$lib"
XLATE_GUARD_NM="$work/nm-full" bash "$guard" verify "$repo" "$lib"

# 3. Missing Metal 3 symbols must be rejected even with valid manifest and RDNA4_ALLOW_STALE_XLATE=1
if XLATE_GUARD_NM="$work/nm-stale-pre-m3" RDNA4_ALLOW_STALE_XLATE=1 bash "$guard" verify "$repo" "$lib" 2>"$work/err3"; then
    echo "FAIL: missing Metal 3 symbols should have failed" >&2; exit 1
fi
grep -q 'garnetx_translate_stage3' "$work/err3"
grep -q 'garnetx_lower_icb_stage' "$work/err3"

# 4. Stale manifest digest must be rejected by default
printf '0000000000000000000000000000000000000000000000000000000000000000  third-party/metal2vulkan-rdna4\n' > "${lib}.patches.sha256"
if XLATE_GUARD_NM="$work/nm-full" bash "$guard" verify "$repo" "$lib" 2>"$work/err4"; then
    echo "FAIL: stale digest should have failed" >&2; exit 1
fi
grep -q 'stale libgarnetxlate.dylib' "$work/err4"

# 5. RDNA4_ALLOW_STALE_XLATE=1 bypasses manifest check when symbols are intact
XLATE_GUARD_NM="$work/nm-full" RDNA4_ALLOW_STALE_XLATE=1 bash "$guard" verify "$repo" "$lib" 2>"$work/err5"
grep -q 'WARNING: bypassing' "$work/err5"

echo "xlate-guard-test: all 5 checks passed"
