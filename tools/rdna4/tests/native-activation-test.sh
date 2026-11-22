#!/bin/bash
set -euo pipefail
if [ "${1:-}" = --help ]; then
    printf 'Usage: bash native-activation-test.sh [ACTIVATION_SCRIPT]\nChecks activation order and failed-pixel refusal using stub hardware commands. No driver is loaded.\n'
    exit 0
fi
[ "$#" -le 1 ] || exit 64
source_dir=$(cd "$(dirname "$0")" && pwd)
activation=${1:-$source_dir/../activate-native.sh}
scratch_root=${TMPDIR:-$source_dir/../../../temp/scratch}
mkdir -p "$scratch_root"
work=$(mktemp -d "${scratch_root%/}/native-activation-test.XXXXXXXX")
trap 'rm -rf "$work"' EXIT
cp "$activation" "$work/activate-native.sh"
chmod 700 "$work/activate-native.sh"
for name in sw_vers sysctl ioreg sleep kmutil native-bundle-check native-translate publish-metal mtlprobe-direct garnettest touch chmod pgrep killall; do
    cp "$source_dir/native-activation-stub.sh" "$work/$name"
    chmod 700 "$work/$name"
done
export RDNA4_ACTIVATION_TEST_TRACE="$work/trace"
export PATH="$work:$PATH"
failures=0
cases=0
run_case() {
    local name=$1 profile=$2 probe=$3 missing=${4:-} legacy=${5:-0} no_service=${6:-0} allow=${7:-ok} target=${8:-WallpaperAerialsExtension} rc=0 ok=1
    cases=$((cases + 1))
    printf '%s\n' "$profile" > "$work/profile"
    : > "$RDNA4_ACTIVATION_TEST_TRACE"
    rm -f "$RDNA4_ACTIVATION_TEST_TRACE.service-ready"
    RDNA4_ACTIVATION_FAIL_PROBE=$probe RDNA4_ACTIVATION_MISSING_BOOT=$missing \
        RDNA4_ACTIVATION_LEGACY_FB=$legacy RDNA4_ACTIVATION_NO_SERVICE=$no_service \
        RDNA4_ACTIVATION_APP_ALLOW=$allow \
        RDNA4_ACTIVATION_APP_ALLOW_TARGET=$target \
        bash "$work/activate-native.sh" > "$work/output" 2>&1 || rc=$?
    grep -qx 'kmutil showloaded --list-only' "$work/trace" || ok=0
    ! grep -Eq '^kmutil load -b (as\.vit9696\.Lilu|com\.hackintosh\.|com\.rdna4\.|org\.garnet\.driver)' "$work/trace" || ok=0
    ! grep -Eq '^garnettest accel fbname( |$)' "$work/trace" || ok=0
    if [ -n "$missing" ] || [ "$legacy" = 1 ]; then
        [ "$rc" -ne 0 ] && ! grep -Eq '^(kmutil load|native-bundle-check|native-translate|publish-metal|mtlprobe-direct|garnettest|killall) ' "$work/trace" || ok=0
        if [ -n "$missing" ]; then
            grep -Fq "Required boot component is not loaded: $missing" "$work/output" || ok=0
        else
            grep -q 'Conflicting legacy RDNA4/Navi48 kext is loaded' "$work/output" || ok=0
        fi
    elif [ "$profile" = vm-load-only ]; then
        [ "$rc" -eq 0 ] && ! grep -Eq '^(mtlprobe-direct|garnettest|killall) ' "$work/trace" || ok=0
    elif [ "$no_service" = 1 ] || [ "$no_service" = unregistered ]; then
        [ "$rc" -ne 0 ] && grep -q 'GarnetDriver IOService did not appear' "$work/output" &&
            ! grep -Eq '^(native-translate|publish-metal|mtlprobe-direct|garnettest|killall) ' "$work/trace" || ok=0
    elif [ -n "$probe" ]; then
        [ "$rc" -ne 0 ] && grep -q "^mtlprobe-direct $probe --registry-id 42$" "$work/trace" &&
            ! grep -Eq '^(garnettest|killall) ' "$work/trace" || ok=0
    elif [ "$allow" != ok ] && [ "$allow" != already ]; then
        [ "$rc" -ne 0 ] && grep -Fxq "garnettest accel appallow add $target" "$work/trace" &&
            ! grep -Eq '^(garnettest accel pipe|killall) ' "$work/trace" || ok=0
    else
        [ "$rc" -eq 0 ] || ok=0
        grep -q '^NATIVE_ACTIVATION_ATTEMPTED boot=activation-test-boot registry-id=42;' "$work/output" || ok=0
        awk '
            /^mtlprobe-direct compute / { if (phase == 0) phase = 1; next }
            /^mtlprobe-direct quad / { if (phase == 1) phase = 2; next }
            /^mtlprobe-direct sysdraw / { if (phase == 2) phase = 3; next }
            /^mtlprobe-direct windowserver-lut / { if (phase == 3) phase = 4; next }
            /^garnettest accel appallow add WallpaperAerialsExtension$/ { if (phase == 4) phase = 5; next }
            /^garnettest accel appallow add System Information$/ { if (phase == 5) phase = 6; next }
            /^garnettest accel appallow add system_profiler$/ { if (phase == 6) phase = 7; next }
            /^garnettest accel appallow add System Settings$/ { if (phase == 7) phase = 8; next }
            /^garnettest accel appallow add iconservicesagent$/ { if (phase == 8) phase = 9; next }
            /^garnettest accel appallow add Terminal$/ { if (phase == 9) phase = 10; next }
            /^garnettest accel pipeadopt$/ { if (phase == 10) phase = 11; next }
            /^killall -9 WindowServer$/ { if (phase == 11) phase = 12; next }
            END { exit phase != 12 }
        ' "$work/trace" || ok=0
    fi
    if [ "$ok" -eq 1 ]; then
        printf 'ACTIVATION_CASE_PASS %s\n' "$name"
    else
        printf 'ACTIVATION_CASE_FAIL %s exit=%s\n' "$name" "$rc"
        cat "$work/trace" "$work/output"
        failures=$((failures + 1))
    fi
}
run_case native-pass native ''
run_case compute-failure native compute
run_case quad-failure native quad
run_case system-draw-failure native sysdraw
run_case lut-failure native windowserver-lut
run_case vm-load-only vm-load-only ''
run_case missing-garnet native '' org.garnet.driver
run_case conflicting-legacy native '' '' 1
run_case vm-missing-garnet vm-load-only '' org.garnet.driver
run_case service-absent native '' '' 0 1
run_case service-unregistered native '' '' 0 unregistered
run_case service-registration-delayed native '' '' 0 delayed
run_case wallpaper-already-allowed native '' '' 0 0 already
run_case wallpaper-allowlist-full native '' '' 0 0 full
run_case wallpaper-allowlist-wrong-result native '' '' 0 0 wrong
run_case wallpaper-allowlist-transport-failure native '' '' 0 0 transport
run_case system-information-allowlist-full native '' '' 0 0 full 'System Information'
run_case profiler-allowlist-transport-failure native '' '' 0 0 transport system_profiler
run_case settings-already-allowed native '' '' 0 0 already 'System Settings'
run_case settings-allowlist-full native '' '' 0 0 full 'System Settings'
run_case icons-already-allowed native '' '' 0 0 already iconservicesagent
run_case icons-allowlist-wrong-result native '' '' 0 0 wrong iconservicesagent
run_case icons-allowlist-transport-failure native '' '' 0 0 transport iconservicesagent
run_case terminal-already-allowed native '' '' 0 0 already Terminal
run_case terminal-allowlist-full native '' '' 0 0 full Terminal
run_case terminal-allowlist-wrong-result native '' '' 0 0 wrong Terminal
run_case terminal-allowlist-transport-failure native '' '' 0 0 transport Terminal
printf 'ACTIVATION_SUITE cases=%s failures=%s hardware_commands=stubbed\n' "$cases" "$failures"
[ "$failures" -eq 0 ]
