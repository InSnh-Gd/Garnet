#!/bin/bash
# RDNA4 auto-translator, host-side v1.
# Every 5 s: list new AIR dumps on the PC (/private/tmp/rdna4m), pull them, translate with add-air.py
# (llvm-dis + metal2vulkan on the host Mac), and push the .spv + .meta.json into the PC's side cache
# /private/var/tmp/rdna4m-spv/, where the bundle's hot-swap watcher picks them up (meta first, .spv last).
# Translations also land in src/garnet-metal/spvcache (for the next bundle build).
set -u
ROOT="${RDNA4_ROOT:-$(cd "$(dirname "$0")/../../.." && pwd)}"
M=$ROOT/src/garnet-metal
WORK=$ROOT/re/pc-26.6.2/ws-air-dumps/auto
LOG=$WORK/air-pull.log
mkdir -p "$WORK"
export PATH=/opt/homebrew/opt/llvm/bin:$PATH
ssh rdna4 'sudo -n mkdir -p /private/var/tmp/rdna4m-spv && sudo -n chmod 755 /private/var/tmp/rdna4m-spv' >>"$LOG" 2>&1
echo "$(date '+%F %T') start" >> "$LOG"
while true; do
  LIST=$(ssh -o ConnectTimeout=5 rdna4 'ls /private/tmp/rdna4m 2>/dev/null | grep "\.air$"' 2>/dev/null)
  for f in $LIST; do
    sha=${f%.air}
    [ -f "$WORK/$sha.done" ] && continue
    [ -f "$M/spvcache/$sha.spv" ] && { touch "$WORK/$sha.done"; continue; }
    scp -q "rdna4:/private/tmp/rdna4m/$sha.*" "$WORK/" 2>>"$LOG" || continue
    j=$(ls "$WORK/$sha".*.json 2>/dev/null | head -1)
    [ -n "$j" ] && python3 -c "import json,sys;print(json.load(open(sys.argv[1]))['function'])" "$j" > "$WORK/$sha.txt" 2>/dev/null
    OUT=$(cd "$M" && python3 add-air.py "$WORK/$sha.air" 2>&1 | head -1)
    echo "$(date '+%F %T') $sha $OUT" >> "$LOG"
    if [ -f "$M/spvcache/$sha.spv" ]; then
      scp -q "$M/spvcache/$sha.meta.json" rdna4:/tmp/$sha.meta.json && scp -q "$M/spvcache/$sha.spv" rdna4:/tmp/$sha.spv && \
      ssh rdna4 "sudo -n mv /tmp/$sha.meta.json /private/var/tmp/rdna4m-spv/$sha.meta.json.tmp && sudo -n mv /private/var/tmp/rdna4m-spv/$sha.meta.json.tmp /private/var/tmp/rdna4m-spv/$sha.meta.json && sudo -n mv /tmp/$sha.spv /private/var/tmp/rdna4m-spv/$sha.spv.tmp && sudo -n mv /private/var/tmp/rdna4m-spv/$sha.spv.tmp /private/var/tmp/rdna4m-spv/$sha.spv && sudo -n chmod 644 /private/var/tmp/rdna4m-spv/$sha.*" >>"$LOG" 2>&1 && \
      echo "$(date '+%F %T') $sha pushed" >> "$LOG"
    fi
    touch "$WORK/$sha.done"
  done
  sleep 5
done
