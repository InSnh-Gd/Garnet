#!/bin/bash
# Navi 44 SSH harness. Read-only diagnostics; loading explicitly runs the armed ladder.
set -u
set -o pipefail
RDNA4_HOME=${RDNA4_HOME:-$HOME/rdna4}
RDNA4T=${RDNA4T:-$RDNA4_HOME/bin/garnettest}
KEXT=${KEXT:-/Library/Extensions/Garnet.kext}
BUNDLE=org.garnet.driver
REGLIST=${REGLIST:-$RDNA4_HOME/regs-navi44.txt}
PRED='process == "kernel" AND (eventMessage CONTAINS "Garnet" OR eventMessage CONTAINS "RDNA4")'
LOAD_PRED='process == "kernel" AND eventMessage CONTAINS "Garnet"'
RUN='' STAGE='' ARGS='' DIR=''
fail() { printf 'FAIL: %s\n' "$*" >&2; return 1; }
nvargs() {
    local out rc
    out=$(nvram boot-args 2>&1); rc=$?
    if [ "$rc" -ne 0 ]; then
        case "$out" in *'data was not found'*) return 0;; esac
        fail "cannot read NVRAM: $out"; return 1
    fi
    printf '%s\n' "$out" | awk 'index($0,"\t") { sub(/^[^\t]*\t/, ""); print; found=1; exit } END {exit !found}'
}
armed_tokens() {
    # rdna4*/navi48* too: the kext still honours the pre-rename spelling (src/garnet/src/GarnetBootArgs.h)
    printf '%s\n' "$1" | awk '{ for(i=1;i<=NF;i++) if($i ~ /^(garnet|rdna4|navi48)/) print $i }'
}
pending() {
    local key value
    RUN='' STAGE='' ARGS=''
    [ -f "$RDNA4_HOME/pending" ] || return 1
    while IFS='=' read -r key value; do
        case "$key" in run) RUN=$value;; stage) STAGE=$value;; args) ARGS=$value;; esac
    done < "$RDNA4_HOME/pending"
    case "$RUN" in ''|*[!A-Za-z0-9_.-]*|.|..) fail 'invalid pending run'; return 1;; esac
    case "$STAGE" in [0-9]|1[0-7]) ;; *) fail 'invalid pending stage'; return 1;; esac
}
run_dir() {
    if [ -f "$RDNA4_HOME/pending" ]; then pending || return 1
    else RUN="rdna4-adhoc-$(date +%Y%m%d-%H%M%S)"; STAGE=unknown; ARGS=''; fi
    DIR="$RDNA4_HOME/runs/$RUN"
    mkdir -p "$DIR"
}
version() { defaults read "$KEXT/Contents/Info" CFBundleVersion 2>/dev/null; }
uuid() { dwarfdump --uuid "$KEXT/Contents/MacOS/Garnet" 2>/dev/null | awk '{print $2}'; }
instances() { ioreg -rc GarnetDriver -d1 | awk '/GarnetDriver/ {n++} END {print n+0}'; }
loaded() { kmutil showloaded --list-only 2>/dev/null | awk -v b="$BUNDLE" 'index($0,b) {n++} END {print n+0}'; }
bootsec() { sysctl -n kern.boottime | sed -n 's/.*[ {]sec *= *\([0-9][0-9]*\),.*/\1/p'; }
base() {
    local value tokens
    value=$(nvargs) || return 1
    tokens=$(armed_tokens "$value")
    [ -z "$tokens" ] || { fail "refusing armed base: $tokens"; return 1; }
    mkdir -p "$RDNA4_HOME" || return 1
    printf '%s\n' "$value" > "$RDNA4_HOME/bootargs.base" || return 1
    printf 'Saved boot-args: %s\n' "$value"
}
countdown() { local n; for n in 3 2 1; do printf 'Reboot in %s...\n' "$n"; sleep 1; done; }
arm() {
    local stage='' extra='' reboot=no token value new actual tag
    for token in "$@"; do
        if [ "$token" = --reboot ]; then reboot=yes; continue; fi
        if [ -z "$stage" ]; then stage=$token; continue; fi
        case "$token" in ''|*[!a-zA-Z0-9_.=:,-]*|*garnet-arm*|*rdna4-arm*) fail "invalid extra token: $token"; return 1;; esac
        case "$token" in garnet=*|garnet-stage=*|rdna4bringup=*|rdna4-stage=*|navi48*) fail "conflicting extra token: $token"; return 1;; esac
        extra="$extra $token"
    done
    case "$stage" in [0-9]|1[0-7]) ;; *) fail 'stage must be an integer 0..17'; return 1;; esac
    [ -f "$RDNA4_HOME/bootargs.base" ] || { fail 'run base first'; return 1; }
    value=$(cat "$RDNA4_HOME/bootargs.base") || return 1
    [ -z "$(armed_tokens "$value")" ] || { fail 'base contains armed tokens'; return 1; }
    new="${value:+$value }garnet=1"
    [ "$stage" -eq 0 ] || new="$new garnet-stage=$stage"
    new="$new$extra"
    tag=$(printf '%s' "${RUN_TAG:-}" | LC_ALL=C tr -cd 'A-Za-z0-9_-' | cut -c1-24)
    RUN="rdna4-$(date +%Y%m%d-%H%M%S)-s$stage${tag:+-$tag}"
    [ ! -e "$RDNA4_HOME/runs/$RUN" ] || { fail 'run id collision; wait a second'; return 1; }
    {
        printf 'run=%s\nstage=%s\nargs=%s\narmed_at=%s\n' "$RUN" "$stage" "$new" "$(date +%s)"
        printf 'kext_uuid=%s\nkext_version=%s\n' "$(uuid)" "$(version)"
    } > "$RDNA4_HOME/pending" || return 1
    sudo -n nvram "boot-args=$new" || return 1
    actual=$(nvargs) || return 1
    [ "$actual" = "$new" ] || { fail "NVRAM read-back mismatch: $actual"; return 1; }
    printf 'Armed %s: %s\n' "$RUN" "$actual"
    printf 'After reboot, run disarm immediately, then check/load. Stages 1..4 are survey only.\n'
    if [ "$reboot" = yes ]; then countdown; sudo -n shutdown -r now; fi
}
disarm() {
    local value actual
    [ -f "$RDNA4_HOME/bootargs.base" ] || { fail 'run base first'; return 1; }
    value=$(cat "$RDNA4_HOME/bootargs.base") || return 1
    [ -z "$(armed_tokens "$value")" ] || { fail 'base contains armed tokens'; return 1; }
    actual=$(nvargs) || return 1
    if [ "$actual" != "$value" ]; then
        if [ -n "$value" ]; then sudo -n nvram "boot-args=$value" || return 1
        else sudo -n nvram -d boot-args || return 1; fi
    fi
    actual=$(nvargs) || return 1
    if [ "$actual" != "$value" ] || [ -n "$(armed_tokens "$actual")" ]; then
        fail 'disarm read-back mismatch'; return 1
    fi
    printf 'OK: NVRAM disarmed: %s\n' "$actual"
}
check() {
    local kernel boot now nv ni nl ok=0
    kernel=$(sysctl -n kern.bootargs) || return 1
    printf 'kern.bootargs: %s\nkern.bootsessionuuid: %s\n' "$kernel" "$(sysctl -n kern.bootsessionuuid)"
    boot=$(bootsec); now=$(date +%s)
    if [ -n "$boot" ]; then printf 'uptime seconds: %s\n' "$((now - boot))"; else printf 'uptime seconds: unavailable\n'; fi
    if pending; then
        if [ -n "$ARGS" ] && awk -v expected="$ARGS" -v actual="$kernel" 'BEGIN {
            n=split(expected,e," "); m=split(actual,a," ");
            for(i=1;i<=n;i++) { found=0; for(j=1;j<=m;j++) if(e[i]==a[j]) found=1; if(!found) exit 1 }
        }'; then ok=1; fi
    fi
    nv=$(nvargs) || nv=rdna4-read-error
    if [ -n "$(armed_tokens "$nv")" ]; then printf 'WARN: NVRAM is armed or unreadable; run disarm before load.\n'; fi
    ni=$(instances) || { fail 'cannot inspect GarnetDriver instances'; return 1; }
    nl=$(loaded) || { fail 'cannot inspect loaded bundles'; return 1; }
    printf 'GarnetDriver instances: %s; loaded bundle count: %s\n' "$ni" "$nl"
    if [ "${ni:-0}" -gt 0 ]; then printf 'matched-at-boot: yes\n'; else printf 'matched-at-boot: no\n'; fi
    [ "$ok" -eq 1 ] || { fail 'this boot did not take the armed args'; return 1; }
}
load() {
    local first=no ni nl pci rc=0 elapsed=0 logs pattern
    case "${1:-}" in --first) first=yes; shift;; esac
    [ "$#" -eq 0 ] || { fail 'usage: load [--first]'; return 1; }
    check || return 1
    run_dir || return 1
    ni=$(instances) || return 1; nl=$(loaded) || return 1
    pci=$(ioreg -l -w0 -r -c IOPCIDevice) || { fail 'cannot inspect one-ladder markers'; return 1; }
    if grep -qE '"(Garnet|RDNA4),(StageReached|ProvisionedThisBoot)"' <<< "$pci"; then
        printf 'one ladder per boot: reboot\nrc=1\n' | tee "$DIR/load.txt"; return 1
    fi
    {
        if [ "$first" = yes ]; then sudo -n kmutil load -p "$KEXT"; rc=$?
        elif [ "${ni:-0}" -gt 0 ]; then printf 'Existing instance; no unload/reload.\n'
        elif [ "${nl:-0}" -eq 0 ]; then sudo -n kmutil load -b "$BUNDLE"; rc=$?
        else
            sudo -n kmutil unload -b "$BUNDLE" -P; rc=$?
            if [ "$rc" -eq 0 ]; then sudo -n kmutil load -b "$BUNDLE" -P GarnetDriver; rc=$?; fi
        fi
        printf 'rc=%s\n' "$rc"
    } > "$DIR/load.txt" 2>&1
    cat "$DIR/load.txt"
    if [ "$first" = yes ]; then
        printf 'Click Allow in System Settings > Privacy & Security, then disarm and reboot.\n'; return 3
    fi
    [ "$rc" -eq 0 ] || return "$rc"
    pattern='stages: ladder|start: done'
    [ "$STAGE" -gt 5 ] || pattern='start: done|psp-stage:'
    while :; do
        logs=$(sudo -n /usr/bin/log show --last 5m --info --predicate "$LOAD_PRED" --style compact 2>&1); rc=$?
        if [ "$rc" -ne 0 ]; then printf '%s\n' "$logs"; return "$rc"; fi
        grep -qE "$pattern" <<< "$logs" && break
        [ "$elapsed" -lt 60 ] || { printf '%s\n' "$logs" | grep -F Garnet | tail -40; fail 'timeout waiting for load logs'; return 1; }
        sleep 2; elapsed=$((elapsed + 2))
    done
    printf '%s\n' "$logs" | grep -F Garnet | tail -40
}
# Bound privileged diagnostic clients without requiring a timeout utility.
bounded() {
    local pid elapsed=0 rc
    # Explicit stdin redirection prevents bash from giving async commands /dev/null.
    "$@" <&0 & pid=$!
    while kill -0 "$pid" 2>/dev/null; do
        if [ "$elapsed" -ge 20 ]; then
            kill -TERM "$pid" 2>/dev/null || true
            sleep 1
            kill -KILL "$pid" 2>/dev/null || true
            wait "$pid" 2>/dev/null || true
            printf 'timeout after 20 s\n' >&2; return 124
        fi
        sleep 1; elapsed=$((elapsed + 1))
    done
    wait "$pid"; rc=$?
    return "$rc"
}
rdna4() {
    local rc
    bounded sudo -n "$RDNA4T" "$@"; rc=$?
    [ "$rc" -eq 0 ] || printf 'garnettest failed (rc=%s). If sudo denied it, sudoers needs a NOPASSWD line for %s.\n' "$rc" "$RDNA4T" >&2
    return "$rc"
}
step() {
    local rc
    "$@"; rc=$?
    [ "$rc" -eq 0 ] || printf 'rc=%s: %s\n' "$rc" "$*" >> "$DIR/collect-errors.txt"
    return "$rc"
}
# awk filters treat an empty result as success while pipefail preserves read errors.
bundle_meta() { kmutil showloaded --list-only | awk 'tolower($0) ~ /(garnet|rdna4)/'; }
panic_nvram() { nvram -p | awk 'tolower($0) ~ /panic/'; }
props() { awk '/"[A-Za-z0-9]+,[A-Za-z0-9]+" =/' "$1" | sort; }
boot_log() {
    local style=$1 predicate=$2 start seconds
    sudo -n /usr/bin/log show --last boot --info --debug --predicate "$predicate" --style "$style" && return 0
    printf 'log --last boot failed; retrying with boot time\n' >> "$DIR/collect-errors.txt"
    seconds=$(bootsec); [ -n "$seconds" ] || return 1
    start=$(date -r "$seconds" '+%Y-%m-%d %H:%M:%S') || return 1
    sudo -n /usr/bin/log show --start "$start" --info --debug --predicate "$predicate" --style "$style"
}
collect() {
    local final=no label='' token ni verb file cutoff now boot_uuid archive_ok=0
    for token in "$@"; do
        if [ "$token" = --final ]; then final=yes
        elif [ -z "$label" ]; then label=$token
        else fail 'usage: collect [label] [--final]'; return 1; fi
    done
    run_dir || return 1
    : > "$DIR/collect-errors.txt"
    now=$(date +%s)
    {
        printf 'label: %s\n' "$label"
        step date -u || true; step date || true; step sw_vers || true; step uname -a || true
        step sysctl kern.bootargs kern.bootsessionuuid kern.boottime kern.osversion || true
        step uptime || true
        if [ -f "$RDNA4_HOME/pending" ]; then step cat "$RDNA4_HOME/pending" || true; fi
        step nvram boot-args || true; step csrutil status || true
        step version || true; step uuid || true
        step bundle_meta || true
        step system_profiler SPDisplaysDataType || true
        if command -v pmset >/dev/null 2>&1; then step pmset -g therm || true; fi
    } > "$DIR/meta.txt" 2>&1
    step boot_log compact "$PRED" > "$DIR/kernel-log.txt" 2>&1 || true
    step boot_log ndjson "$PRED" > "$DIR/kernel-log.ndjson" 2> "$DIR/kernel-log-ndjson-errors.txt" || true
    boot_uuid=$(step sysctl -n kern.bootsessionuuid) || true
    if [ -n "$boot_uuid" ]; then
        step grep -F "$boot_uuid" "$DIR/kernel-log.ndjson" > "$DIR/kernel-log.thisboot.ndjson" || true
    else printf 'boot UUID unavailable; cannot filter ndjson\n' >> "$DIR/collect-errors.txt"; fi
    step sudo -n /usr/bin/log show --last 20m --info --debug --predicate 'process == "kernel"' --style compact > "$DIR/kernel-all-tail.txt" 2>&1 || true
    step boot_log compact 'process == "kernelmanagerd" OR process == "kmutil" OR process == "syspolicyd"' > "$DIR/kmutil-log.txt" 2>&1 || true
    step ioreg -rc GarnetDriver -l -w0 > "$DIR/ioreg-garnet.txt" 2>&1 || true
    step props "$DIR/ioreg-garnet.txt" > "$DIR/ioreg-props.txt" || true
    step ioreg -rc AMDGarnetPrimaryFramebuffer -l -w0 > "$DIR/ioreg-primaryfb.txt" 2>&1 || true
    step ioreg -rc IONDRVFramebuffer -l -w0 > "$DIR/ioreg-ndrv.txt" 2>&1 || true
    step ioreg -l -w0 -r -c IOPCIDevice > "$DIR/ioreg-pci-all.txt" 2>&1 || true
    # The dollar expressions below belong to awk, not the shell.
    # shellcheck disable=SC2016
    step awk '
        function emit() { if (block ~ /0x75901002/ || (block ~ /"device-id" = <90750000>/ && block ~ /"vendor-id" = <02100000>/)) printf "%s", block }
        /\+-o / { emit(); block="" } { block=block $0 "\n" } END { emit() }
    ' "$DIR/ioreg-pci-all.txt" > "$DIR/ioreg-gpu-pci.txt" || true
    ni=$(step instances) || true
    if [ "${ni:-0}" -gt 0 ] && [ -x "$RDNA4T" ]; then
        for verb in info counters log; do step rdna4 "$verb" > "$DIR/rdna4t-$verb.txt" 2>&1 || true; done
        step rdna4 metrics 3 > "$DIR/rdna4t-metrics.txt" 2>&1 || true
        if [ -f "$REGLIST" ]; then step rdna4 regs < "$REGLIST" > "$DIR/rdna4t-regs.txt" 2>&1 || true; fi
    else printf 'garnettest skipped: no instance or executable missing\n' >> "$DIR/collect-errors.txt"; fi
    step mkdir -p "$DIR/panics" || true
    if [ -f "$RDNA4_HOME/.last-collect" ]; then cutoff=$(step cat "$RDNA4_HOME/.last-collect") || cutoff=$((now - 259200))
    else cutoff=$((now - 259200)); fi
    case "$cutoff" in ''|*[!0-9]*) cutoff=$((now - 259200));; esac
    for file in /Library/Logs/DiagnosticReports/*.panic /Library/Logs/DiagnosticReports/*panic*.ips /Library/Logs/DiagnosticReports/Kernel*.ips; do
        [ -f "$file" ] || continue
        token=$(step stat -f %m "$file") || continue
        if [ "$token" -gt "$cutoff" ]; then step cp -p "$file" "$DIR/panics/" || true; fi
    done
    step panic_nvram > "$DIR/panics/nvram-panic.txt" 2>&1 || true
    if [ -f "$DIR/thermal.csv" ]; then printf 'Thermal watch: see thermal.csv\n' > "$DIR/thermal.txt"; fi
    {
        printf 'Run: %s\nStage target: %s\nLabel: %s\n' "$RUN" "$STAGE" "$label"
        cat "$DIR/ioreg-props.txt" "$DIR/ioreg-gpu-pci.txt" | grep -E '"((Garnet|RDNA4),(StageReached|StageReachedName|StageResult|StageFailed|CPRingTest|ComputeQueueTest|CPEopTest|CPWriteDataTest|SDMACopyTest|ComputeDispatch|IBTest|ComputeLanesCorrect|NativeS1b)|PSP,Stage|SMU,FirmwareVersion|BAR0,PlanReason)"' | sort -u || true
        printf '\nMatching line counts (driver kernel log):\n'
        for token in FAILED REFUS timeout WARNING; do printf '%s: ' "$token"; grep -c "$token" "$DIR/kernel-log.txt" || true; done
        printf '\nDiagnostic lines:\n'
        for token in 'idle clamp' 'SMU online' 'BAR0 plan' 'stages: ladder'; do grep -F "$token" "$DIR/kernel-log.txt" | tail -1 || true; done
    } > "$DIR/SUMMARY.txt"
    step gzip -f "$DIR/ioreg-pci-all.txt" || true
    if step tar -czf "$RDNA4_HOME/runs/$RUN.tar.gz" -C "$RDNA4_HOME/runs" "$RUN"; then
        archive_ok=1
        printf 'Archive: %s\n' "$RDNA4_HOME/runs/$RUN.tar.gz"
        step shasum -a 256 "$RDNA4_HOME/runs/$RUN.tar.gz" || true
    fi
    printf '%s\n' "$now" > "$RDNA4_HOME/.last-collect" || return 1
    if [ "$final" = yes ] && [ "$archive_ok" -eq 1 ]; then rm -f "$RDNA4_HOME/pending" || return 1; fi
    [ "$archive_ok" -eq 1 ]
}
thermal() {
    local minutes=${1:-10} interval=${2:-5} end metrics raw fields edge hot mem fan gfx board hex thm stamp trip next delay
    local hex_re='^0[xX][0-9a-fA-F]{1,8}$'
    [ "$#" -le 2 ] || { fail 'usage: thermal [minutes=10] [interval=5]'; return 1; }
    case "$minutes:$interval" in *[!0-9:]*|:*|*:) fail 'minutes and interval must be positive integers'; return 1;; esac
    minutes=$((10#$minutes)); interval=$((10#$interval))
    if [ "$minutes" -le 0 ] || [ "$interval" -le 0 ]; then
        fail 'minutes and interval must be positive'; return 1
    fi
    [ -x "$RDNA4T" ] || { fail "not executable: $RDNA4T"; return 1; }
    run_dir || return 1
    printf 'thm_raw_c is an unverified (未验证) decode; metrics layout may also be UNVERIFIED.\n'
    if [ ! -s "$DIR/thermal.csv" ]; then printf 'timestamp,edge_c,hotspot_c,mem_c,fan_rpm,gfx_mhz,board_w,thm_raw_c\n' > "$DIR/thermal.csv" || return 1; fi
    end=$(($(date +%s) + minutes * 60))
    while [ "$(date +%s)" -lt "$end" ]; do
        next=$(($(date +%s) + interval))
        metrics=$(rdna4 metrics 1) || metrics=''
        fields=$(printf '%s\n' "$metrics" | awk '
            /temps +edge/ {for(i=1;i<=NF;i++){if($i=="edge")e=$(i+1);if($i=="hotspot")h=$(i+1);if($i=="mem")m=$(i+1);if($i=="fan")f=$(i+1)}}
            /clocks +gfx/ {for(i=1;i<=NF;i++)if($i=="gfx")g=$(i+1)}
            /power +socket/ {for(i=1;i<=NF;i++)if($i=="board")b=$(i+1)}
            END {printf "%s %s %s %s %s %s",e==""?"NA":e,h==""?"NA":h,m==""?"NA":m,f==""?"NA":f,g==""?"NA":g,b==""?"NA":b}')
        read -r edge hot mem fan gfx board <<< "$fields"
        raw=$(rdna4 reg 0x16600) || raw=''
        hex=$(printf '%s\n' "$raw" | awk '{for(i=1;i<=NF;i++)if($i ~ /^0[xX][0-9a-fA-F]+$/)v=$i} END{print v}')
        thm=NA
        if [[ "$hex" =~ $hex_re ]] && [ "$((hex))" -ne 4294967295 ]; then
            thm=$(awk -v v="$(((hex >> 21) & 2047))" 'BEGIN {printf "%.3f",v/8}')
        fi
        stamp=$(date '+%Y-%m-%dT%H:%M:%S%z')
        printf '%s,%s,%s,%s,%s,%s,%s,%s\n' "$stamp" "$edge" "$hot" "$mem" "$fan" "$gfx" "$board" "$thm" | tee -a "$DIR/thermal.csv"
        if [ "$fields" = '0 0 0 0 0 0' ]; then printf 'Metrics all zeros (pre-features); relying on thm_raw_c.\n'; fi
        if [ "$thm" = NA ] && ! awk -v e="$edge" -v h="$hot" -v m="$mem" 'BEGIN{exit !(e+0>0 || h+0>0 || m+0>0)}'; then
            printf 'NO TELEMETRY — rely on the person at the machine\n'
        fi
        trip=$(awk -v e="$edge" -v h="$hot" -v f="$fan" -v t="$thm" 'BEGIN{print ((h!="NA"&&h+0>95)||(f=="0"&&(e+0>60||h+0>60))||(t!="NA"&&t+0>95))?1:0}')
        if [ "$trip" -eq 1 ]; then
            printf 'THERMAL GUARD TRIP\nFAIL: thermal watch\n'
            if [ "${THERMAL_ACTION:-exit}" = shutdown ]; then sudo -n shutdown -h now; fi
            return 2
        fi
        delay=$((next - $(date +%s)))
        [ "$next" -le "$end" ] || delay=$((end - $(date +%s)))
        if [ "$delay" -gt 0 ]; then sleep "$delay"; fi
    done
    printf 'PASS: thermal watch completed (unavailable/unverified telemetry is not validated).\n'
}
reboot_cmd() {
    local value
    case "$#:${1:-}" in 0:|1:--armed) ;; *) fail 'usage: reboot [--armed]'; return 1;; esac
    value=$(nvargs) || return 1
    if [ "${1:-}" != --armed ] && [ -n "$(armed_tokens "$value")" ]; then fail 'NVRAM still armed; disarm or use --armed'; return 1; fi
    sudo -n shutdown -r now
}
help() {
    printf '%s\n' 'Usage: rdna4.sh base | arm <0..17> [tokens...] [--reboot] | disarm | check' \
        '       rdna4.sh load [--first] | collect [label] [--final] | thermal [minutes=10] [interval=5]' \
        '       rdna4.sh reboot [--armed] | help' \
        'Workflow: base; arm; reboot --armed; disarm immediately after login; check; load; collect.' \
        'Stages 0..4: survey only; 5: PSP SOS; 6..17: one ladder per boot. Never reload a used ladder.'
}
main() {
    local cmd=${1:-help}; [ "$#" -eq 0 ] || shift
    case "$cmd" in
        base|disarm|check|help) [ "$#" -eq 0 ] || { fail "unexpected arguments to $cmd"; return 1; }; "$cmd";;
        arm) arm "$@";; load) load "$@";; collect) collect "$@";; thermal) thermal "$@";; reboot) reboot_cmd "$@";;
        *) help >&2; fail "unknown subcommand: $cmd";;
    esac
}
if [ "${BASH_SOURCE[0]}" = "$0" ]; then main "$@"; exit $?; fi
