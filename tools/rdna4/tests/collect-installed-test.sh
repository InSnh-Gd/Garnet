#!/bin/bash
# Stub integration checks; optional argument preserves all fixtures as evidence.
set -eu
set -o pipefail
umask 077
here=$(cd "$(dirname "$0")/.." && pwd)
scratch_root=${TMPDIR:-$here/../../temp/scratch}
mkdir -p "$scratch_root"
evidence=${1:-$(mktemp -d "${scratch_root%/}/rdna4-collector-test.XXXXXXXX")}
mkdir -p "$evidence"
evidence=$(cd "$evidence" && pwd)
printf 'scenario\tinvocation\tobservable\tartifact\n' > "$evidence/criteria.tsv"
root="$evidence/stubs"
mkdir -p "$root/bin" "$root/usr/bin" "$root/usr/sbin"
for name in mkdir chmod cat sleep bash; do ln -s "$(command -v "$name")" "$root/bin/$name"; done
for name in mktemp awk uname tail find; do ln -s "$(command -v "$name")" "$root/usr/bin/$name"; done
# One dispatcher records actual invocations and supplies native-command failures.
export FIXTURE_PATH="$PATH"
export FIXTURE_TAR=$(command -v tar)
printf '#!%s\n' "$BASH" > "$root/stub"
cat >> "$root/stub" <<'STUB'
export PATH="$FIXTURE_PATH"
name=${0##*/}
printf '%s' "$name" >> "$CALLS"
printf ' <%s>' "$@" >> "$CALLS"
printf '\n' >> "$CALLS"
case "$name" in
    date)
        case "$1" in
            +%s) printf '2000\n';;
            -r) printf 'epoch-%s\n' "$2";;
            *) printf 'test-time\n';;
        esac;;
    sysctl)
        case "$2" in
            kern.boottime)
                [ "$SCENARIO" != boot-failure ] || { printf 'boot read failed\n' >&2; exit 9; }
                if [ "$SCENARIO" = old-boot ]; then printf '{ sec = 100, usec = 0 }\n'
                else printf '{ sec = 1900, usec = 0 }\n'; fi;;
            kern.bootsessionuuid) printf 'CURRENT-BOOT-UUID\n';;
            *) printf 'sysctl-value\n';;
        esac;;
    kmutil) [ "$*" = showloaded ] || exit 99; printf 'kmutil unavailable\n' >&2; exit 7;;
    diskutil) [ "$*" = list ] || exit 99; printf 'diskutil diagnostic\n';;
    system_profiler)
        [ "${GARNET_ALLOW:-}" = 1 ] || exit 96
        printf 'partial display information\n'
        # A delayed child exposes timers that kill only the parent shell.
        (sleep 3; printf 'escaped\n' > "$ESCAPED") &
        wait;;
    ps)
        printf ' PID PPID UID STAT ELAPSED COMMAND\n'
        [ "$SCENARIO" != no-service ] || exit 0
        printf ' 10 1 0 S 00:01 /System/WindowServer\n'
        printf ' 11 1 0 S 00:01 /System/loginwindow\n'
        printf ' 12 1 0 S 00:01 /System/SetupAssistant\n'
        printf ' 13 1 0 S 00:01 unrelated-process\n'
        for ((i=0; i<100; i++)); do printf ' 20 1 0 S 00:01 /System/WindowServer\n'; done;;
    sample)
        [ "$*" = '10 1 10 -file /dev/stdout' ] ||
            [ "$*" = '20 1 10 -file /dev/stdout' ] || exit 99
        [ "$SCENARIO" != native-probe ] || { printf 'sample permission denied\n' >&2; exit 7; }
        if [ "$SCENARIO" = old-boot ]; then
            (sleep 3; printf 'escaped\n' > "$ESCAPED") &
            wait
        fi
        printf 'WindowServer stack sentinel\n';;
    garnettest)
        case "$*" in
            info|log|counters|'accel pipestat'|'accel sessstat '[0-8]) ;;
            *) exit 99;;
        esac
        if [ "$SCENARIO" = native-probe ]; then
            case "$0" in */Garnet/native/garnettest) ;; *) printf 'obsolete probe used\n' >&2; exit 97;; esac
        fi
        [ "$SCENARIO" != no-service ] || { printf 'GarnetDriver not found\n' >&2; exit 1; }
        printf 'driver-ring-%s-sentinel\n' "$1";;
    log)
        printf 'WindowServer[10:abc] GarnetMetal: FALLBACK VfxU10 / Timg (spvcache miss for fragment: /Library/GPUBundles/GarnetMetal.bundle/Contents/Resources/spvcache/%s.spv)\n' "$SHADER_KEY" "$SYMLINK_KEY" "$AIR_ONLY_KEY";;
    tar) exec "$FIXTURE_TAR" "$@";;
    *) printf '%s diagnostic\n' "$name";;
esac
STUB
chmod 700 "$root/stub"
for name in sw_vers kmutil log sample tar; do ln -s "$root/stub" "$root/usr/bin/$name"; done
for name in sysctl ioreg diskutil system_profiler; do ln -s "$root/stub" "$root/usr/sbin/$name"; done
for name in date ps; do ln -s "$root/stub" "$root/bin/$name"; done
mkdir -p "$root/Library/Application Support/RDNA4"
ln -s "$root/stub" "$root/Library/Application Support/RDNA4/garnettest"
mkdir -p "$root/private/var/tmp"
export SHADER_KEY=3b5c3b282f057ab872028db2598f50cfa633280af773ccc7d2dc65351b9cb0da
export SYMLINK_KEY=1111111111111111111111111111111111111111111111111111111111111111
export AIR_ONLY_KEY=8934b558f252a86f23a970a823e6adec543346c3c1486580f149a5e803476432
extra_key=2222222222222222222222222222222222222222222222222222222222222222
shader_cache="$root/private/var/tmp/garnet-spv"
mkdir -p "$shader_cache"
printf 'compiled-fragment\n' > "$shader_cache/$SHADER_KEY.spv"
printf '{"stage":"fragment"}\n' > "$shader_cache/$SHADER_KEY.meta.json"
printf 'unreferenced\n' > "$shader_cache/$extra_key.spv"
printf '{}\n' > "$shader_cache/$extra_key.meta.json"
ln -s "$shader_cache/$SHADER_KEY.spv" "$shader_cache/$SYMLINK_KEY.spv"
printf '{}\n' > "$shader_cache/$SYMLINK_KEY.meta.json"
printf 'current-frame-trace-sentinel\n' > "$root/private/var/tmp/garnet-cblog.123.txt"
ln -s "$root/private/var/tmp/garnet-cblog.123.txt" "$root/private/var/tmp/garnet-cblog.124.txt"
mkdir -p "$root/private/tmp/garnet-xlate/88"
printf 'untranslated-fragment\n' > "$root/private/tmp/garnet-xlate/88/$AIR_ONLY_KEY.air"
printf '{"function":"fixture-fragment"}\n' > "$root/private/tmp/garnet-xlate/88/$AIR_ONLY_KEY.fragment.json"
ln -s "$AIR_ONLY_KEY.air" "$root/private/tmp/garnet-xlate/88/$SYMLINK_KEY.air"
printf 'unreferenced-fragment\n' > "$root/private/tmp/garnet-xlate/88/$extra_key.air"
mkdir -p "$root/Library/Logs/GarnetTranslate"
printf 'translator-worker-start\n' > "$root/Library/Logs/GarnetTranslate/translate.stdout"
ln -s translate.stdout "$root/Library/Logs/GarnetTranslate/translate.stderr"
printf 'present-metadata-sentinel\n' > "$root/private/tmp/garnet-xlate/88/present-10-1-100.json"
printf 'present-pixels-sentinel\n' > "$root/private/tmp/garnet-xlate/88/present-10-1-100.raw"
printf 'obsolete-process\n' > "$root/private/tmp/garnet-xlate/88/present-9-1-100.raw"
ln -s "$root/private/tmp/garnet-xlate/88/present-10-1-100.raw" "$root/private/tmp/garnet-xlate/88/present-10-2-200.raw"
export RDNA4_TEST_MODE=1 RDNA4_TEST_ROOT="$root" RDNA4_TEST_TIMEOUT=1
export CALLS ESCAPED SCENARIO
for SCENARIO in recent-boot old-boot boot-failure no-service native-probe missing-probe; do
    if [ "$SCENARIO" = native-probe ]; then
        mkdir -p "$root/Library/Application Support/Garnet/native"
        ln -s "$root/stub" "$root/Library/Application Support/Garnet/native/garnettest"
    fi
    if [ "$SCENARIO" = missing-probe ]; then
        rm "$root/Library/Application Support/RDNA4/garnettest"
        rm "$root/Library/Application Support/Garnet/native/garnettest"
    fi
    CALLS="$evidence/$SCENARIO.calls"
    ESCAPED="$evidence/$SCENARIO.escaped"
    started=$SECONDS
    bash "$here/collect-installed.sh" "$evidence/output with spaces" \
        < /dev/null > "$evidence/$SCENARIO.stdout" 2> "$evidence/$SCENARIO.stderr"
    elapsed=$((SECONDS - started))
    [ "$elapsed" -lt 10 ]
    archive=$(cat "$evidence/$SCENARIO.stdout")
    [ -s "$archive" ]
    tar -tzf "$archive" > "$evidence/$SCENARIO.tar-list"
    extracted="$evidence/$SCENARIO-extracted"
    mkdir "$extracted"
    tar -xzf "$archive" -C "$extracted"
    bundle=${archive%.tar.gz}
    saved="$extracted/${bundle##*/}"
    [ -s "$saved/status.tsv" ]
    grep -qx "$(printf 'kmutil\t7')" "$saved/status.tsv"
    grep -qx "$(printf 'displays\t124')" "$saved/status.tsv"
    grep -q 'kmutil unavailable' "$saved/kmutil.stderr"
    grep -q 'partial display information' "$saved/displays.txt"
    grep -q 'timeout after 1s' "$saved/displays.stderr"
    grep -q 'diskutil diagnostic' "$saved/diskutil.txt"
    for class in GarnetDriver AMDGarnetPrimaryFramebuffer IONDRVFramebuffer; do
        grep -Fq "ioreg <-l> <-w0> <-r> <-c> <$class>" "$CALLS"
        grep -qx "$(printf 'ioreg-%s\t0' "$class")" "$saved/status.tsv"
        [ -s "$saved/ioreg-$class.txt" ]
    done
    ! grep -Fq '<RDNA4FB>' "$CALLS"
    ! grep -Fq '<AMDRadeonRDNA4>' "$CALLS"
    if [ "$SCENARIO" = no-service ]; then
        ! grep -q '^sample ' "$CALLS"
    else
        grep -q 'WindowServer' "$saved/processes.txt"
        [ "$(wc -l < "$saved/processes.txt" | tr -d ' ')" = 80 ]
        [ "$(grep -c '^sample ' "$CALLS")" = 2 ]
        for pid in 10 20; do
            case "$SCENARIO" in
                native-probe) sample_rc=7; grep -q 'sample permission denied' "$saved/windowserver-sample-$pid.stderr";;
                old-boot) sample_rc=124; [ -f "$saved/windowserver-sample-$pid.timeout" ];;
                *) sample_rc=0; grep -q 'WindowServer stack sentinel' "$saved/windowserver-sample-$pid.txt";;
            esac
            grep -qx "$(printf 'windowserver-sample-%s\t%s' "$pid" "$sample_rc")" "$saved/status.tsv"
        done
        tar -tf "$saved/present-top-10-88.tar" > "$evidence/$SCENARIO.present-list"
        [ "$(wc -l < "$evidence/$SCENARIO.present-list" | tr -d ' ')" = 2 ]
        tar -xOf "$saved/present-top-10-88.tar" present-10-1-100.raw | grep -qx 'present-pixels-sentinel'
        grep -qx "$(printf 'present-top-10-88\t0')" "$saved/status.tsv"
        ! grep -q 'present-9\|present-10-2' "$evidence/$SCENARIO.present-list"
    fi
    ! grep -q 'unrelated-process' "$saved/processes.txt"
    grep -q 'CURRENT-BOOT-UUID' "$saved/boot-session-uuid.txt"
    grep -qx 'current-frame-trace-sentinel' "$saved/garnet-cblog.123.txt"
    grep -qx "$(printf 'garnet-cblog.123\t0')" "$saved/status.tsv"
    [ ! -e "$saved/garnet-cblog.124.txt" ]
    case "$SCENARIO" in
        missing-probe)
            ! grep -q '^garnettest ' "$CALLS"
            grep -qx "$(printf 'driver-log\t127')" "$saved/status.tsv";;
        no-service)
            for verb in info log counters; do
                grep -qx "$(printf 'driver-%s\t1' "$verb")" "$saved/status.tsv"
                grep -q 'GarnetDriver not found' "$saved/driver-$verb.stderr"
            done
            grep -qx "$(printf 'display-pipe\t1')" "$saved/status.tsv"
            grep -qx "$(printf 'native-sessions\t1')" "$saved/status.tsv";;
        *)
            for verb in info log counters; do
                grep -qx "$(printf 'driver-%s\t0' "$verb")" "$saved/status.tsv"
                grep -q "driver-ring-$verb-sentinel" "$saved/driver-$verb.txt"
            done
            grep -qx "$(printf 'display-pipe\t0')" "$saved/status.tsv"
            grep -qx "$(printf 'native-sessions\t0')" "$saved/status.tsv"
            for slot in 0 1 2 3 4 5 6 7; do
                grep -qx "$(printf 'native-session-%s\t0' "$slot")" "$saved/status.tsv"
            done;;
    esac
    case "$SCENARIO" in
        recent-boot)
            grep -q '<--start> <epoch-1900>' "$CALLS"
            grep -Fq 'eventMessage CONTAINS "GarnetMetal:"' "$CALLS"
            tar -tf "$saved/cold-shaders.tar" > "$evidence/cold-shaders-list"
            printf '%s\n' "$SHADER_KEY.spv" "$SHADER_KEY.meta.json" > "$evidence/expected-cold-shaders"
            cmp "$evidence/expected-cold-shaders" "$evidence/cold-shaders-list"
            tar -xOf "$saved/cold-shaders.tar" "$SHADER_KEY.spv" | grep -qx 'compiled-fragment'
            tar -xOf "$saved/cold-shaders.tar" "$SHADER_KEY.meta.json" | grep -Fxq '{"stage":"fragment"}'
            grep -qx "$(printf 'cold-shaders\t0')" "$saved/status.tsv"
            [ -s "$saved/cold-air-88.tar" ] || { printf 'FAIL collector omitted the untranslated fallback AIR\n' >&2; exit 1; }
            tar -tf "$saved/cold-air-88.tar" > "$evidence/cold-air-list"
            printf '%s\n' "$AIR_ONLY_KEY.air" "$AIR_ONLY_KEY.fragment.json" > "$evidence/expected-cold-air"
            cmp "$evidence/expected-cold-air" "$evidence/cold-air-list"
            tar -xOf "$saved/cold-air-88.tar" "$AIR_ONLY_KEY.air" | grep -qx 'untranslated-fragment'
            grep -qx "$(printf 'cold-air-88\t0')" "$saved/status.tsv"
            grep -qx 'translator-worker-start' "$saved/translate-stdout.txt"
            [ ! -e "$saved/translate-stderr.txt" ];;
        old-boot) grep -q '<--start> <epoch-1100>' "$CALLS";;
        boot-failure)
            ! grep -q '^log ' "$CALLS"
            [ ! -e "$saved/cold-shaders.tar" ]
            [ ! -e "$saved/cold-air-88.tar" ]
            grep -qx "$(printf 'log\t125')" "$saved/status.tsv"
            grep -q 'logs skipped' "$saved/log.stderr";;
    esac
    # Observe restrictive modes with the host stat variant (test also runs on macOS).
    if stat -c %a "$archive" >/dev/null 2>&1; then stat_mode='-c'; else stat_mode='-f'; fi
    if [ "$stat_mode" = -c ]; then fmt=%a; else fmt=%Lp; fi
    [ "$(stat "$stat_mode" "$fmt" "$bundle")" = 700 ]
    [ "$(stat "$stat_mode" "$fmt" "$evidence/output with spaces")" = 700 ]
    [ "$(stat "$stat_mode" "$fmt" "$archive")" = 600 ]
    for file in "$saved"/*; do [ "$(stat "$stat_mode" "$fmt" "$file")" = 600 ]; done
    invocation="RDNA4_TEST_MODE=1 RDNA4_TEST_ROOT=$root RDNA4_TEST_TIMEOUT=1 SCENARIO=$SCENARIO bash $here/collect-installed.sh '$evidence/output with spaces' < /dev/null"
    printf '%s\t%s\tPASS: exit 0 in %ss; archive extracted; kmutil=7; displays=124; native counters; bounded, deduplicated WS samples; log window; modes 700/600\t%s\n' \
        "$SCENARIO" "$invocation" "$elapsed" "$archive" >> "$evidence/criteria.tsv"
    printf '%s-calls\t%s\tPASS: actual diagnostic invocations retained\t%s\n' \
        "$SCENARIO" "$invocation" "$CALLS" >> "$evidence/criteria.tsv"
    printf 'PASS %s (%ss): archive, native counters, WS samples, failures, timeouts, log window and modes checked\n' "$SCENARIO" "$elapsed"
done
# By now the first delayed child would have written if its process group escaped.
sleep 3
for file in "$evidence"/*.escaped; do [ ! -e "$file" ]; done
[ "$(find "$evidence/output with spaces" -name '*.tar.gz' | wc -l | tr -d ' ')" = 6 ]
printf 'isolation\tbash %s/tests/collect-installed-test.sh %s\tPASS: 6 unique archives; no escaped child sentinel\t%s\n' \
    "$here" "$evidence" "$evidence" >> "$evidence/criteria.tsv"
printf 'PASS unique runs and no surviving timed-out children\nEvidence: %s\n' "$evidence"
