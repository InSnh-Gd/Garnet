#!/bin/bash
# Dual-mode (4K@60Hz + 4K@160Hz) physical RX 9060 XT performance, latency, and
# SMU power baseline orchestrator. Runs from the Linux dev host over SSH into
# an isolated ~/rdna4-build/perf-bench directory on the macOS target; never
# modifies /Users/Shared/RDNA4HardwareTest, never loads kexts, and always
# restores the target display's original refresh rate on exit.
#
# Usage:
#   bash tools/rdna4/bench-perf.sh <label> [--modes "60 160"] [--idle-secs 15]
set -euo pipefail
export LC_ALL=C

usage() {
    cat <<'EOF'
Usage: bench-perf.sh <label> [--modes "60 160"] [--idle-secs 15] [--host user@ip] [--key path]
  <label>           Run label (e.g. stage1-baseline, stage1-post, stage2-post)
  --modes           Space-separated refresh rates to sample (default: "60 160")
  --idle-secs       Idle sampling duration per mode in seconds (default: 15)
  --host            SSH target (default: ${RDNA4_TEST_HOST:-insnh-gd@172.29.3.138})
  --key             SSH key path (default: ${RDNA4_SSH_KEY:-$HOME/.ssh/id_ed25519})
EOF
}

if [ "${1:-}" = "-h" ] || [ "${1:-}" = "--help" ] || [ "$#" -lt 1 ]; then
    usage
    exit 0
fi

LABEL=$1
shift
MODES="60 160"
IDLE_SECS=15
HOST="${RDNA4_TEST_HOST:-insnh-gd@172.29.3.138}"
SSH_KEY="${RDNA4_SSH_KEY:-$HOME/.ssh/id_ed25519}"

while [ "$#" -gt 0 ]; do
    case "$1" in
        --modes) MODES=$2; shift 2 ;;
        --idle-secs) IDLE_SECS=$2; shift 2 ;;
        --host) HOST=$2; shift 2 ;;
        --key) SSH_KEY=$2; shift 2 ;;
        *) echo "bench-perf.sh: unknown arg: $1" >&2; usage >&2; exit 2 ;;
    esac
done

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/../.." && pwd)
BENCH_SRC="$REPO_ROOT/tools/native/garnet-perf-bench/garnet_perf_bench.m"
[ -f "$BENCH_SRC" ] || { echo "bench-perf.sh: missing $BENCH_SRC" >&2; exit 1; }

TS=$(date -u +%Y%m%dT%H%M%SZ)
OUT_DIR="$REPO_ROOT/logs/rdna4-perf-baseline/${LABEL}-${TS}"
mkdir -p "$OUT_DIR"

SSH_OPTS=(-i "$SSH_KEY" -o BatchMode=yes -o ConnectTimeout=10)
REMOTE_DIR='$HOME/rdna4-build/perf-bench'

echo "bench-perf: staging and compiling isolated microbench on $HOST..."
ssh "${SSH_OPTS[@]}" "$HOST" "mkdir -p $REMOTE_DIR/tmp"
scp -q -i "$SSH_KEY" "$BENCH_SRC" "$HOST:~/rdna4-build/perf-bench/garnet_perf_bench.m"

ssh "${SSH_OPTS[@]}" "$HOST" '
set -euo pipefail
export TMPDIR="$HOME/rdna4-build/perf-bench/tmp"
cd "$HOME/rdna4-build/perf-bench"
clang -arch x86_64 -fobjc-arc -O2 -Wall -Wextra -Werror \
    -framework Foundation -framework CoreGraphics -framework Metal \
    -o garnet_perf_bench garnet_perf_bench.m
'

echo "bench-perf: collecting host metadata and dual-mode samples ($MODES Hz, ${IDLE_SECS}s idle each)..."
ssh "${SSH_OPTS[@]}" "$HOST" "MODES='$MODES' IDLE_SECS='$IDLE_SECS' bash -s" <<'REMOTE_EOF' > "$OUT_DIR/raw.txt"
set -euo pipefail
export TMPDIR="$HOME/rdna4-build/perf-bench/tmp"
BENCH="$HOME/rdna4-build/perf-bench/garnet_perf_bench"

GARNETTEST=""
for p in "/Library/Application Support/Garnet/native/garnettest" "/Library/Application Support/RDNA4/native/garnettest"; do
    if [ -x "$p" ]; then GARNETTEST="$p"; break; fi
done

ORIG_INFO=$("$BENCH" --display-info)
ORIG_HZ=$(printf '%s\n' "$ORIG_INFO" | awk -F= '$1=="refresh_hz"{print $2; exit}')
restore_mode() {
    if [ -n "${ORIG_HZ:-}" ]; then
        "$BENCH" --set-refresh "$ORIG_HZ" >/dev/null 2>&1 || true
    fi
}
trap restore_mode EXIT

echo "=== HOST_META ==="
echo "utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
echo "macos_version=$(sw_vers -productVersion)"
echo "macos_build=$(sw_vers -buildVersion)"
echo "boot_args=$(nvram boot-args 2>/dev/null | sed 's/^boot-args[[:space:]]*//' || echo unknown)"
echo "bar0_mb=$(ioreg -r -c GarnetDriver -d 2 2>/dev/null | awk -F'= ' '/"ReBAR,BAR0,CurrentMB"/{gsub(/[^0-9]/,"",$2); print $2; exit}')"
echo "bar0_full=$(ioreg -r -c GarnetDriver -d 2 2>/dev/null | awk -F'= ' '/"BAR0,FullMode"/{print $2; exit}')"
echo "orig_refresh_hz=$ORIG_HZ"
printf '%s\n' "$ORIG_INFO" | sed 's/^/orig_/'
if [ -n "$GARNETTEST" ]; then
    echo "=== VRAMSTAT ==="
    sudo "$GARNETTEST" accel vramstat 2>&1 || true
fi

for hz in $MODES; do
    echo "=== MODE_BEGIN hz=$hz ==="
    if "$BENCH" --set-refresh "$hz" 2>&1; then
        sleep 3
    else
        echo "mode_switch_status=FAILED"
        echo "=== MODE_END hz=$hz ==="
        continue
    fi
    "$BENCH" --display-info

    # Capture pipestat before idle window.
    if [ -n "$GARNETTEST" ]; then
        echo "--- PIPESTAT_BEFORE ---"
        sudo "$GARNETTEST" accel pipestat 2>&1 || true
    fi

    # Sample kernel_task & WindowServer CPU% (single 2s top delta) + SMU metrics across IDLE_SECS.
    echo "--- IDLE_SAMPLES ---"
    samples=$(( IDLE_SECS / 3 ))
    [ "$samples" -lt 3 ] && samples=3
    ws_pid=$(pgrep -x WindowServer | head -n 1 || true)
    i=0
    while [ "$i" -lt "$samples" ]; do
        if [ -n "$ws_pid" ]; then
            top -l 2 -s 2 -stats command,cpu -pid 0 -pid "$ws_pid" 2>/dev/null | awk '
                /^Processes:/ { sample++ }
                sample == 2 && /^kernel_task/ { kt = $NF }
                sample == 2 && /^WindowServer/ { ws = $NF }
                END { printf "sample_cpu kernel_task=%.2f windowserver=%.2f\n", kt+0, ws+0 }
            '
        else
            top -l 2 -s 2 -stats command,cpu -pid 0 2>/dev/null | awk '
                /^Processes:/ { sample++ }
                sample == 2 && /^kernel_task/ { kt = $NF }
                END { printf "sample_cpu kernel_task=%.2f windowserver=0.00\n", kt+0 }
            '
        fi
        if [ -n "$GARNETTEST" ]; then
            sudo "$GARNETTEST" metrics 1 2>&1 | grep -E '^(clocks|temps|power|link)' || true
        fi
        i=$(( i + 1 ))
    done

    if [ -n "$GARNETTEST" ]; then
        echo "--- PIPESTAT_AFTER ---"
        sudo "$GARNETTEST" accel pipestat 2>&1 || true
    fi

    echo "--- METAL_BENCH ---"
    sudo env GARNET_ALLOW=1 RDNA4M_ALLOW=1 GARNET_TEST_INPROC=1 RDNA4M_TEST_INPROC=1 "$BENCH" --run-bench 2>&1 || echo "metal_bench_error=1"

    echo "--- RECENT_SCANOUT_LOG ---"
    log show --last 25s --predicate '(eventMessage CONTAINS "GarnetMetal" OR eventMessage CONTAINS "Garnet:") AND (eventMessage CONTAINS "scanout: counters:" OR eventMessage CONTAINS "F1:")' --style compact 2>/dev/null | tail -n 10 || true
    echo "=== MODE_END hz=$hz ==="
done
REMOTE_EOF

# Parse raw.txt into summary.json and REPORT.md using pure awk (no Python required).
awk -v label="$LABEL" -v ts="$TS" -v out_json="$OUT_DIR/summary.json" -v out_md="$OUT_DIR/REPORT.md" '
function trim(s) { gsub(/^[ \t\r\n]+|[ \t\r\n]+$/, "", s); return s }
function jesc(s) { gsub(/\\/, "\\\\", s); gsub(/"/, "\\\"", s); return s }
/^=== HOST_META ===/ { sec="meta"; next }
/^=== VRAMSTAT ===/ { sec="vram"; next }
/^=== MODE_BEGIN hz=/ {
    sec="mode"; subsec=""
    hz=$0; sub(/^=== MODE_BEGIN hz=/, "", hz); sub(/ ===$/, "", hz)
    nmodes++; modes[nmodes]=hz; cur=hz
    kt_sum[cur]=0; ws_sum[cur]=0; cpu_n[cur]=0
    w_sum[cur]=0; gfx_sum[cur]=0; uclk_sum[cur]=0; fclk_sum[cur]=0; temp_sum[cur]=0; fan_sum[cur]=0; smu_n[cur]=0
    f1_cnt[cur]=0
    next
}
/^=== MODE_END / { sec=""; cur=""; next }
/^--- PIPESTAT_BEFORE ---/ { subsec="pb"; next }
/^--- PIPESTAT_AFTER ---/ { subsec="pa"; next }
/^--- IDLE_SAMPLES ---/ { subsec="idle"; next }
/^--- METAL_BENCH ---/ { subsec="bench"; next }
/^--- RECENT_SCANOUT_LOG ---/ { subsec="log"; next }

sec=="meta" && /=/ {
    k=$0; sub(/=.*/, "", k); v=$0; sub(/^[^=]*=/, "", v)
    meta[k]=trim(v)
    next
}
sec=="mode" && subsec=="" && /=/ {
    k=$0; sub(/=.*/, "", k); v=$0; sub(/^[^=]*=/, "", v)
    mval[cur, k]=trim(v)
    next
}
sec=="mode" && subsec=="pb" && /handled/ {
    # hook calls 267/277/278/279: a / b / c / d; handled e / f / g / h
    if (match($0, /handled [0-9]+ \/ [0-9]+ \/ [0-9]+ \/ ([0-9]+)/, a)) pb_handled[cur]=a[1]+0
    next
}
sec=="mode" && subsec=="pa" && /handled/ {
    if (match($0, /handled [0-9]+ \/ [0-9]+ \/ [0-9]+ \/ ([0-9]+)/, a)) pa_handled[cur]=a[1]+0
    next
}
sec=="mode" && subsec=="idle" {
    if ($1 == "sample_cpu") {
        for (i=2; i<=NF; i++) {
            if ($i ~ /^kernel_task=/) { v=$i; sub(/^kernel_task=/, "", v); kt_sum[cur]+=v+0 }
            if ($i ~ /^windowserver=/) { v=$i; sub(/^windowserver=/, "", v); ws_sum[cur]+=v+0 }
        }
        cpu_n[cur]++
    } else if ($1 == "clocks") {
        # clocks gfx 1000 MHz soc 1371 MHz mem 1258 MHz fclk 2016 MHz
        for (i=1; i<=NF; i++) {
            if ($i=="gfx") gfx_sum[cur]+=$(i+1)+0
            if ($i=="mem") uclk_sum[cur]+=$(i+1)+0
            if ($i=="fclk") fclk_sum[cur]+=$(i+1)+0
        }
        smu_n[cur]++
    } else if ($1 == "temps") {
        for (i=1; i<=NF; i++) {
            if ($i=="edge") temp_sum[cur]+=$(i+1)+0
            if ($i=="fan") fan_sum[cur]+=$(i+1)+0
        }
    } else if ($1 == "power") {
        for (i=1; i<=NF; i++) {
            if ($i=="socket") w_sum[cur]+=$(i+1)+0
        }
    }
    next
}
sec=="mode" && subsec=="bench" && /=/ {
    k=$0; sub(/=.*/, "", k); v=$0; sub(/^[^=]*=/, "", v)
    bval[cur, k]=trim(v)
    next
}
sec=="mode" && subsec=="log" {
    if ($0 !~ /\[com\.apple\.log:/ && $0 ~ /F1:/) f1_cnt[cur]++
    next
}
END {
    # Write summary.json
    print "{" > out_json
    printf "  \"label\": \"%s\",\n", jesc(label) >> out_json
    printf "  \"timestamp_utc\": \"%s\",\n", jesc(meta["utc"] ? meta["utc"] : ts) >> out_json
    printf "  \"macos\": \"%s (%s)\",\n", jesc(meta["macos_version"]), jesc(meta["macos_build"]) >> out_json
    printf "  \"bar0_mb\": %d,\n", meta["bar0_mb"]+0 >> out_json
    printf "  \"bar0_full\": \"%s\",\n", jesc(meta["bar0_full"]) >> out_json
    printf "  \"boot_args\": \"%s\",\n", jesc(meta["boot_args"]) >> out_json
    print "  \"modes\": {" >> out_json
    for (m=1; m<=nmodes; m++) {
        hz=modes[m]
        kt = cpu_n[hz] ? kt_sum[hz]/cpu_n[hz] : 0
        ws = cpu_n[hz] ? ws_sum[hz]/cpu_n[hz] : 0
        pw = smu_n[hz] ? w_sum[hz]/smu_n[hz] : 0
        gx = smu_n[hz] ? gfx_sum[hz]/smu_n[hz] : 0
        uc = smu_n[hz] ? uclk_sum[hz]/smu_n[hz] : 0
        fc = smu_n[hz] ? fclk_sum[hz]/smu_n[hz] : 0
        tp = smu_n[hz] ? temp_sum[hz]/smu_n[hz] : 0
        fn = smu_n[hz] ? fan_sum[hz]/smu_n[hz] : 0
        dp = (pa_handled[hz] >= pb_handled[hz]) ? (pa_handled[hz] - pb_handled[hz]) : 0
        comma = (m < nmodes) ? "," : ""
        printf "    \"%sHz\": {\n", hz >> out_json
        printf "      \"refresh_hz\": %.3f,\n", mval[hz,"refresh_hz"]+0 >> out_json
        printf "      \"idle_kernel_task_cpu_pct\": %.2f,\n", kt >> out_json
        printf "      \"idle_windowserver_cpu_pct\": %.2f,\n", ws >> out_json
        printf "      \"idle_pipestat_presents_delta\": %d,\n", dp >> out_json
        printf "      \"idle_smu_socket_w\": %.1f,\n", pw >> out_json
        printf "      \"idle_smu_gfx_mhz\": %.0f,\n", gx >> out_json
        printf "      \"idle_smu_uclk_mhz\": %.0f,\n", uc >> out_json
        printf "      \"idle_smu_fclk_mhz\": %.0f,\n", fc >> out_json
        printf "      \"idle_smu_edge_c\": %.1f,\n", tp >> out_json
        printf "      \"idle_smu_fan_rpm\": %.0f,\n", fn >> out_json
        printf "      \"scanout_f1_lines\": %d,\n", f1_cnt[hz]+0 >> out_json
        printf "      \"metal_device\": \"%s\",\n", jesc(bval[hz,"device_name"]) >> out_json
        printf "      \"submit_p50_us\": %.2f,\n", bval[hz,"submit_p50_us"]+0 >> out_json
        printf "      \"submit_p95_us\": %.2f,\n", bval[hz,"submit_p95_us"]+0 >> out_json
        printf "      \"submit_p99_us\": %.2f,\n", bval[hz,"submit_p99_us"]+0 >> out_json
        printf "      \"submit_cpu_ms\": %.2f,\n", bval[hz,"submit_cpu_ms"]+0 >> out_json
        printf "      \"pipe_cold_ms\": %.2f,\n", bval[hz,"pipe_cold_ms"]+0 >> out_json
        printf "      \"pipe_warm_ms\": %.2f,\n", bval[hz,"pipe_warm_ms"]+0 >> out_json
        printf "      \"draw_10k_encode_ms\": %.2f,\n", bval[hz,"draw_encode_ms"]+0 >> out_json
        printf "      \"draw_10k_wall_ms\": %.2f,\n", bval[hz,"draw_wall_ms"]+0 >> out_json
        printf "      \"draw_10k_cpu_ms\": %.2f,\n", bval[hz,"draw_cpu_ms"]+0 >> out_json
        printf "      \"compute_wall_ms\": %.2f,\n", bval[hz,"compute_wall_ms"]+0 >> out_json
        printf "      \"compute_gb_s\": %.2f,\n", bval[hz,"compute_gb_s"]+0 >> out_json
        printf "      \"blit_wall_ms\": %.2f,\n", bval[hz,"blit_wall_ms"]+0 >> out_json
        printf "      \"blit_gb_s\": %.2f\n", bval[hz,"blit_gb_s"]+0 >> out_json
        printf "    }%s\n", comma >> out_json
    }
    print "  }" >> out_json
    print "}" >> out_json

    # Write REPORT.md
    printf "# Garnet Performance & Power Baseline — %s\n\n", label > out_md
    printf "- **UTC**: `%s`\n", (meta["utc"] ? meta["utc"] : ts) >> out_md
    printf "- **macOS**: `%s (%s)`\n", meta["macos_version"], meta["macos_build"] >> out_md
    printf "- **BAR0**: `%s MiB` (`FullMode=%s`)\n", meta["bar0_mb"], meta["bar0_full"] >> out_md
    printf "- **boot-args**: `%s`\n\n", meta["boot_args"] >> out_md
    print "| Mode | Idle `kernel_task` % | Idle `WindowServer` % | Idle Socket W | GFX / UCLK / FCLK MHz | Edge °C / Fan RPM | Submit P50 / P95 / P99 (µs) | 10k Draw Encode / Wall (ms) | Compute (GB/s) | Blit (GB/s) | Cold / Warm Pipe (ms) | F1 |" >> out_md
    print "| --- | ---: | ---: | ---: | --- | --- | --- | --- | ---: | ---: | --- | ---: |" >> out_md
    for (m=1; m<=nmodes; m++) {
        hz=modes[m]
        kt = cpu_n[hz] ? kt_sum[hz]/cpu_n[hz] : 0
        ws = cpu_n[hz] ? ws_sum[hz]/cpu_n[hz] : 0
        pw = smu_n[hz] ? w_sum[hz]/smu_n[hz] : 0
        gx = smu_n[hz] ? gfx_sum[hz]/smu_n[hz] : 0
        uc = smu_n[hz] ? uclk_sum[hz]/smu_n[hz] : 0
        fc = smu_n[hz] ? fclk_sum[hz]/smu_n[hz] : 0
        tp = smu_n[hz] ? temp_sum[hz]/smu_n[hz] : 0
        fn = smu_n[hz] ? fan_sum[hz]/smu_n[hz] : 0
        printf "| %sHz (`%.2f`) | %.2f%% | %.2f%% | %.1f W | %.0f / %.0f / %.0f | %.1f °C / %.0f | %.1f / %.1f / %.1f | %.2f / %.2f | %.2f | %.2f | %.2f / %.2f | %d |\n", \
            hz, mval[hz,"refresh_hz"]+0, kt, ws, pw, gx, uc, fc, tp, fn, \
            bval[hz,"submit_p50_us"]+0, bval[hz,"submit_p95_us"]+0, bval[hz,"submit_p99_us"]+0, \
            bval[hz,"draw_encode_ms"]+0, bval[hz,"draw_wall_ms"]+0, \
            bval[hz,"compute_gb_s"]+0, bval[hz,"blit_gb_s"]+0, \
            bval[hz,"pipe_cold_ms"]+0, bval[hz,"pipe_warm_ms"]+0, f1_cnt[hz]+0 >> out_md
    }
}
' "$OUT_DIR/raw.txt"

echo "bench-perf: DONE -> $OUT_DIR"
cat "$OUT_DIR/REPORT.md"
