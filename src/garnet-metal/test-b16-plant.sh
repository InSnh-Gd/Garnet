#!/usr/bin/env zsh
# test-b16-plant.sh - planted breaks for bundle build 16 (app-fix round 1, an internal design note): P5 (live-import ledger, F1 never-nil fallback, F2 import cache default ON, F3 use-count policy), P1 (IOSurface wrapper aliasing),
# P4 (installed-pair early-out, log wording). Same discipline as test-b14-plant.sh: copy the REAL files into a scratch tree, apply ONE break (the script proves the text was there exactly once), run the suites and demand that at least one FAILS
# (a compile error counts). A plant the tests let through is a hole: exit non-zero.
#   usage: test-b16-plant.sh           env: TMPDIR (where the scratch tree goes)
#   Fast mode: C compiles at -O0, no sanitizers.
set -u
D="$(cd "$(dirname "$0")" && pwd)"
SCR="$(mktemp -d "${TMPDIR:-/tmp}/b16-plant.XXXXXX")"
case "$SCR" in */b16-plant.*) ;; *) echo "bad scratch dir $SCR"; exit 2;; esac
trap 'rm -rf -- "$SCR"' EXIT
escaped=0; total=0
W="$SCR/c"
cfresh() { rm -rf -- "$W"; mkdir -p "$W/autotranslate"; cp "$D"/rdna4_xlate.h "$D"/rdna4_gate.h "$D"/rdna4_ledger.h "$D"/rdna4_ioalias.h "$D"/rdna4_impcache.h "$D"/GarnetDevice.m "$D"/test-xlate.c "$D"/test-ledger.c "$D"/test-ioalias.c "$D"/test-impcache.c "$W/"; cp "$D/../../tools/native/autotranslate/overrides.txt" "$W/autotranslate/"; python3 "$D/assemble-device.py" "$W/GarnetDevice.m" --include-dir "$D" --output "$W/GarnetDevice.m"; }
c_test() {
  OUT=""
  local t
  for t in ledger ioalias impcache; do
    ( cd "$W" && cc -O0 -Wall -Wextra -Werror -o t-$t test-$t.c ) >"$SCR/cc.out" 2>&1 || { OUT="test-$t: compile error: $(grep -m1 error "$SCR/cc.out" | cut -c1-100)"; return 1; }
    ( cd "$W" && ./t-$t . ) >"$SCR/o0" 2>&1 || { OUT="test-$t: $(grep -m1 '^FAIL' "$SCR/o0" | cut -c1-110)"; [ -n "${OUT#test-$t: }" ] || OUT="test-$t died: $(tail -1 "$SCR/o0" | cut -c1-80)"; return 1; }
  done
  ( cd "$W" && cc -O0 -Wall -Wextra -Werror -DRDNA4X_TESTHOOKS -o t-xlate test-xlate.c ) >"$SCR/cc.out" 2>&1 || { OUT="test-xlate: compile error: $(grep -m1 error "$SCR/cc.out" | cut -c1-100)"; return 1; }
  ( cd "$W" && TMPDIR="$SCR" ./t-xlate . autotranslate/overrides.txt ) >"$SCR/o0" 2>&1 || { OUT="test-xlate: $(grep -m1 '^FAIL' "$SCR/o0" | cut -c1-110)"; [ -n "${OUT#test-xlate: }" ] || OUT="test-xlate died: $(tail -1 "$SCR/o0" | cut -c1-80)"; return 1; }
  return 0
}
apply() {   # apply <file> <old> <new>  -> exit 3 if the text is not there exactly once
  OLD="$2" NEW="$3" FILE="$1" python3 - <<'PY'
import os,sys
p=os.environ['FILE']; s=open(p).read(); o=os.environ['OLD']; n=os.environ['NEW']
if s.count(o)!=1:
    print("PLANT TEXT NOT FOUND EXACTLY ONCE (%d): %r" % (s.count(o), o[:80])); sys.exit(3)
open(p,'w').write(s.replace(o,n))
PY
}
cplant() {   # cplant <id> <file in the scratch dir> <old> <new> <description>
  local id="$1" file="$2" old="$3" new="$4" desc="$5"
  cfresh; apply "$W/$file" "$old" "$new"; local rc=$?; total=$((total+1))
  if [ $rc -ne 0 ]; then echo "PLANT $id: $desc: THE PLANT ITSELF DID NOT APPLY"; escaped=$((escaped+1)); return; fi
  if c_test; then echo "PLANT $id: $desc: *** ESCAPED (the tests passed) ***"; escaped=$((escaped+1)); else echo "PLANT $id: $desc: CAUGHT by $OUT"; fi
}
cfresh; total=$((total+1)); if c_test; then echo "BASELINE (C): the unmodified scratch copy passes"; else echo "BASELINE (C): FAILED: $OUT"; escaped=$((escaped+1)); fi
M=GarnetDevice.m; L=rdna4_ledger.h; A=rdna4_ioalias.h; I=rdna4_impcache.h; X=rdna4_xlate.h
# ---- P5: the leak ----------------------------------------------------------------------------------------------------------------------------
cplant 1  $M "        else if (_imem) { vkFreeMemory(RDNA4R.dev, _imem, NULL); @synchronized ([GarnetDevice class]) { RDNA4R.impTotal -= _ialloc; } }
    }
    if (_led)" "        else if (_imem) { @synchronized ([GarnetDevice class]) { RDNA4R.impTotal -= _ialloc; } }
    }
    if (_led)" "the texture's dealloc skips vkFreeMemory of its own import (the leak)"
cplant 2  $M "        vkFreeMemory(RDNA4R.dev, (VkDeviceMemory)o.mem, NULL);
        @synchronized ([GarnetDevice class]) { RDNA4R.impTotal -= o.size; }" "        @synchronized ([GarnetDevice class]) { RDNA4R.impTotal -= o.size; }" "the import cache's drain skips vkFreeMemory"
cplant 3  $M "    if (_led) rdna4_led_del(self);   // build 16 (P5 Step 0)
" "" "the texture never leaves the ledger"
cplant 4  $M "    #define RDNA4F1_FALL(...) do { f1 = YES;" "    #define RDNA4F1_FALL(...) do { f1 = YES; if (1) return nil;" "F1: the fallback is removed (a refused import returns nil again)"
cplant 5  $M "goto nobase_path; } while (0)" "return nil; } while (0)" "F1: the fallback jump replaced by nil"
cplant 6  $M "if (rdna4f1_action((int)r) == RDNA4F1_FALLBACK) { _imem = VK_NULL_HANDLE; _impShared = NO; RDNA4F1_FALL(" "if (r != VK_SUCCESS) { _imem = VK_NULL_HANDLE; _impShared = NO; IOSFAIL(84, @\"vkAllocateMemory(import %zu B at %p) = %d\", ialloc, base, r); RDNA4F1_FALL(" "F1: the allocation refusal is a nil again (IOSFAIL 84)"
cplant 7  $M "        if (rdna4_imp_drain()) r = rdna4_import_alloc(ma, &mem);" "        rdna4_imp_drain();" "F1: the refused import no longer retries after reclaim"
cplant 8  $I "static inline int rdna4ic_default_on(int killedStat) { return killedStat ? 0 : 1; }" "static inline int rdna4ic_default_on(int killedStat) { (void)killedStat; return 0; }" "F2: the import cache default flips back to OFF (header)"
cplant 9  $M "RDNA4IC.on = rdna4ic_default_on(access(RDNA4_IMPCACHE_KILL_FILE, F_OK) == 0) ? YES : NO;" "RDNA4IC.on = access(\"/private/tmp/rdna4m-impcache\", F_OK) == 0;" "F2: the latch reads the old presence file again (default OFF)"
cplant 10 $M "if (rdna4f1_inject(rdna4f1_every, &rdna4f1_ctr)) { *out = VK_NULL_HANDLE; return VK_ERROR_OUT_OF_DEVICE_MEMORY; }" "(void)rdna4f1_inject;" "F1: the failure-injection hook is gone (nothing can drive the F1 path)"
cplant 11 $M "rdna4_led_report(tick);   // build 16" "(void)tick;   // build 16" "the ledger is never reported"
# ---- F3: the use count ------------------------------------------------------------------------------------------------------------------------
cplant 12 $M "static void rdna4_uc_dec(void *s, void *c) { (void)c; IOSurfaceDecrementUseCount((IOSurfaceRef)s); CFRelease(s);" "static void rdna4_uc_dec(void *s, void *c) { (void)c; CFRelease(s);" "F3: the decrement is dropped (a surface stays in use forever)"
cplant 13 $L "    for (size_t i = 0; i < k; i++) dec(l->s[i], ctx);
    l->n = 0;" "    (void)dec; (void)ctx; l->n = 0;" "F3: rdna4uc_release never decrements (header)"
cplant 14 $L "    for (size_t i = 0; i < k; i++) dec(l->s[i], ctx);
    l->n = 0;" "    for (size_t i = 0; i < k; i++) dec(l->s[i], ctx);" "F3: rdna4uc_release is not idempotent (header)"
cplant 15 $M "    _ialloc = ialloc; _ibase = (uint8_t *)base + poff; _ioff = poff; _ibpr = bpr; _ios = (IOSurfaceRef)CFRetain(s); rdna4_led_add" "    _ialloc = ialloc; _ibase = (uint8_t *)base + poff; _ioff = poff; _ibpr = bpr; _ios = (IOSurfaceRef)CFRetain(s); IOSurfaceIncrementUseCount(s); rdna4_led_add" "F3: the lifetime hold is restored on the mapped path"
cplant 16 $M "    if (_ios) CFRelease(_ios);   // build 16 (F3)" "    if (_ios) { IOSurfaceDecrementUseCount(_ios); CFRelease(_ios); }   // build 16 (F3)" "F3: the texture dealloc decrements again (lifetime hold)"
cplant 17 $M "- (void)rdna4PoolDone { [self rdna4UCRelease]; [self rdna4OccResolve]; if (_pser) rdna4_fence_close(_pser); }" "- (void)rdna4PoolDone { [self rdna4OccResolve]; if (_pser) rdna4_fence_close(_pser); }" "F3: a completed command buffer does not give its use counts back (only the dealloc would)"
cplant 18 $M "    [self rdna4UCRelease]; rdna4uc_free(&_uc); rdna4ia_destroy(&_ia);   // build 16" "    rdna4uc_free(&_uc); rdna4ia_destroy(&_ia);   // build 16" "F3: the command buffer's dealloc does not give use counts back"
cplant 19 $M "        (void)rdna4uc_take(&_uc, RDNA4LED.ucMode, (void *)[t rdna4UCSurface], rdna4_uc_inc, NULL);" "        (void)0;" "F3: a touch never takes the in-flight hold (the surface would never be protected)"
cplant 20 $L "static inline int rdna4uc_mode(int killedStat) { return killedStat ? RDNA4UC_NONE : RDNA4UC_INFLIGHT; }" "static inline int rdna4uc_mode(int killedStat) { (void)killedStat; return RDNA4UC_INFLIGHT; }" "F3: the kill file is ignored (header)"
cplant 21 $L "    for (size_t i = 0; i < l->n; i++) if (l->s[i] == surface) return 0;" "" "F3: two touches of one surface by a command buffer take two counts (no dedup)"
cplant 22 $L "    if (L->e[i].inflight > 0) L->delInflight++;" "" "ledger: a texture dying while a command buffer still holds it is not noticed"
cplant 23 $L "        if (e->inflight > 0) s->inflightTex++; else s->idle++;" "        s->idle++;" "ledger: textures with a command buffer in flight are counted idle"
cplant 24 $L "    for (size_t i = 0; i < L->n;) { size_t j = i; uint64_t mx = 0; while (j < L->n && t[j].sid == t[i].sid) { if (t[j].bytes > mx) mx = t[j].bytes; j++; } s->distinct++; s->distinctBytes += mx; i = j; }" "    for (size_t i = 0; i < L->n; i++) { s->distinct++; s->distinctBytes += t[i].bytes; }" "ledger: distinct surface ids are not de-duplicated"
# ---- P1: aliasing ------------------------------------------------------------------------------------------------------------------------------
cplant 25 $M "            rdna4_ios_writeback([cb vk], x);   // image -> pages (path (b)), then the host-visibility barrier
" "" "P1: no copy-back of the writer when another wrapper touches the range"
cplant 26 $A "    if (r->writer && r->writer != obj && r->writerB && r->dirty) { d.flush = r->writer; r->dirty = 0; }" "    (void)r->dirty;" "P1: the machine never asks for a flush (header)"
cplant 27 $A "        if (need && !o->valid) { d.upload = 1; d.reupload = !fresh; o->valid = 1; }" "        if (need && fresh) { d.upload = 1; d.reupload = 0; o->valid = 1; }" "P1: no forced re-upload of a wrapper touched before"
cplant 28 $M "            [cb rdna4IOUnwrite:x];             // the end-of-command-buffer write-back must not copy the (now older) image over a later writer
" "" "P1 CLOBBER: the flushed writer is still copied back at the end of the command buffer"
cplant 29 $A "static inline int rdna4ia_enabled(int isWs, int killedStat) { return !isWs && !killedStat; }" "static inline int rdna4ia_enabled(int isWs, int killedStat) { (void)isWs; return !killedStat; }" "P1: WindowServer is let into the gate (header)"
cplant 30 $M "RDNA4LED.iaActs = rdna4ia_enabled(rdna4_is_ws() ? 1 : 0, access(RDNA4_NOIOALIAS_FILE, F_OK) == 0);" "RDNA4LED.iaActs = rdna4ia_enabled(0, access(RDNA4_NOIOALIAS_FILE, F_OK) == 0);" "P1: the glue tells the gate it is never WindowServer"
cplant 31 $A "    for (size_t i = 0; i < a->n; i++) if (a->r[i].sid == sid && a->r[i].off == off) return &a->r[i];" "    for (size_t i = 0; i < a->n; i++) if (a->r[i].sid == sid) return &a->r[i];" "P1: ranges keyed by the surface id only, ignoring the plane offset"
cplant 32 $M "*sid = _ios ? (uint32_t)IOSurfaceGetID(_ios) : 0; *off = (uint64_t)_ioff;" "*sid = _ios ? (uint32_t)IOSurfaceGetID(_ios) : 0; *off = 0;" "P1: the glue keys by the surface id only (plane offset dropped)"
cplant 33 $A "        for (size_t i = 0; i < r->n; i++) if (r->o[i].obj != obj && r->o[i].isB) r->o[i].valid = 0;" "" "P1: a write does not make the other wrappers' images stale"
cplant 34 $M "            rdna4_full_barrier([cb vk]);       // ... and the transfer write must be visible to whatever the GPU does next with those pages
" "" "P1: no barrier after the mid-command-buffer copy-back"
cplant 35 $M "    if (d.flush) {
        RDNA4Texture *x = [cb rdna4IOTex:d.flush];
        if (x && [x rdna4IsIOS]) {
            if (endPass) endPass();" "    if (d.flush) {
        RDNA4Texture *x = [cb rdna4IOTex:d.flush];
        if (x && [x rdna4IsIOS]) {" "P1: the render pass is not ended before the mid-command-buffer copy"
cplant 36 $M "    if (!rdna4_ioalias_acts()) {
        if (d.flush)" "    if (0) {
        if (d.flush)" "P1: the legacy (WindowServer) branch is gone"
# ---- P4 ---------------------------------------------------------------------------------------------------------------------------------------
cplant 37 $X "        if (rdna4x_file_path(done, sizeof done, dir, sha, RDNA4X_EXT_SPV) && lstat(done, &dst) == 0 && S_ISREG(dst.st_mode)) return RDNA4X_OK;" "        (void)done; (void)dst;" "P4: a repeat of an installed sha is translated again"
cplant 38 $M "    if (inproc) RDNA4LOG(\"compute pipeline: not in the bundle's spvcache (%s); trying the in-process translation\", e.localizedDescription.UTF8String);
    else RDNA4LOG(\"compute pipeline creation failed: %s\", e.localizedDescription.UTF8String);" "    RDNA4LOG(\"compute pipeline creation failed: %s\", e.localizedDescription.UTF8String);" "P4: the first-attempt miss is still logged as a failure"
cplant 39 $X "    for (rdna4x_job *j = e->head; j; j = j->next) if (!strcmp(j->sha, sha) && !strcmp(j->dir, dir)) return j;
    if (e->running && !strcmp(e->running->sha, sha) && !strcmp(e->running->dir, dir)) return e->running;
    return NULL;" "    (void)e; (void)dir; (void)sha; return NULL;" "P4: translations in progress are not shared"
echo "test-b16-plant: $total runs (1 baseline + $((total-1)) plants), $escaped escaped or broken"
[ $escaped -eq 0 ]
