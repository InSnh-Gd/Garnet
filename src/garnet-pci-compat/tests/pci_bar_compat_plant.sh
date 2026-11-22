#!/bin/bash
# pci_bar_compat_plant.sh — planted-defect controls for pci_bar_compat_pure.h.
# Proves tests/pci_bar_compat_test.cpp catches removed UUID verification, removed
# RDNA4 allowlist gating, broken BAR0->BAR2 stepping, and other safety regressions.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../../.." && pwd -P)
SRC_H="$ROOT/src/garnet-pci-compat/src/pci_bar_compat_pure.h"
TEST_CPP="$ROOT/src/garnet-pci-compat/tests/pci_bar_compat_test.cpp"

PY=${PYTHON3:-$(command -v python3 || true)}
CXX=${HOST_CXX:-$(command -v clang++ || command -v g++ || true)}
[ -n "$PY" ] && [ -x "$PY" ] || { echo "error: python3 not found (set PYTHON3=...)" >&2; exit 2; }
[ -n "$CXX" ] && [ -x "$CXX" ] || { echo "error: C++ compiler not found (set HOST_CXX=...)" >&2; exit 2; }

SCR=$(mktemp -d "${TMPDIR:-/tmp}/pci-compat-plant.XXXXXX")
trap 'rm -rf "$SCR"' EXIT

escaped=0
total=0

run_case() {
    local name=$1 expect_pass=$2
    total=$((total + 1))
    local build_rc=0 run_rc=0
    "$CXX" -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
        -I"$SCR" "$TEST_CPP" -o "$SCR/test_bin" >"$SCR/build.log" 2>&1 || build_rc=$?
    if [ "$build_rc" -ne 0 ]; then
        printf 'PLANT/CONTROL %s: COMPILE FAILED (expected runtime test)\n' "$name" >&2
        cat "$SCR/build.log" >&2
        escaped=$((escaped + 1))
        return 0
    fi
    "$SCR/test_bin" >"$SCR/run.log" 2>&1 || run_rc=$?
    if [ "$expect_pass" -eq 1 ]; then
        if [ "$run_rc" -eq 0 ]; then
            printf 'CONTROL %s: PASS\n' "$name"
        else
            printf 'CONTROL %s: FAIL (unexpected)\n' "$name" >&2
            cat "$SCR/run.log" 2>/dev/null || true
            escaped=$((escaped + 1))
        fi
    else
        if [ "$run_rc" -ne 0 ]; then
            printf 'PLANT %s: CAUGHT at runtime (rc=%d)\n' "$name" "$run_rc"
        else
            printf 'PLANT %s: *** ESCAPED ***\n' "$name" >&2
            escaped=$((escaped + 1))
        fi
    fi
}

plant() {
    local id=$1 desc=$2 old=$3 new=$4
    cp "$SRC_H" "$SCR/pci_bar_compat_pure.h"
    PLANT_OLD="$old" PLANT_NEW="$new" "$PY" - "$SCR/pci_bar_compat_pure.h" <<'PY'
import os, sys
p = sys.argv[1]
txt = open(p, "r", encoding="utf-8").read()
old = os.environ["PLANT_OLD"]
new = os.environ["PLANT_NEW"]
cnt = txt.count(old)
if cnt != 1:
    print(f"plant mismatch ({cnt}) for: {old!r}", file=sys.stderr)
    sys.exit(3)
open(p, "w", encoding="utf-8").write(txt.replace(old, new, 1))
PY
    run_case "$id ($desc)" 0
}

cp "$SRC_H" "$SCR/pci_bar_compat_pure.h"
run_case "0 (clean control)" 1

plant 1 "removed UUID verification" \
    '    if (!haveUuid || !uuidMatches) {
        out.status = CompatStatus::kUuidMismatch;
        return out.status;
    }' \
    '    if (false && (!haveUuid || !uuidMatches)) {
        out.status = CompatStatus::kUuidMismatch;
        return out.status;
    }'

plant 2 "removed RDNA4 device allowlist" \
    '    const uint64_t maxAllowed = max_allowed_bar0_for_device(vendor, devId);
    if (maxAllowed == 0ULL) {' \
    '    (void)vendor;
    (void)devId;
    const uint64_t maxAllowed = 16ULL << 30;
    if (false) {'

plant 3 "broken BAR0->BAR2 step (probes BAR1 / corrupts BAR2)" \
    '    uint32_t barNum = 2u;' \
    '    uint32_t barNum = 1u;'

plant 4 "removed duplicate pattern check" \
    '    if (nPrologue > 1) { out.status = CompatStatus::kPatternDuplicate; return out.status; }' \
    '    if (false && nPrologue > 1) { out.status = CompatStatus::kPatternDuplicate; return out.status; }'

plant 5 "removed bridge PFM64 capacity check" \
    '    if ((fFlags & kIOPCIConfiguratorPFM64) == 0u ||
        !check_root_pfm64_capacity(configurator, device, bar0Size64)) {' \
    '    if (false && (fFlags & kIOPCIConfiguratorPFM64) == 0u &&
        !check_root_pfm64_capacity(configurator, device, bar0Size64)) {'

plant 6 "removed late-load refusal" \
    '    if (iopciDeviceInstances > 0u || configuratorInstances > 0u) {
        return CompatStatus::kLateLoad;
    }' \
    '    if (false && (iopciDeviceInstances > 0u || configuratorInstances > 0u)) {
        return CompatStatus::kLateLoad;
    }'

printf 'pci_bar_compat_plant: %d cases run, %d escaped/failed\n' "$total" "$escaped"
[ "$escaped" -eq 0 ]
