#!/usr/bin/env zsh
# test-gate-plant.sh - planted breaks for test-gate.c (kext 0.0.640, GPU-apps G4: the bundle's fail-closed gates, the kernel probe, the per-process cache path).
# For each plant: copy the REAL rdna4_gate.h, GarnetDevice.m, test-gate.c and the kernel ABI header into a scratch tree, apply ONE break to the real code (the script first proves the text
# was there exactly once), compile and run the real test and demand that it FAILS (a compile error counts). A plant the suite lets through is a hole: exit non-zero.
#   run from anywhere:  src/garnet-metal/test-gate-plant.sh
set -u
D="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$D/../.." && pwd)"
SCR="${TMPDIR:-/tmp}/gate-plant.$$"
trap 'rm -rf "$SCR"' EXIT
escaped=0; total=0
fresh() {
  rm -rf "$SCR"; mkdir -p "$SCR/src/garnet-metal" "$SCR/src/garnet/src"
  cp "$D/rdna4_gate.h" "$D/GarnetDevice.m" "$D/test-gate.c" "$D/Info.plist" "$SCR/src/garnet-metal/"
  python3 "$D/assemble-device.py" "$SCR/src/garnet-metal/GarnetDevice.m" --include-dir "$D" --output "$SCR/src/garnet-metal/GarnetDevice.m"
  cp "$ROOT/src/garnet/src/GarnetNativeABI.h" "$SCR/src/garnet/src/"
}
build_run() {
  OUT=$(cd "$SCR/src/garnet-metal" && cc -O1 -Wall -Wextra -Werror -o "$SCR/t" test-gate.c 2>&1) || return 2
  OUT=$(cd "$SCR/src/garnet-metal" && "$SCR/t" . 2>&1) && return 0
  return 1
}
plant() {   # plant <id> <file in the metal dir> <old> <new> <description>
  local id="$1" file="$2" old="$3" new="$4" desc="$5"
  fresh
  OLD="$old" NEW="$new" FILE="$SCR/src/garnet-metal/$file" python3 - <<'PY'
import os,sys
p=os.environ['FILE']; s=open(p).read(); o=os.environ['OLD']; n=os.environ['NEW']
if s.count(o)!=1:
    print("PLANT TEXT NOT FOUND EXACTLY ONCE (%d): %r" % (s.count(o), o[:70])); sys.exit(3)
open(p,'w').write(s.replace(o,n))
PY
  local rc=$?
  total=$((total+1))
  if [ $rc -ne 0 ]; then echo "PLANT $id: $desc: THE PLANT ITSELF DID NOT APPLY"; escaped=$((escaped+1)); return; fi
  build_run; rc=$?
  if [ $rc -eq 2 ]; then echo "PLANT $id: $desc: CAUGHT at compile time: $(echo "$OUT" | grep -m1 -E 'error' | cut -c1-140)"; return; fi
  if [ $rc -eq 0 ]; then echo "PLANT $id: $desc: *** ESCAPED (the suite passed) ***"; escaped=$((escaped+1)); return; fi
  echo "PLANT $id: $desc: CAUGHT by $(echo "$OUT" | grep -c '^FAIL') check(s); first: $(echo "$OUT" | grep -m1 '^FAIL' | cut -c1-120)"
}
fresh; total=$((total+1)); build_run; rc=$?
if [ $rc -ne 0 ]; then echo "BASELINE: the unmodified scratch copy does not pass (rc $rc): $(echo "$OUT" | grep -m1 -E 'error|^FAIL' | cut -c1-140)"; escaped=$((escaped+1)); else echo "BASELINE: the unmodified scratch copy compiles and passes ($(echo "$OUT" | tail -1))"; fi
G=rdna4_gate.h; M=GarnetDevice.m
plant 1  $G "return (err == ENOENT || err == ENOTDIR) ? RDNA4G_KILL_ABSENT : RDNA4G_KILL_UNREADABLE;" "return (err == ENOENT || err == ENOTDIR || err == EPERM) ? RDNA4G_KILL_ABSENT : RDNA4G_KILL_UNREADABLE;" "GATE 1: a sandbox-denied stat (EPERM) reads as 'no kill file' (fails OPEN)"
plant 2  $G "return isWs ? 0 : (state != RDNA4G_KILL_ABSENT);" "(void)isWs; (void)state; return 0;" "GATE 2: an unreadable kill file declines nobody outside WindowServer (fails OPEN)"
plant 3  $G "return isWs ? 0 : (state != RDNA4G_KILL_ABSENT);" "(void)isWs; return state != RDNA4G_KILL_ABSENT;" "GATE 3: an unreadable kill file now declines WindowServer too (today's rule changed)"
plant 4  $G "if (state == RDNA4G_KILL_PRESENT) return !ignored;" "(void)ignored; if (state == RDNA4G_KILL_PRESENT) return 1;" "GATE 4: the root test bypass no longer works"
plant 5  $G "if (state == RDNA4G_KILL_PRESENT) return !ignored;" "(void)ignored; if (state == RDNA4G_KILL_PRESENT) return 0;" "GATE 5: a PRESENT kill file declines nobody"
plant 6  $G "static inline int rdna4g_ready_declines(int isWs, int flag) { return isWs ? (flag == 0) : (flag != 1); }" "static inline int rdna4g_ready_declines(int isWs, int flag) { (void)isWs; return flag == 0; }" "GATE 6: an absent / unreadable Ready property proceeds outside WindowServer (fails OPEN)"
plant 7  $G "static inline int rdna4g_ready_declines(int isWs, int flag) { return isWs ? (flag == 0) : (flag != 1); }" "static inline int rdna4g_ready_declines(int isWs, int flag) { (void)isWs; return flag != 1; }" "GATE 7: WindowServer declines when Ready is absent (today's rule changed)"
plant 8  $G "static inline int rdna4g_autodisarm_declines(int isWs, int flag) { (void)isWs; return flag == 1; }" "static inline int rdna4g_autodisarm_declines(int isWs, int flag) { (void)isWs; (void)flag; return 0; }" "GATE 8: AutoDisarmed == 1 declines nobody"
plant 9  $G "    if ((infoFlags & RDNA4G_INFO_HUNG) != 0u) return RDNA4G_PROBE_HUNG;" "    (void)infoFlags;" "PROBE 9: a HUNG GPU does not decline the application"
plant 10 $G "    if (infoRc != 0u) return RDNA4G_PROBE_INFO;" "    (void)infoRc;" "PROBE 10: an unreadable QueryInfo does not decline (fails OPEN)"
plant 11 $G "    if (helloRc != 0u) return RDNA4G_PROBE_HELLO;" "    (void)helloRc;" "PROBE 11: a failed Hello does not decline"
plant 12 $G "    if (openRc != 0u) return RDNA4G_PROBE_OPEN;" "    (void)openRc;" "PROBE 12: a refused open does not decline (the kernel's verdict is ignored)"
plant 13 $G "static inline int rdna4g_probe_retry(uint32_t openRc, int tries) { return openRc == RDNA4G_KR_EXCLUSIVE && tries < RDNA4G_PROBE_RETRIES; }" "static inline int rdna4g_probe_retry(uint32_t openRc, int tries) { return openRc != 0u && tries < RDNA4G_PROBE_RETRIES; }" "PROBE 13: every refusal is retried (the allow-list refusal too)"
plant 14 $G "static inline int rdna4g_probe_retry(uint32_t openRc, int tries) { return openRc == RDNA4G_KR_EXCLUSIVE && tries < RDNA4G_PROBE_RETRIES; }" "static inline int rdna4g_probe_retry(uint32_t openRc, int tries) { (void)tries; return openRc == RDNA4G_KR_EXCLUSIVE; }" "PROBE 14: the retry is unbounded"
plant 15 $G "#define RDNA4G_INFO_FLAGS_OFF   8u" "#define RDNA4G_INFO_FLAGS_OFF   12u" "PROBE 15: the probe reads the flags word at the wrong offset"
plant 16 $G "#define RDNA4G_SEL_QUERYINFO    1u" "#define RDNA4G_SEL_QUERYINFO    2u" "PROBE 16: the probe calls ReadRegs instead of QueryInfo"
plant 17 $G "#define RDNA4G_KR_EXCLUSIVE     0xe00002c5u" "#define RDNA4G_KR_EXCLUSIVE     0xe00002c1u" "PROBE 17: the retried code is not ExclusiveAccess"
plant 18 $G "        if (!userCacheDir || !userCacheDir[0]) return 0;" "        if (!userCacheDir || !userCacheDir[0]) userCacheDir = \"/private/var/tmp/\";" "CACHE 18: an application with no user cache dir falls back to /private/var/tmp (the sticky directory beside WindowServer's file)"
plant 19 $G "c == '.' || c == '-' || c == '_') ? c : '_';" "c == '.' || c == '-' || c == '_' || c == '/') ? c : '_';" "CACHE 19: a '/' in the program name reaches the path"
plant 20 $G "static inline int rdna4g_pcache_save(int cls, int rootSaveEnv) { return cls == RDNA4G_CLASS_WS ? 1 : cls == RDNA4G_CLASS_ROOT ? rootSaveEnv : 1; }" "static inline int rdna4g_pcache_save(int cls, int rootSaveEnv) { (void)cls; (void)rootSaveEnv; return 1; }" "CACHE 20: a root tool also saves (its file would block WindowServer's rename)"
plant 21 $G "static inline int rdna4g_pcache_save(int cls, int rootSaveEnv) { return cls == RDNA4G_CLASS_WS ? 1 : cls == RDNA4G_CLASS_ROOT ? rootSaveEnv : 1; }" "static inline int rdna4g_pcache_save(int cls, int rootSaveEnv) { return cls == RDNA4G_CLASS_WS ? 1 : rootSaveEnv; }" "CACHE 21: an application never saves its cache"
plant 22 $G "        const char *dir = (envDirAllowed && envDirAllowed[0]) ? envDirAllowed : (varTmpWritable ? \"/private/var/tmp\" : \"/private/tmp\");" "        const char *dir = \"/private/var/tmp\"; (void)envDirAllowed; (void)varTmpWritable;" "CACHE 22: WindowServer's directory choice is hard-coded"
plant 23 $G "    if (cls == RDNA4G_CLASS_OTHER) {
        if (!userCacheDir" "    if (cls == RDNA4G_CLASS_OTHER && 0) {
        if (!userCacheDir" "CACHE 23: an application gets WindowServer's shared cache file"
plant 24 $G "    return isWs || force || testAsWs || appAdmitted;" "    (void)isWs; (void)appAdmitted; return force || testAsWs;" "FALLBACK 24: WS and admitted apps lose placeholders (c4aa594 regression)"
plant 25 $G "    return isWs || force || testAsWs || appAdmitted;" "    (void)isWs; (void)force; (void)testAsWs; (void)appAdmitted; return 1;" "FALLBACK 25: every process gets the fallback"
plant 26 $G "static inline int rdna4g_class(int isWs, int allowRoot) { return isWs ? RDNA4G_CLASS_WS : allowRoot ? RDNA4G_CLASS_ROOT : RDNA4G_CLASS_OTHER; }" "static inline int rdna4g_class(int isWs, int allowRoot) { (void)allowRoot; return isWs ? RDNA4G_CLASS_WS : RDNA4G_CLASS_ROOT; }" "CLASS 26: no process is ever an 'other' process (the kernel probe never runs)"
plant 27 $M "        const char *why = rdna4_app_probe(port);" "        const char *why = NULL;" "ADMIT 27: rdna4_admit never asks the kernel (any process is admitted)"
plant 28 $M "    if (cls == RDNA4G_CLASS_OTHER) {   // not WindowServer, not an RDNA4M_ALLOW root tool: only the kernel can admit it" "    if (1) {   // not WindowServer, not an RDNA4M_ALLOW root tool: only the kernel can admit it" "ADMIT 28: WindowServer and root tools also go through the probe"
plant 29 $M "    if (rdna4g_kill_declines(isWs, ks, killIgnored)) {" "    if (0) {" "ADMIT 29: rdna4_admit never applies the unreadable-kill-file rule"
plant 30 $M "    if (rdna4g_ready_declines(isWs, rdy)) return" "    if (rdy == 0) return" "ADMIT 30: rdna4_admit tests Ready with the old bare '== 0'"
plant 31 $M "            IOServiceClose(conn);   // the session is released at once; Mesa opens its own later" "            (void)conn;   // the session is released at once; Mesa opens its own later" "ADMIT 31: the probe connection is never closed (a session slot leaks per process)"
plant 32 $M "rdna4g_fallback_ok(rdna4_is_ws(), rdna4_force_fallback(), rdna4_test_fb_as_ws(), atomic_load(&rdna4_app_admitted))" "rdna4g_fallback_ok(rdna4_is_ws(), rdna4_force_fallback(), rdna4_test_fb_as_ws(), 0)" "ADMIT 32: the fallback rule never sees the admitted flag"
plant 33 $M "        atomic_store(&rdna4_app_admitted, 1);" "        atomic_store(&rdna4_app_admitted, 1);
        atomic_store(&rdna4_app_admitted, 1);" "ADMIT 33: the admitted flag is stored in a second place"
plant 35 $M "    if (!rdna4g_pcache_path(RDNA4PC.path, sizeof RDNA4PC.path, pcls, envdir, vtw, ucd, getprogname()))" "    if (!rdna4g_pcache_path(RDNA4PC.path, sizeof RDNA4PC.path, RDNA4G_CLASS_WS, envdir, 1, ucd, getprogname()))" "CACHE 35: rdna4_pc_open always asks for WindowServer's path"
plant 36 $M "return RDNA4G_FLAG_NONUB;" "return RDNA4G_FLAG_ABSENT;" "NUB 36: an unreachable nub reads as 'property absent'"
plant 37 $M "    if (!v) return RDNA4G_FLAG_ABSENT;" "    if (!v) return 1;" "NUB 37: an absent property reads as 'true'"
plant 38 $G "    if ((budgetFlags & RDNA4G_BUDGET_APP) == 0u) return RDNA4G_PROBE_NOTAPP;" "    (void)budgetFlags;" "PROBE 38: a root process the kernel admits as root is admitted as an application"
plant 39 $G "#define RDNA4G_BUDGET_APP       2u" "#define RDNA4G_BUDGET_APP       1u" "PROBE 39: the APP bit is the wrong bit (the VALID bit)"
plant 40 $G "#define RDNA4G_INFO_BUDGET_OFF  172u" "#define RDNA4G_INFO_BUDGET_OFF  168u" "PROBE 40: the probe reads the budget flags from the wrong word"
plant 41 $M "memcpy(&budgetFlags, info + RDNA4G_INFO_BUDGET_OFF, sizeof budgetFlags);" "budgetFlags = 2u;" "PROBE 41: the bundle fakes the APP role instead of reading it"
# ---- 0.0.641 (G4 review fixes): the euid >= 501 probe scope, the dump directory, the bundle build ----
plant 42 $G "static inline int rdna4g_declined_by_class(int cls, uint32_t euid) { return cls == RDNA4G_CLASS_OTHER && euid < RDNA4G_APP_MIN_EUID; }" "static inline int rdna4g_declined_by_class(int cls, uint32_t euid) { (void)cls; (void)euid; return 0; }" "SCOPE 42: root and daemons are probed (the review's HIGH 2)"
plant 43 $G "#define RDNA4G_APP_MIN_EUID 501u" "#define RDNA4G_APP_MIN_EUID 500u" "SCOPE 43: the first probed euid is 500"
plant 44 $G "return cls == RDNA4G_CLASS_OTHER && euid < RDNA4G_APP_MIN_EUID; }" "return euid < RDNA4G_APP_MIN_EUID; (void)cls; }" "SCOPE 44: WindowServer (and RDNA4M_ALLOW root tools) are declined by class too"
plant 45 $M "    if (rdna4g_declined_by_class(cls, (uint32_t)geteuid())) return RDNA4G_DECLINE_BY_CLASS_TEXT;
" "" "SCOPE 45: rdna4_admit never declines by class (every process reaches the kernel probe)"
plant 46 $M "if (rdna4g_declined_by_class(cls, (uint32_t)geteuid()))" "if (rdna4g_declined_by_class(cls, (uint32_t)getuid()))" "SCOPE 46: the decline uses the REAL uid, not the effective one (a setuid-root process is probed)"
plant 47 $M "if (rdna4g_declined_by_class(cls, (uint32_t)geteuid())) return RDNA4G_DECLINE_BY_CLASS_TEXT;" "if (rdna4g_declined_by_class(cls, (uint32_t)geteuid())) {}" "SCOPE 47: the by-class decline is evaluated but not acted on"
plant 48 $G "#define RDNA4G_DECLINE_BY_CLASS_TEXT \"process is not WindowServer and RDNA4M_ALLOW=1 (as root) is not set\"" "#define RDNA4G_DECLINE_BY_CLASS_TEXT \"declined\"" "SCOPE 48: the by-class reason is not 0.0.632's"
plant 49 $G "return lstatRc == 0 && isDir && !isLnk && ownerUid == callerUid; }" "(void)ownerUid; (void)callerUid; return lstatRc == 0 && isDir && !isLnk; }" "DUMP 49: a directory owned by someone else is used"
plant 50 $G "return lstatRc == 0 && isDir && !isLnk && ownerUid == callerUid; }" "(void)isLnk; return lstatRc == 0 && isDir && ownerUid == callerUid; }" "DUMP 50: a symlink is used"
plant 51 $G "return (mode & 0077u) != 0u; }" "return (mode & 0007u) != 0u; }" "DUMP 51: group-writable directories of ours are not narrowed"
plant 56 $M "    if (!rdna4g_dumpdir_ok(rc, " "    if (0 && !rdna4g_dumpdir_ok(rc, " "DUMP 56: the helper never applies the verdict"
# ---- bundle build 6 (C1): per-uid dump subdirectories, the bounded synchronous wait, the family claim ----
plant 52 $M "(void)mkdir(RDNA4G_DUMP_DIR, RDNA4G_DUMP_ROOT_MODE);" "(void)mkdir(RDNA4G_DUMP_DIR, 0777);" "DUMP 52: the parent is created 0777 without the sticky bit"
plant 53 $M "const int rc = lstat(sub, &st);" "const int rc = stat(sub, &st);" "DUMP 53: the subdirectory is checked with stat (follows a symlink)"
plant 54 $M "(void)fchmodat(AT_FDCWD, RDNA4G_DUMP_DIR, RDNA4G_DUMP_ROOT_MODE, AT_SYMLINK_NOFOLLOW);" "(void)chmod(RDNA4G_DUMP_DIR, RDNA4G_DUMP_ROOT_MODE);" "DUMP 54: the parent repair follows a symlink (plain chmod)"
plant 55 $M "if (!rdna4_dump_dir_ready(sub, sizeof sub)) {" "if (0) {" "DUMP 55: the dump never checks its directory"
plant 57 $M "if (!rdna4_dump_write(side.UTF8String, js)) {" "if (!rdna4_dump_write(air.UTF8String, bc)) {" "DUMP 57: the .air is written instead of its sidecar (the daemon can start without the sidecar)"
plant 59 $G "(ownerUid == 0u || ownerUid == callerUid || ownerUid == RDNA4G_DAEMON_UID)" "(ownerUid == 0u || ownerUid == callerUid || ownerUid == RDNA4G_DAEMON_UID || 1)" "ROOT 59: a parent owned by a THIRD user is accepted"
plant 60 $G "(mode & 07777u) == RDNA4G_DUMP_ROOT_MODE;" "(mode & 01000u) != 0u;" "ROOT 60: a sticky parent with any other bits (0700, 4777) is accepted"
plant 61 $G "return ownerUid == callerUid && (mode & 07777u) != RDNA4G_DUMP_ROOT_MODE; }" "return (mode & 07777u) != RDNA4G_DUMP_ROOT_MODE; (void)ownerUid; (void)callerUid; }" "ROOT 61: a parent owned by someone else is also 'repaired' (chmod on another user's directory)"
plant 62 $G "snprintf(out, n, \"%s/%u\", RDNA4G_DUMP_DIR, (unsigned)uid)" "snprintf(out, n, \"%s/x%u\", RDNA4G_DUMP_DIR, (unsigned)uid)" "SUB 62: the per-uid subdirectory name is not the uid"
plant 63 $G "return (isWs || appAdmitted) && !force && !testAsWs; }" "return appAdmitted && !isWs && !force && !testAsWs; }" "WAIT 63: WindowServer loses its bounded wait"
plant 64 $G "#define RDNA4G_SYNC_WAIT_WS_MS 250" "#define RDNA4G_SYNC_WAIT_WS_MS 60000" "WAIT 64: WindowServer can block for 60 seconds"
plant 65 $G "if (found) s->strikes = 0;" "if (found) {}" "WAIT 65: a successful wait does not reset strikes"
plant 66 $G "#define RDNA4G_SYNC_STRIKES 3" "#define RDNA4G_SYNC_STRIKES 1000" "WAIT 66: a dead daemon never switches the wait off"
plant 67 $G "#define RDNA4G_SYNC_WAIT_APP_MS 3000" "#define RDNA4G_SYNC_WAIT_APP_MS 60000" "WAIT 67: apps can block for 60 seconds"
plant 80 $G "return isWs ? RDNA4G_SYNC_WAIT_WS_MS : RDNA4G_SYNC_WAIT_APP_MS;" "(void)isWs; return RDNA4G_SYNC_WAIT_APP_MS;" "WAIT 80: WindowServer gets the app wait budget"
plant 81 $G "else if (s->strikes < RDNA4G_SYNC_STRIKES) s->strikes++;" "else s->strikes++;" "WAIT 81: timeout strikes do not saturate"
plant 82 $M "if (!waiting) waitMs = 0;" "if (!waiting) waitMs = RDNA4G_SYNC_WAIT_APP_MS;" "WAIT 82: exhausted in-process budget still blocks the caller"
plant 68 $G "case RDNA4G_FAMILY_MAC2: case RDNA4G_FAMILY_COMMON1:" "case RDNA4G_FAMILY_METAL3: case RDNA4G_FAMILY_MAC2: case RDNA4G_FAMILY_COMMON1:" "FAMILY 68: Metal3 is claimed again"
plant 78 $G "case RDNA4G_FAMILY_METAL3: return isWs ? 1 : 0;" "case RDNA4G_FAMILY_METAL3: return 0;" "FAMILY 78: WindowServer loses its Metal3 answer"
plant 79 $M "    if (rdna4_is_ws()) {   // the base class" "    if (0) {   // the base class" "FAMILY 79: WindowServer gets Tier1 instead of the base answer"
plant 69 $M "return (NSUInteger)RDNA4G_ARGBUF_TIER1; }" "return 1; }" "FAMILY 69: argument buffers report tier 2"
plant 70 $M "if (rdna4_sync_wait(\"render\", want, NO)) {" "if (0) {" "WAIT 70: the render miss never waits"
plant 71 $G "return (isWs || appAdmitted) && !force && !testAsWs; }" "(void)force; (void)testAsWs; return isWs || appAdmitted; }" "WAIT 71: explicit placeholder tests also wait"
plant 72 $M "fchmod(fd, 0644)" "fchmod(fd, 0666)" "DUMP 72: dump files are world-writable"
plant 73 $M "const int fd = mkstemp(tmp);" "const int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);" "DUMP 73: the temp file no longer uses exclusive creation"
plant 74 $M "fs.st_ino != st.st_ino ||" "0 ||" "DUMP 74: the open descriptor is not tied to the lstat'ed directory"
plant 75 $M "rdna4da_grant_list(fd, (uid_t)RDNA4G_DAEMON_UID)" "rdna4da_grant_list(fd, (uid_t)0)" "DUMP 75: the ACL grant goes to the wrong account"
plant 76 $M "if (ksha && rdna4_sync_wait(\"kernel\", @[ ksha ], YES)) {" "if (ksha && rdna4_sync_wait(\"kernel\", @[ ksha ], NO)) {" "WAIT 76: the compute wait does not require the .meta.json (kernel_dispatch lives there)"
plant 77 $G "return s->strikes < RDNA4G_SYNC_STRIKES;" "(void)s; return 1;" "WAIT 77: exhausted strikes never disable waiting"
plant 58 Info.plist "<key>CFBundleVersion</key>
	<string>43</string>" "<key>CFBundleVersion</key>
	<string>2</string>" "VERSION 58: the bundle build is not bumped"
# ---- bundle 8 (G6): sandboxed applications open RDNA4N on the accelerator port ----
plant 90 $M "    io_service_t svc = (io_service_t)port;   // G6: the accelerator port (0 = none); never released here" "    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching(\"GarnetDriver\"));" "G6 90: the APP probe goes back to a GarnetDriver lookup by name (a sandbox denies the open)"
plant 91 $G "return cls == RDNA4G_CLASS_OTHER ? RDNA4G_SVC_ACCEL : (const char *)0;" "(void)cls; return RDNA4G_SVC_ACCEL;" "G6 91: WindowServer and root tools are routed to the accelerator too"
plant 92 $G "return cls == RDNA4G_CLASS_OTHER ? RDNA4G_SVC_ACCEL : (const char *)0;" "(void)cls; return (const char *)0;" "G6 92: the application class keeps Mesa's default service (GarnetDriver: denied by the sandbox)"
plant 93 $M "if (!setSvc) { RDNA4R.err = rdna4_err(7, @\"no \" RDNA4G_MESA_SETTER \" in RADV (an application needs the accelerator route)\"); goto fail; }" "if (!setSvc) { }" "G6 93: a RADV without the route setter is used anyway (fails OPEN onto the wrong service)"
plant 94 $M "            if (svcCls) {" "            if (0) {" "G6 94: the route setter is never called"
plant 95 $M "        if (kr == KERN_SUCCESS) {
            uint64_t in[2] = { 1ull, RDNA4G_HELLO_F_MINOR }" "        IOObjectRelease(svc);
        if (kr == KERN_SUCCESS) {
            uint64_t in[2] = { 1ull, RDNA4G_HELLO_F_MINOR }" "G6 95: the probe releases the port Metal gave the bundle (a reference it does not own)"
plant 96 $G "#define RDNA4G_MESA_SETTER      \"radv_darwin_set_service_class\"" "#define RDNA4G_MESA_SETTER      \"radv_darwin_set_service\"" "G6 96: the bundle looks up a setter name Mesa does not export"
plant 97 $M "const int src = setSvc(svcCls);
                if (src != 0) {" "const int src = setSvc(svcCls); (void)src;
                if (0) {" "G6 97: a refused service name is ignored"
echo "test-gate-plant: $((total - 1)) plants (+ the baseline), $escaped escaped or did not apply"
[ $escaped -eq 0 ]
