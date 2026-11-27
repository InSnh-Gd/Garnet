#!/bin/bash
# Self-contained safe-boot recovery handler for com.garnet.recovery (macOS Bash 3.2).
# Checks NVRAM 6E727447-6172-4E65-9452-65636F766572:GarnetRecoverAction.
# On "uninstall": clears the flag first, verifies Garnet kexts are not loaded,
# runs transactional garnetctl uninstall, writes recovery-complete.txt, and never auto-reboots.
set -euo pipefail
umask 022

GARNET_VENDOR_GUID='6E727447-6172-4E65-9452-65636F766572'
VAR_KEY="${GARNET_VENDOR_GUID}:GarnetRecoverAction"

NVRAM_BIN=${GARNET_NVRAM_BIN:-/usr/sbin/nvram}
KMUTIL_BIN=${GARNET_KMUTIL_BIN:-/usr/bin/kmutil}
CTL_SELF=${GARNET_GARNETCTL_BIN:-/Library/Application Support/Garnet/native/garnetctl}
CTL_APP=${GARNET_APP_CTL_BIN:-/Applications/Garnet.app/Contents/Resources/garnetctl}
LOGS_DIR=${GARNET_LOGS_DIR:-/Library/Logs/Garnet}
SYSROOT=${GARNET_SYSROOT:-/}

raw_nvram=$("$NVRAM_BIN" "$VAR_KEY" 2>/dev/null || true)
[ -n "$raw_nvram" ] || exit 0

# nvram outputs "<key>\t<value>"; strip leading key, whitespace, and any trailing %00.
action=$(printf '%s' "$raw_nvram" | awk '{
    line=$0
    sub(/^[^[:space:]]+[[:space:]]+/, "", line)
    gsub(/%00/, "", line)
    gsub(/^[[:space:]]+|[[:space:]]+$/, "", line)
    print line
    exit
}')
if [ "$action" != "uninstall" ]; then
    exit 0
fi

[ ! -L "$LOGS_DIR" ] || exit 1
mkdir -p "$LOGS_DIR"
LOG_FILE="$LOGS_DIR/recovery.log"
COMPLETE_FILE="$LOGS_DIR/recovery-complete.txt"
[ ! -L "$LOG_FILE" ] && [ ! -L "$COMPLETE_FILE" ] || exit 1

log_line() {
    printf '%s [com.garnet.recovery] %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$*" >> "$LOG_FILE"
}

log_line "Triggered by $VAR_KEY=uninstall"

# Always clear the NVRAM trigger BEFORE touching disk so failures can never cause a recovery loop.
if "$NVRAM_BIN" -d "$VAR_KEY" >> "$LOG_FILE" 2>&1; then
    log_line "Cleared $VAR_KEY"
else
    log_line "WARN: failed to clear $VAR_KEY (continuing defensively)"
fi

loaded=$("$KMUTIL_BIN" showloaded --list-only 2>/dev/null || true)
if printf '%s\n' "$loaded" | grep -Eq '(^|[[:space:]])org\.garnet\.(driver|accel)([[:space:]]|$)'; then
    log_line "REFUSED: Garnet kexts are actively loaded; cannot uninstall live kexts."
    log_line "Action: In OpenCore Picker choose GarnetRecover -> [2], then boot macOS directly without rebooting first."
    exit 1
fi

ctl=""
if [ -x "$CTL_SELF" ] && [ ! -L "$CTL_SELF" ]; then
    ctl="$CTL_SELF"
elif [ -x "$CTL_APP" ] && [ ! -L "$CTL_APP" ]; then
    ctl="$CTL_APP"
else
    log_line "ERROR: Could not locate self-contained garnetctl ($CTL_SELF) or app garnetctl ($CTL_APP)"
    exit 1
fi

log_line "Running transactional system-disk uninstall via $ctl (ESP untouched)"
uninstall_rc=0
if [ "$SYSROOT" != "/" ]; then
    GARNET_RECOVERY_DAEMON=1 "$ctl" uninstall --sysroot "$SYSROOT" --allow-nonroot-sandbox >> "$LOG_FILE" 2>&1 || uninstall_rc=$?
else
    GARNET_RECOVERY_DAEMON=1 "$ctl" uninstall >> "$LOG_FILE" 2>&1 || uninstall_rc=$?
fi

if [ "$uninstall_rc" -ne 0 ]; then
    log_line "ERROR: garnetctl uninstall exited $uninstall_rc"
    exit "$uninstall_rc"
fi

now=$(date -u +%Y-%m-%dT%H:%M:%SZ)
cat > "$COMPLETE_FILE" <<EOF
status=UNINSTALLED
completed_at=$now
esp_modified=no
auto_reboot=no
next_step=Mount your EFI partition, open Garnet.app -> OpenCore Assistant -> Restore OpenCore (or run: garnetctl restore-oc --config /Volumes/EFI/EFI/OC/config.plist), then reboot.
EOF
chmod 644 "$COMPLETE_FILE" 2>/dev/null || true
log_line "Recovery uninstall complete; staying on safe desktop (no auto-reboot). See $COMPLETE_FILE"
exit 0
