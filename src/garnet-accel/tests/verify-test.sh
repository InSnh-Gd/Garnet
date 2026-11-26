#!/bin/bash
# Host-only coverage for verify.sh. All macOS/target commands are stubs and
# every filesystem read, including /Library, is redirected below a scratch root.
set -euo pipefail
source_dir=$(cd "$(dirname "$0")" && pwd)
verify="$source_dir/../verify.sh"
if [ -z "${TMPDIR:-}" ]; then
    TMPDIR="${source_dir}/../../../temp/scratch"
    mkdir -p "$TMPDIR"
    export TMPDIR
fi
work=$(mktemp -d "${TMPDIR%/}/garnetaccel-verify-test.XXXXXXXX")
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/bin" "$work/root/Library/KernelCollections" \
    "$work/root/Library/Extensions/Garnet.kext/Contents" \
    "$work/root/Library/Extensions/GarnetAccel.kext/Contents"
printf '{}\n' > "$work/root/Library/Extensions/Garnet.kext/Contents/Info.plist"
printf '{}\n' > "$work/root/Library/Extensions/GarnetAccel.kext/Contents/Info.plist"
printf 'aux fixture\n' > "$work/root/Library/KernelCollections/AuxiliaryKernelExtensions.kc"

cat > "$work/bin/sw_vers" <<'SH'
#!/bin/bash
case "${1:-}" in
    -productVersion) printf '%s\n' "${STUB_PRODUCT:-26.7.2}";;
    -buildVersion) printf '%s\n' "${STUB_BUILD:-25G332}";;
    *) exit 64;;
esac
SH
cat > "$work/bin/kmutil" <<'SH'
#!/bin/bash
case "${1:-}" in
    inspect)
        [ "${STUB_INSPECT_FAIL:-0}" != 1 ] || { echo 'inspect read failed'; exit 9; }
        printf '%s\n' "${STUB_AUX:-org.garnet.driver /Library/Extensions/Garnet.kext
org.garnet.accel /Library/Extensions/GarnetAccel.kext}"
        ;;
    showloaded)
        [ "${STUB_SHOWLOADED_FAIL:-0}" != 1 ] || { echo 'showloaded read failed'; exit 9; }
        for id in ${STUB_LOADED:-org.garnet.driver}; do
            printf '1 0 0x0 %s 0x0\n' "$id"
        done
        ;;
    *) exit 64;;
esac
SH
cat > "$work/bin/ioreg" <<'SH'
#!/bin/bash
exit 0
SH
cat > "$work/bin/nvram" <<'SH'
#!/bin/bash
printf 'boot-args\tgarnet=1 garnet-metal=1\n'
SH
cat > "$work/bin/log" <<'SH'
#!/bin/bash
printf 'garnetaccel: layout PASS 2720/2720\n'
SH
cat > "$work/bin/sudo" <<'SH'
#!/bin/bash
exit 0
SH
cat > "$work/bin/pgrep" <<'SH'
#!/bin/bash
exit 0
SH
cat > "$work/bin/PlistBuddy" <<'SH'
#!/bin/bash
printf '%s\n' "${STUB_VERSION:-0.1.1}"
SH
chmod 700 "$work"/bin/*
export PATH="$work/bin:$PATH"
export GARNET_VERIFY_ROOT="$work/root"
export GARNET_VERIFY_PLISTBUDDY="$work/bin/PlistBuddy"

run_verify() {
    local rc=0
    output=$(bash "$verify" 2>&1) || rc=$?
    [ "$rc" -eq "$1" ] || {
        printf 'verify exit=%s expected=%s\n%s\n' "$rc" "$1" "$output" >&2
        return 1
    }
    printf '%s\n' "$output" > "$work/output"
}

unset STUB_PRODUCT STUB_BUILD STUB_LOADED STUB_AUX STUB_VERSION
unset STUB_INSPECT_FAIL STUB_SHOWLOADED_FAIL
run_verify 0
grep -Fq 'PASS  verification complete' "$work/output"
! grep -Fq 'FAIL  ' "$work/output"

export STUB_LOADED='as.vit9696.Lilu com.hackintosh.AMDRadeonRDNA4 com.hackintosh.RDNA4FB'
export STUB_AUX='org.garnet.accel /Library/Extensions/GarnetAccel.kext
com.hackintosh.RDNA4FB /Library/Extensions/RDNA4FB.kext
com.hackintosh.AMDRadeonRDNA4 /Library/Extensions/AMDRadeonRDNA4.kext'
run_verify 1
grep -Fq 'FAIL  required boot component is not loaded: org.garnet.driver' "$work/output"
grep -Fq 'FAIL  conflicting legacy kext is loaded: as.vit9696.Lilu' "$work/output"
grep -Fq 'FAIL  conflicting legacy kext is loaded: com.hackintosh.AMDRadeonRDNA4' "$work/output"
grep -Fq 'FAIL  conflicting legacy kext is loaded: com.hackintosh.RDNA4FB' "$work/output"
grep -Fq 'FAIL  legacy RDNA4FB is in the aux KC' "$work/output"
grep -Fq 'FAIL  AMDRadeonRDNA4 display plugin is in the aux KC' "$work/output"

unset STUB_LOADED STUB_AUX
export STUB_INSPECT_FAIL=1 STUB_SHOWLOADED_FAIL=1
run_verify 1
grep -Fq 'FAIL  kmutil inspect failed for the Aux KC' "$work/output"
grep -Fq 'FAIL  kmutil showloaded failed' "$work/output"

printf 'VERIFY_SCRIPT_TEST cases=3 failures=0 hardware_commands=stubbed\n'
