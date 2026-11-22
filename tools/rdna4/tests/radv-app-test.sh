#!/usr/bin/env bash
set -euo pipefail

tests=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$tests/../../.." && pwd)
launcher=${1:-$tests/../radv-app.sh}
scratch_root=${TMPDIR:-$repo/temp/scratch}
mkdir -p "$scratch_root"
work=$(mktemp -d "${scratch_root%/}/garnet-radv-app-test.XXXXXXXX")
trap 'rm -rf "$work"' EXIT

bash -n "$launcher"

# 1. Missing command returns usage code 64.
set +e
bash "$launcher" >/dev/null 2>"$work/usage.err"
rc=$?
set -e
[ "$rc" -eq 64 ]

# 2. Unknown option returns 64.
set +e
bash "$launcher" --no-such-flag true >/dev/null 2>"$work/opt.err"
rc=$?
set -e
[ "$rc" -eq 64 ]

# 3. Missing explicit --icd and missing GARNET_RADV_ICD return 1.
set +e
bash "$launcher" --icd "$work/nonexistent.json" true >/dev/null 2>"$work/missing-arg.err"
rc=$?
set -e
[ "$rc" -eq 1 ]

set +e
GARNET_RADV_ICD="$work/nonexistent-env.json" bash "$launcher" true >/dev/null 2>"$work/missing-env.err"
rc=$?
set -e
[ "$rc" -eq 1 ]

# 4. Repo-relative default ICD auto-discovery works without --icd or GARNET_RADV_ICD.
mkdir -p "$work/fake-repo/tools/rdna4" "$work/fake-repo/icd"
cp "$launcher" "$work/fake-repo/tools/rdna4/radv-app.sh"
chmod +x "$work/fake-repo/tools/rdna4/radv-app.sh"
printf '{"ICD":{"library_path":"/tmp/libvulkan_radeon.dylib"}}\n' > "$work/fake-repo/icd/rdna4-production.json"
out_auto=$(env -u GARNET_RADV_ICD -u VK_ICD_FILENAMES -u VK_DRIVER_FILES -u DYLD_LIBRARY_PATH \
    "$work/fake-repo/tools/rdna4/radv-app.sh" --print-env)
fake_icd_real=$(cd "$work/fake-repo/icd" && pwd -P)/rdna4-production.json
printf '%s\n' "$out_auto" | grep -Fqx "VK_ICD_FILENAMES=$fake_icd_real"
printf '%s\n' "$out_auto" | grep -Fqx "VK_DRIVER_FILES=$fake_icd_real"

# 5. Fixture ICD without sibling dylib -> no DYLD_LIBRARY_PATH injection.
mkdir -p "$work/icd-only" "$work/icd-with-dylib"
printf '{"ICD":{"library_path":"/tmp/libvulkan_radeon.dylib"}}\n' > "$work/icd-only/radeon_icd.json"
printf '{"ICD":{"library_path":"libvulkan_radeon.dylib"}}\n' > "$work/icd-with-dylib/radeon_icd.json"
: > "$work/icd-with-dylib/libvulkan_radeon.dylib"

out=$(env -u VK_ICD_FILENAMES -u VK_DRIVER_FILES -u DYLD_LIBRARY_PATH \
    "$launcher" --icd "$work/icd-only/radeon_icd.json" --print-env)
icd_only_real=$(cd "$work/icd-only" && pwd -P)/radeon_icd.json
printf '%s\n' "$out" | grep -Fqx "VK_ICD_FILENAMES=$icd_only_real"
printf '%s\n' "$out" | grep -Fqx "VK_DRIVER_FILES=$icd_only_real"
if printf '%s\n' "$out" | grep -q '^DYLD_LIBRARY_PATH='; then
    printf 'unexpected DYLD_LIBRARY_PATH without sibling dylib\n' >&2
    exit 1
fi

# 6. Sibling libvulkan_radeon.dylib prepends DYLD_LIBRARY_PATH unless --no-dyld is passed.
icd_dylib_dir=$(cd "$work/icd-with-dylib" && pwd -P)
out_dyld=$(env -u VK_ICD_FILENAMES -u VK_DRIVER_FILES DYLD_LIBRARY_PATH=/existing/lib \
    GARNET_RADV_ICD="$work/icd-with-dylib/radeon_icd.json" bash "$launcher" --print-env)
printf '%s\n' "$out_dyld" | grep -Fqx "VK_ICD_FILENAMES=$icd_dylib_dir/radeon_icd.json"
printf '%s\n' "$out_dyld" | grep -Fqx "VK_DRIVER_FILES=$icd_dylib_dir/radeon_icd.json"
printf '%s\n' "$out_dyld" | grep -Fqx "DYLD_LIBRARY_PATH=$icd_dylib_dir:/existing/lib"

out_nodyld=$(env -u VK_ICD_FILENAMES -u VK_DRIVER_FILES -u DYLD_LIBRARY_PATH \
    "$launcher" --icd "$work/icd-with-dylib/radeon_icd.json" --no-dyld --print-env)
if printf '%s\n' "$out_nodyld" | grep -q '^DYLD_LIBRARY_PATH='; then
    printf 'unexpected DYLD_LIBRARY_PATH with --no-dyld\n' >&2
    exit 1
fi

# 7. Exec propagates VK_ICD_FILENAMES, VK_DRIVER_FILES, DYLD_LIBRARY_PATH and preserves `--` args.
exec_out=$(env -u VK_ICD_FILENAMES -u VK_DRIVER_FILES -u DYLD_LIBRARY_PATH \
    "$launcher" --icd "$work/icd-with-dylib/radeon_icd.json" -- \
    bash -c 'printf "%s|%s|%s|%s\n" "$VK_ICD_FILENAMES" "$VK_DRIVER_FILES" "$DYLD_LIBRARY_PATH" "$1"' _ --app-flag)
[ "$exec_out" = "$icd_dylib_dir/radeon_icd.json|$icd_dylib_dir/radeon_icd.json|$icd_dylib_dir|--app-flag" ]

printf 'RADV_APP_PASS cases=7\n'
