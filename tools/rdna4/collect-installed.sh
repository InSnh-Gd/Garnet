#!/bin/bash
# Read-only installed-system capture; works before login with macOS Bash 3.2.
set -u
set -o pipefail
umask 077
export PATH=/usr/bin:/bin:/usr/sbin:/sbin LC_ALL=C
tool_root=''
limit_override=''
# Explicit test-only prefix, never a PATH search for privileged commands.
if [ "${RDNA4_TEST_MODE:-}" = 1 ]; then
    tool_root=${RDNA4_TEST_ROOT:?absolute stub root required}
    case "$tool_root" in /*) ;; *) exit 2;; esac
    limit_override=${RDNA4_TEST_TIMEOUT:-}
    case "$limit_override" in ''|*[!0-9]*|0) exit 2;; esac
fi
mac() { local path=$1; shift; "$tool_root$path" "$@"; }
[ "$#" -le 1 ] || { printf 'Usage: %s [output-directory]\n' "$0" >&2; exit 2; }
base=${1:-/Library/Logs/Garnet}
case "$base" in /*) ;; *) base="$PWD/$base";; esac
mac /bin/mkdir -p "$base" && mac /bin/chmod 700 "$base" || exit 1
bundle=$(mac /usr/bin/mktemp -d "$base/Garnet.XXXXXXXX") || exit 1
status="$bundle/status.tsv"
printf 'capture\texit_status\n' > "$status"
active='' timer=''
# Job control gives each diagnostic its own group, including any children.
set -m
cleanup() {
    [ -z "$active" ] || kill -KILL -- "-$active" 2>/dev/null || :
    [ -z "$timer" ] || kill -KILL -- "-$timer" 2>/dev/null || :
}
trap 'cleanup; exit 130' INT
trap 'cleanup; exit 143' TERM HUP
trap cleanup EXIT
capture() {
    local stem=$1 seconds=$2 path=$3 rc
    shift 3
    seconds=${limit_override:-$seconds}
    printf '%q ' "$path" "$@" > "$stem.command"
    printf '\n' >> "$stem.command"
    "$tool_root$path" "$@" > "$stem.txt" 2> "$stem.stderr" &
    active=$!
    (
        mac /bin/sleep "$seconds"
        if kill -0 "$active" 2>/dev/null; then
            printf 'timeout after %ss\n' "$seconds" > "$stem.timeout"
            kill -KILL -- "-$active" 2>/dev/null || :
        fi
    ) &
    timer=$!
    wait "$active" 2>/dev/null; rc=$?
    # Kill the timer group too, so its sleeping child cannot outlive this run.
    kill -KILL -- "-$timer" 2>/dev/null || :
    wait "$timer" 2>/dev/null || :
    active='' timer=''
    if [ -f "$stem.timeout" ]; then
        mac /bin/cat "$stem.timeout" >> "$stem.stderr"
        rc=124
    fi
    printf '%s\t%s\n' "${stem##*/}" "$rc" >> "$status"
    return "$rc"
}
cold_shader_keys() {
    mac /usr/bin/awk '
        /WindowServer\[.*GarnetMetal: FALLBACK .*spvcache miss/ {
            if (!match($0, /\/spvcache\/[0-9a-f]+\.spv\)/)) next
            key = substr($0, RSTART + 10, RLENGTH - 15)
            if (length(key) == 64 && !seen[key]++) {
                print key
                if (++count == 16) exit
            }
        }
    ' "$bundle/log.txt"
}
cold_shader_file() {
    [ -f "$1" ] && [ ! -L "$1" ] &&
        [ -n "$(mac /usr/bin/find "$1" -type f -links 1 -size +0c -size "-$2c" -print)" ]
}
capture_cold_shaders() {
    local keys=$1 cache="$tool_root/private/var/tmp/garnet-spv" key
    [ -d "$cache" ] && [ ! -L "$cache" ] || cache="$tool_root/private/var/tmp/rdna4m-spv"
    local files=()
    [ -d "$cache" ] && [ ! -L "$cache" ] || return 0
    while IFS= read -r key; do
        cold_shader_file "$cache/$key.spv" 4194305 || continue
        cold_shader_file "$cache/$key.meta.json" 262145 || continue
        files[${#files[@]}]="$key.spv"
        files[${#files[@]}]="$key.meta.json"
    done <<< "$keys"
    [ "${#files[@]}" -gt 0 ] || return 0
    capture "$bundle/cold-shaders" 15 /usr/bin/tar -cf "$bundle/cold-shaders.tar" -C "$cache" "${files[@]}"
}
capture_cold_air() {
    local keys=$1 uid=$2 dump="$tool_root/private/tmp/garnet-xlate" key sidecar
    [ ! -L "$dump" ] && [ -d "$dump/$uid" ] && [ ! -L "$dump/$uid" ] || dump="$tool_root/private/tmp/rdna4m"
    local files=()
    [ ! -L "$dump" ] && [ -d "$dump/$uid" ] && [ ! -L "$dump/$uid" ] || return 0
    while IFS= read -r key; do
        cold_shader_file "$dump/$uid/$key.air" 4194305 || continue
        files[${#files[@]}]="$key.air"
        for sidecar in vertex.json fragment.json kernel.json txt; do
            cold_shader_file "$dump/$uid/$key.$sidecar" 65537 || continue
            files[${#files[@]}]="$key.$sidecar"
        done
    done <<< "$keys"
    [ "${#files[@]}" -gt 0 ] || return 0
    capture "$bundle/cold-air-$uid" 15 /usr/bin/tar -cf "$bundle/cold-air-$uid.tar" -C "$dump/$uid" "${files[@]}"
}
capture "$bundle/time" 5 /bin/date '+%Y-%m-%dT%H:%M:%S%z' || :
capture "$bundle/sw_vers" 10 /usr/bin/sw_vers || :
capture "$bundle/uname" 5 /usr/bin/uname -a || :
capture "$bundle/bootargs" 5 /usr/sbin/sysctl -n kern.bootargs || :
capture "$bundle/osversion" 5 /usr/sbin/sysctl -n kern.osversion || :
capture "$bundle/boot-session-uuid" 5 /usr/sbin/sysctl -n kern.bootsessionuuid || :
boot_ok=0
capture "$bundle/boottime" 5 /usr/sbin/sysctl -n kern.boottime && boot_ok=1
probe='/Library/Application Support/Garnet/native/garnettest'
if [ ! -x "$tool_root$probe" ]; then
    probe='/Library/Application Support/RDNA4/native/garnettest'
fi
if [ ! -x "$tool_root$probe" ]; then
    probe='/Library/Application Support/RDNA4/garnettest'
fi
if [ -x "$tool_root$probe" ]; then
    capture "$bundle/driver-info" 10 "$probe" info || :
    capture "$bundle/driver-log" 20 "$probe" log || :
    capture "$bundle/driver-counters" 10 "$probe" counters || :
    # These read-only counters survive a stalled desktop without submitting GPU work.
    capture "$bundle/display-pipe" 5 "$probe" accel pipestat || :
    capture "$bundle/native-sessions" 5 "$probe" accel sessstat 8 || :
    for slot in 0 1 2 3 4 5 6 7; do
        capture "$bundle/native-session-$slot" 5 "$probe" accel sessstat "$slot" || :
    done
else
    printf 'Packaged garnettest missing; driver log ring was not captured.\n' > "$bundle/driver-log.stderr"
    printf 'driver-log\t127\n' >> "$status"
fi
capture "$bundle/ioreg-GarnetDriver" 20 /usr/sbin/ioreg -l -w0 -r -c GarnetDriver || :
capture "$bundle/ioreg-AMDGarnetPrimaryFramebuffer" 20 /usr/sbin/ioreg -l -w0 -r -c AMDGarnetPrimaryFramebuffer || :
capture "$bundle/ioreg-IONDRVFramebuffer" 20 /usr/sbin/ioreg -l -w0 -r -c IONDRVFramebuffer || :
capture "$bundle/ioreg-IOPCIDevice" 20 /usr/sbin/ioreg -l -w0 -r -c IOPCIDevice || :
capture "$bundle/kmutil" 30 /usr/bin/kmutil showloaded || :
capture "$bundle/processes" 10 /bin/bash -o pipefail -c '
    "$1" -axo pid,ppid,uid,stat,etime,comm |
    "$2" '\''NR == 1 || /WindowServer|loginwindow|SetupAssistant/ {if (++n <= 80) print}'\''
' _ "$tool_root/bin/ps" "$tool_root/usr/bin/awk" || :
# A freeze leaves no crash report; sample the existing process before slower diagnostics.
for pid in $(mac /usr/bin/awk '
    $1 ~ /^[0-9]+$/ && $NF ~ /\/WindowServer$/ && !seen[$1]++ && ++n <= 4 {print $1}
' "$bundle/processes.txt"); do
    capture "$bundle/windowserver-sample-$pid" 10 /usr/bin/sample "$pid" 1 10 -file /dev/stdout || :
    for uid in 88 0; do
        pixels="$tool_root/private/tmp/garnet-xlate/$uid"
        [ -d "$pixels" ] && [ ! -L "$pixels" ] || pixels="$tool_root/private/tmp/rdna4m/$uid"
        [ -d "$pixels" ] && [ ! -L "$pixels" ] || continue
        capture "$bundle/present-top-$pid-$uid" 20 /bin/bash -c '
            directory=$1 pid=$2 tar=$3 output=$4
            files=()
            for file in "$directory"/present-"$pid"-*.json "$directory"/present-"$pid"-*.raw; do
                [ -f "$file" ] && [ ! -L "$file" ] || continue
                [ "${#files[@]}" -lt 32 ] || break
                files[${#files[@]}]=${file##*/}
            done
            [ "${#files[@]}" -gt 0 ] || exit 0
            "$tar" -cf "$output" -C "$directory" "${files[@]}"
        ' _ "$pixels" "$pid" "$tool_root/usr/bin/tar" "$bundle/present-top-$pid-$uid.tar" || :
    done
done
capture "$bundle/diskutil" 30 /usr/sbin/diskutil list || :
GARNET_ALLOW=1 capture "$bundle/displays" 30 /usr/sbin/system_profiler SPDisplaysDataType -timeout 20 || :
for trace in "$tool_root"/private/var/tmp/garnet-cblog.[0-9]*.txt "$tool_root"/private/var/tmp/rdna4m-cblog.[0-9]*.txt; do
    [ -f "$trace" ] && [ ! -L "$trace" ] || continue
    trace_name=${trace##*/}
    capture "$bundle/${trace_name%.txt}" 10 /usr/bin/tail -c 52428800 "$trace" || :
done
translator_logs="$tool_root/Library/Logs/GarnetTranslate"
[ -d "$translator_logs" ] && [ ! -L "$translator_logs" ] || translator_logs="$tool_root/Library/Logs/RDNA4Translate"
if [ -d "$translator_logs" ] && [ ! -L "$translator_logs" ]; then
    for stream in stdout stderr; do
        trace="$translator_logs/translate.$stream"
        [ -f "$trace" ] && [ ! -L "$trace" ] || continue
        capture "$bundle/translate-$stream" 5 /usr/bin/tail -c 262144 "$trace" || :
    done
fi

boot_text=$(< "$bundle/boottime.txt")
boot_pattern='sec[[:space:]]*=[[:space:]]*([0-9]+),'
now=$(mac /bin/date +%s) || now=''
if [ "$boot_ok" -eq 1 ] && [[ "$now" =~ ^[0-9]+$ ]] && [[ "$boot_text" =~ $boot_pattern ]]; then
    boot=${BASH_REMATCH[1]}
    start=$((now - 900))
    [ "$start" -ge "$boot" ] || start=$boot
    since=$(mac /bin/date -r "$start" '+%Y-%m-%d %H:%M:%S')
    until=$(mac /bin/date -r "$now" '+%Y-%m-%d %H:%M:%S')
    printf 'boot_epoch=%s\nstart=%s\nend=%s\n' "$boot" "$since" "$until" > "$bundle/log-window.txt"
    if [ -n "$since" ] && [ -n "$until" ]; then
        capture "$bundle/log" 90 /usr/bin/log show --start "$since" --end "$until" \
            --style compact --info --debug --predicate \
            'process == "WindowServer" OR process == "kextd" OR process == "kernelmanagerd" OR eventMessage CONTAINS "GarnetMetal:" OR eventMessage CONTAINS "RDNA4Metal:" OR (process == "kernel" AND (eventMessage CONTAINS[c] "Garnet" OR eventMessage CONTAINS[c] "RDNA4" OR eventMessage CONTAINS[c] "AMD" OR eventMessage CONTAINS[c] "kext" OR eventMessage CONTAINS[c] "GPU" OR eventMessage CONTAINS[c] "display" OR eventMessage CONTAINS[c] "IOGraphics" OR eventMessage CONTAINS[c] "IOAccelerator"))' || :
        keys=$(cold_shader_keys)
        if [ -n "$keys" ]; then
            capture_cold_shaders "$keys" || :
            capture_cold_air "$keys" 88 || :
            capture_cold_air "$keys" 0 || :
        fi
    else
        printf 'Cannot format current boot log window; logs skipped.\n' > "$bundle/log.stderr"
        printf 'log\t125\n' >> "$status"
    fi
else
    printf 'Cannot establish current boot time; logs skipped.\n' > "$bundle/log.stderr"
    printf 'log\t125\n' >> "$status"
fi
# Keep archive diagnostics outside the directory being archived.
if capture "$bundle-archive" 60 /usr/bin/tar -czf "$bundle.tar.gz" -C "$base" "${bundle##*/}"; then
    printf '%s\n' "$bundle.tar.gz"
else
    printf 'Archive failed; raw bundle: %s (see %s-archive.stderr)\n' "$bundle" "$bundle" >&2
    exit 1
fi
