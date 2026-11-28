#!/bin/bash
# Isolated host test suite for tools/rdna4/recover-uninstall.sh.
set -euo pipefail

REPO_ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
SCRIPT="$REPO_ROOT/tools/rdna4/recover-uninstall.sh"
BASE_TMP=${TMPDIR:-/tmp}
mkdir -p "$BASE_TMP"
WORK=$(mktemp -d "$BASE_TMP/garnet-recover-sh-test.XXXXXXXX")
trap 'rm -rf "$WORK"' EXIT

fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

# Mock nvram backed by a state file
cat > "$WORK/mock-nvram" <<'EOF'
#!/usr/bin/env bash
state="$MOCK_NVRAM_STATE"
log="$MOCK_NVRAM_LOG"
printf 'nvram %s\n' "$*" >> "$log"
if [ "${1:-}" = "-d" ]; then
    rm -f "$state"
    exit 0
fi
[ -f "$state" ] || exit 1
printf '%s\t%s\n' "$1" "$(cat "$state")"
EOF
chmod 755 "$WORK/mock-nvram"

cat > "$WORK/mock-kmutil" <<'EOF'
#!/usr/bin/env bash
cat "$MOCK_KMUTIL_OUT"
EOF
chmod 755 "$WORK/mock-kmutil"

cat > "$WORK/mock-garnetctl" <<'EOF'
#!/usr/bin/env bash
printf 'garnetctl %s (GARNET_RECOVERY_DAEMON=%s)\n' "$*" "${GARNET_RECOVERY_DAEMON:-unset}" >> "$MOCK_CTL_LOG"
# Verify NVRAM flag was ALREADY cleared before garnetctl runs!
if [ -f "$MOCK_NVRAM_STATE" ]; then
    printf 'ERROR: NVRAM flag still set when garnetctl ran!\n' >&2
    exit 99
fi
exit "${MOCK_CTL_EXIT:-0}"
EOF
chmod 755 "$WORK/mock-garnetctl"

# Case 1: Normal boot (no NVRAM variable) -> exits 0, creates no logs, never calls garnetctl
LOGS1="$WORK/logs1"
: > "$WORK/nvram1.log"
: > "$WORK/ctl1.log"
: > "$WORK/km1.out"
MOCK_NVRAM_STATE="$WORK/nv1.state" MOCK_NVRAM_LOG="$WORK/nvram1.log" \
MOCK_KMUTIL_OUT="$WORK/km1.out" MOCK_CTL_LOG="$WORK/ctl1.log" \
GARNET_NVRAM_BIN="$WORK/mock-nvram" GARNET_KMUTIL_BIN="$WORK/mock-kmutil" \
GARNET_GARNETCTL_BIN="$WORK/mock-garnetctl" GARNET_APP_CTL_BIN="$WORK/missing" \
GARNET_LOGS_DIR="$LOGS1" \
    bash "$SCRIPT"
[ ! -e "$LOGS1" ] || fail "Case 1 created logs on normal boot"
[ ! -s "$WORK/ctl1.log" ] || fail "Case 1 unexpectedly ran garnetctl"

# Case 2: Safe boot with GarnetRecoverAction=uninstall -> clears NVRAM first, runs garnetctl with GARNET_RECOVERY_DAEMON=1, writes recovery-complete.txt
LOGS2="$WORK/logs2"
printf 'uninstall%%00\n' > "$WORK/nv2.state"
: > "$WORK/nvram2.log"
: > "$WORK/ctl2.log"
printf 'com.apple.kpi.bsd\n' > "$WORK/km2.out"
MOCK_NVRAM_STATE="$WORK/nv2.state" MOCK_NVRAM_LOG="$WORK/nvram2.log" \
MOCK_KMUTIL_OUT="$WORK/km2.out" MOCK_CTL_LOG="$WORK/ctl2.log" \
GARNET_NVRAM_BIN="$WORK/mock-nvram" GARNET_KMUTIL_BIN="$WORK/mock-kmutil" \
GARNET_GARNETCTL_BIN="$WORK/mock-garnetctl" GARNET_APP_CTL_BIN="$WORK/missing" \
GARNET_LOGS_DIR="$LOGS2" GARNET_SYSROOT="$WORK/sysroot2" \
    bash "$SCRIPT"
[ ! -f "$WORK/nv2.state" ] || fail "Case 2 did not clear NVRAM flag"
grep -Fq 'GARNET_RECOVERY_DAEMON=1' "$WORK/ctl2.log" || fail "Case 2 missing GARNET_RECOVERY_DAEMON=1"
grep -Fq -- '--sysroot' "$WORK/ctl2.log" || fail "Case 2 did not pass sysroot"
grep -Fqx 'status=UNINSTALLED' "$LOGS2/recovery-complete.txt" || fail "Case 2 missing status=UNINSTALLED"
grep -Fqx 'auto_reboot=no' "$LOGS2/recovery-complete.txt" || fail "Case 2 missing auto_reboot=no"
grep -Fqx 'esp_modified=no' "$LOGS2/recovery-complete.txt" || fail "Case 2 missing esp_modified=no"

# Case 3: Garnet driver is actively loaded -> clears NVRAM flag, refuses uninstall, exits non-zero
LOGS3="$WORK/logs3"
printf 'uninstall\n' > "$WORK/nv3.state"
: > "$WORK/nvram3.log"
: > "$WORK/ctl3.log"
printf 'org.garnet.driver\n' > "$WORK/km3.out"
rc=0
MOCK_NVRAM_STATE="$WORK/nv3.state" MOCK_NVRAM_LOG="$WORK/nvram3.log" \
MOCK_KMUTIL_OUT="$WORK/km3.out" MOCK_CTL_LOG="$WORK/ctl3.log" \
GARNET_NVRAM_BIN="$WORK/mock-nvram" GARNET_KMUTIL_BIN="$WORK/mock-kmutil" \
GARNET_GARNETCTL_BIN="$WORK/mock-garnetctl" GARNET_APP_CTL_BIN="$WORK/missing" \
GARNET_LOGS_DIR="$LOGS3" \
    bash "$SCRIPT" || rc=$?
[ "$rc" -ne 0 ] || fail "Case 3 should fail when org.garnet.driver is loaded"
[ ! -f "$WORK/nv3.state" ] || fail "Case 3 must still clear NVRAM flag to avoid loop"
[ ! -s "$WORK/ctl3.log" ] || fail "Case 3 must NOT invoke garnetctl when driver is loaded"
[ ! -e "$LOGS3/recovery-complete.txt" ] || fail "Case 3 must NOT write recovery-complete.txt"
grep -Fq 'REFUSED: Garnet kexts are actively loaded' "$LOGS3/recovery.log" || fail "Case 3 missing refusal log"

# Case 4: Fallback to Garnet.app garnetctl when self-contained copy is missing
LOGS4="$WORK/logs4"
printf 'uninstall\n' > "$WORK/nv4.state"
: > "$WORK/nvram4.log"
: > "$WORK/ctl4.log"
: > "$WORK/km4.out"
MOCK_NVRAM_STATE="$WORK/nv4.state" MOCK_NVRAM_LOG="$WORK/nvram4.log" \
MOCK_KMUTIL_OUT="$WORK/km4.out" MOCK_CTL_LOG="$WORK/ctl4.log" \
GARNET_NVRAM_BIN="$WORK/mock-nvram" GARNET_KMUTIL_BIN="$WORK/mock-kmutil" \
GARNET_GARNETCTL_BIN="$WORK/nonexistent-self" GARNET_APP_CTL_BIN="$WORK/mock-garnetctl" \
GARNET_LOGS_DIR="$LOGS4" \
    bash "$SCRIPT"
grep -Fq 'garnetctl uninstall' "$WORK/ctl4.log" || fail "Case 4 failed app fallback"

printf 'PASS recover-uninstall-test (4/4 cases)\n'
