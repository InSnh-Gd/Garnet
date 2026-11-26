#!/bin/bash
# GarnetAccel verify: READ-ONLY. Run ON the PC (e.g. `ssh garnet 'bash -s' < verify.sh`). Changes nothing. Prints PASS/FAIL/INFO lines.
# PHASE 1 (after reboot, before activation): Garnet.kext is boot-loaded from the Aux KC
# (/Library/Extensions/Garnet.kext); GarnetAccel is present in the Aux KC but is
# not loaded until activation.
# PHASE 2 (after activation/publish): additionally require loaded GarnetAccel, nub and accelerator.
set -uo pipefail
R=${GARNET_VERIFY_ROOT:-${RDNA4_VERIFY_ROOT:-}}
AUX="$R/Library/KernelCollections/AuxiliaryKernelExtensions.kc"
K="$R/Library/Extensions/GarnetAccel.kext"
PLISTBUDDY=${GARNET_VERIFY_PLISTBUDDY:-${RDNA4_VERIFY_PLISTBUDDY:-/usr/libexec/PlistBuddy}}
failures=0
ok(){ echo "PASS  $*"; }; no(){ echo "FAIL  $*"; failures=$((failures + 1)); }; inf(){ echo "INFO  $*"; }
has_token(){ printf '%s\n' "$1" | awk -v wanted="$2" '{ for (i = 1; i <= NF; i++) if ($i == wanted) found = 1 } END { exit !found }'; }
echo "== GarnetAccel verify $(date '+%F %T') =="
product=$(sw_vers -productVersion 2>/dev/null) || product=
build=$(sw_vers -buildVersion 2>/dev/null) || build=
inf "macOS ${product:-unknown} build ${build:-unknown} (expect 26.7.2 / 25G332)"
[ "$product" = 26.7.2 ] && [ "$build" = 25G332 ] && ok "target macOS build" || no "unsupported macOS (expected 26.7.2 / 25G332)"
inf "boot-args: $(nvram boot-args 2>/dev/null | cut -f2)"
[ -d "$R/Library/Extensions/Garnet.kext" ] && ok "Garnet.kext is in /Library/Extensions" || no "Garnet.kext is missing from /Library/Extensions"
[ -d "$R/Library/Extensions/RDNA4Bringup.kext" ] && no "the old /Library/Extensions/RDNA4Bringup.kext is still installed" || ok "no RDNA4Bringup.kext in /Library/Extensions"
[ -d "$R/Library/Extensions/RDNA4AccelProbe.kext" ] && no "the old /Library/Extensions/RDNA4AccelProbe.kext is still installed" || ok "no old RDNA4AccelProbe.kext in /Library/Extensions"
if [ -d "$K" ]; then
  version=$($PLISTBUDDY -c 'Print :CFBundleVersion' "$K/Contents/Info.plist" 2>&1) || version="<read failed>"
  [ "$version" = 0.1.1 ] && ok "GarnetAccel version 0.1.1" || no "GarnetAccel version is $version (expect 0.1.1)"
else
  no "GarnetAccel is not installed at $K"
fi
if ! ins=$(kmutil inspect -A "$AUX" 2>&1); then
  no "kmutil inspect failed for the Aux KC: $ins"
  ins=
fi
printf '%s\n' "$ins" | grep -iE 'garnet\.(driver|accel)|Garnet(Accel)?' | sed 's/^/INFO  /'
printf '%s\n' "$ins" | grep -qi 'org\.garnet\.driver' && ok "[PHASE 1] Garnet is in the aux KC" || no "[PHASE 1] Garnet is not in the aux KC"
printf '%s\n' "$ins" | grep -qi 'org\.garnet\.accel' && ok "[PHASE 1] GarnetAccel is in the aux KC" || no "[PHASE 1] GarnetAccel is not in the aux KC"
printf '%s\n' "$ins" | grep -Eqi 'com\.hackintosh\.RDNA4FB|RDNA4FB\.kext' && no "legacy RDNA4FB is in the aux KC" || ok "legacy RDNA4FB is not in the aux KC"
printf '%s\n' "$ins" | grep -qi 'com\.hackintosh\.AMDRadeonRDNA4' && no "AMDRadeonRDNA4 display plugin is in the aux KC" || ok "AMDRadeonRDNA4 is not in the aux KC"
printf '%s\n' "$ins" | grep -Eqi 'as\.vit9696\.Lilu|com\.rdna4\.(bringup|accelprobe)' && no "conflicting legacy RDNA4/Lilu kext is in the aux KC" || ok "no legacy RDNA4/Lilu kext in the aux KC"
if ! sl=$(kmutil showloaded --list-only 2>&1); then
  no "kmutil showloaded failed: $sl"
  sl=
fi
printf '%s\n' "$sl" | grep -i 'garnet' | sed 's/^/INFO  /'
for bundle_id in org.garnet.driver; do
  has_token "$sl" "$bundle_id" && ok "$bundle_id boot-loaded" || no "required boot component is not loaded: $bundle_id"
done
for legacy_id in as.vit9696.Lilu com.hackintosh.AMDRadeonRDNA4 com.hackintosh.RDNA4FB com.rdna4.bringup com.rdna4.accelprobe; do
  has_token "$sl" "$legacy_id" && no "conflicting legacy kext is loaded: $legacy_id" || ok "no conflicting legacy $legacy_id"
done
has_token "$sl" org.garnet.accel && ok "[PHASE 2] GarnetAccel is loaded" || inf "[PHASE 1] GarnetAccel is not loaded (expected before activation)"
nub=$(ioreg -c GarnetMetalNub -r -l 2>/dev/null)
if [ -n "$nub" ]; then
  ok "[PHASE 2] GarnetMetalNub is in the registry"
  echo "$nub" | grep -E '"model"|Garnet,' | sed 's/^/INFO  /'
  acc=$(ioreg -c GarnetAccelerator -r -l 2>/dev/null)
  if [ -n "$acc" ]; then
    ok "[PHASE 2] GarnetAccelerator is in the registry"
    echo "$acc" | grep -E 'MetalPluginName|MetalPluginClassName|IOMatchCategory|IOAccelRevision|AccelCaps|IOCFPlugInTypes|"IOClass"|IOProviderClass' | sed 's/^/INFO  /'
    echo "$acc" | grep -q 'IOAccelRevision' && ok "IOAccelRevision published by the family start" || no "IOAccelRevision missing (family start did not finish?)"
    echo "$acc" | grep -q '"MetalPluginName" = "GarnetMetal"' && ok "MetalPluginName on the accelerator itself" || no "MetalPluginName missing on the accelerator"
  else no "[PHASE 2] no GarnetAccelerator: see the log lines below"; fi
else inf "[PHASE 1] no GarnetMetalNub (not published: expected before garnetnub publish)"; fi
L=$(log show --last boot --predicate 'eventMessage CONTAINS "garnetaccel:" OR eventMessage CONTAINS "GarnetDriver: metal:"' --style compact 2>/dev/null | grep -v '^Timestamp\|^Filtering\|log run noninteractively')
[ -z "$L" ] && L=$(sudo -n dmesg 2>/dev/null | grep -E 'garnetaccel:|GarnetDriver: metal:')
echo "$L" | grep -q 'garnetaccel: layout PASS' && ok "[GATE] runtime layout gate PASS: $(echo "$L" | grep -m1 'layout PASS' | sed 's/.*garnetaccel: //')" || inf "no 'garnetaccel: layout PASS' line (expected only in PHASE 2)"
echo "$L" | grep -q 'layout PASS 2720/2720' && ok "[GATE] aux 0.1.1: 13 classes / 2720 slots compared (GarnetFramebuffer included)" || inf "no 'layout PASS 2720/2720' line (expected only in PHASE 2)"
echo "$L" | grep -q 'layout FAIL' && no "[GATE] layout FAIL: $(echo "$L" | grep -m1 'layout FAIL' | sed 's/.*garnetaccel: //')"
echo "$L" | grep -E 'garnetaccel:|GarnetDriver: metal:' | tail -30 | sed 's/^/INFO  /'
inf "panics: $(ls -t "$R/Library/Logs/DiagnosticReports"/Kernel* 2>/dev/null | head -1)"
inf "WindowServer / loginwindow pids: $(pgrep -x WindowServer | tr '\n' ' ') / $(pgrep -x loginwindow | tr '\n' ' ')"
if [ "$failures" -eq 0 ]; then
  ok "verification complete"
  exit 0
fi
printf 'FAIL  verification complete with %s failed check(s)\n' "$failures" >&2
exit 1
