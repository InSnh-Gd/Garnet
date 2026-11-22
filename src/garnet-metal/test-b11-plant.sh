#!/usr/bin/env zsh
# test-b11-plant.sh - planted breaks for bundle build 11 (M6 Stage 1a: which display a display surface belongs to): rdna4_m6route.h / test-m6route.c, the glue in GarnetDevice.m, rdna4_scanabi.h, the kernel header it mirrors.
# For each plant: copy the REAL files into a scratch tree that keeps the repo's relative layout (the test reads ../../src/garnet/src/...), apply ONE break (the script first proves the text was there exactly once),
# build and run test-m6route and demand that it FAILS (a compile error counts). A plant the test lets through is a hole: exit non-zero.
#   run from anywhere:  src/garnet-metal/test-b11-plant.sh
set -u
D="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$D/../.." && pwd)"
SCR="$(mktemp -d "${TMPDIR:-/tmp}/b11-plant.XXXXXX")"
case "$SCR" in */b11-plant.*) ;; *) echo "bad scratch dir $SCR"; exit 2;; esac
trap 'rm -rf -- "$SCR"' EXIT
escaped=0; total=0
W="$SCR/src/garnet-metal"
fresh() {
  rm -rf -- "$SCR/tools" "$SCR/src"; mkdir -p "$W" "$SCR/src/garnet/src/amd"
  cp "$D"/rdna4_m6route.h "$D"/rdna4_scanabi.h "$D"/GarnetDevice.m "$D"/Info.plist "$D"/test-m6route.c "$D"/rdna4_dispflip.h "$D"/rdna4_impcache.h "$D"/test-dispflip.c "$W/"
  python3 "$D/assemble-device.py" "$W/GarnetDevice.m" --include-dir "$D" --output "$W/GarnetDevice.m"
  cp "$ROOT/src/garnet/src/amd/native_m6_pure.h" "$SCR/src/garnet/src/amd/"; cp "$ROOT/src/garnet/src/GarnetNativeABI.h" "$ROOT/src/garnet/src/GarnetDisplayOps.h" "$SCR/src/garnet/src/"; cp "$ROOT/CHANGES.md" "$SCR/"
}
run_test() {   # 0 = passed; prints the first failure into OUT
  OUT=""
  ( cd "$W" && cc -O1 -D_GNU_SOURCE -Wall -Wextra -Werror -o t-m6route test-m6route.c ) >"$SCR/cc.out" 2>&1 || { OUT="test-m6route: compile error: $(grep -m1 error "$SCR/cc.out" | cut -c1-100)"; return 1; }
  ( cd "$W" && ./t-m6route . ) >"$SCR/o1" 2>&1 || { OUT="test-m6route: $(grep -m1 '^FAIL' "$SCR/o1" | cut -c1-110)"; return 1; }
  # bundle 12: the state machine's skip (rdna4df_skip_content) is exercised by test-dispflip.c
  ( cd "$W" && cc -O1 -Wall -Wextra -Werror -o t-dispflip test-dispflip.c ) >"$SCR/cc2.out" 2>&1 || { OUT="test-dispflip: compile error: $(grep -m1 error "$SCR/cc2.out" | cut -c1-100)"; return 1; }
  ( cd "$W" && ./t-dispflip ) >"$SCR/o2" 2>&1 || { OUT="test-dispflip: $(grep -m1 '^FAIL' "$SCR/o2" | cut -c1-110)"; return 1; }
  return 0
}
plant() {   # plant <id> <file relative to the scratch root> <old> <new> <description>
  local id="$1" file="$2" old="$3" new="$4" desc="$5"
  fresh
  OLD="$old" NEW="$new" FILE="$SCR/$file" python3 - <<'PY'
import os,sys
p=os.environ['FILE']; s=open(p).read(); o=os.environ['OLD']; n=os.environ['NEW']
if s.count(o)!=1:
    print("PLANT TEXT NOT FOUND EXACTLY ONCE (%d): %r" % (s.count(o), o[:80])); sys.exit(3)
open(p,'w').write(s.replace(o,n))
PY
  local rc=$?; total=$((total+1))
  if [ $rc -ne 0 ]; then echo "PLANT $id: $desc: THE PLANT ITSELF DID NOT APPLY"; escaped=$((escaped+1)); return; fi
  if run_test; then echo "PLANT $id: $desc: *** ESCAPED (the test passed) ***"; escaped=$((escaped+1)); else echo "PLANT $id: $desc: CAUGHT by $OUT"; fi
}
fresh; total=$((total+1)); if run_test; then echo "BASELINE: the unmodified scratch copy passes"; else echo "BASELINE: FAILED: $OUT"; escaped=$((escaped+1)); fi
R=src/garnet-metal/rdna4_m6route.h; M=src/garnet-metal/GarnetDevice.m; A=src/garnet-metal/rdna4_scanabi.h; KP=src/garnet/src/amd/native_m6_pure.h
plant 1  $R "    if (!s->latch) return RDNA4M6_PRESENT;                                      /* build 10 */" "    if (0) return RDNA4M6_PRESENT;" "the gate ignores the latch: a latch-OFF boot skips surfaces (build 10 broken)"
plant 2  $R "    if (e->inst != RDNA4M6_INST_DP) { s->gate_skip[e->inst]++; return RDNA4M6_SKIP_OTHER; }" "    if (0) { s->gate_skip[e->inst]++; return RDNA4M6_SKIP_OTHER; }" "the gate lets a monitor B surface take a slot (the misroute this build exists to stop)"
plant 3  $R "    if (e->flags & RDNA4M6_ENT_AMBIGUOUS) { s->gate_ambig++; return RDNA4M6_SKIP_AMBIG; }" "" "the gate lets an ambiguous surface take a slot"
plant 4  $R "    if (e->inst != RDNA4M6_INST_DP) return RDNA4M6_SKIP_OTHER;
    return RDNA4M6_PRESENT;
}
static inline int rdna4m6_final_count" "    return RDNA4M6_PRESENT;
}
static inline int rdna4m6_final_count" "the completion presents a surface mapped to another display"
plant 5  $R "    if (!e) return RDNA4M6_SKIP_UNKNOWN;
    if (e->flags & RDNA4M6_ENT_AMBIGUOUS) return RDNA4M6_SKIP_AMBIG;" "    if (!e) return RDNA4M6_PRESENT;
    if (e->flags & RDNA4M6_ENT_AMBIGUOUS) return RDNA4M6_SKIP_AMBIG;" "the completion presents a surface the kernel has not seen"
plant 6  $R "    if (!e) return RDNA4M6_SKIP_UNKNOWN;
    if (e->flags & RDNA4M6_ENT_AMBIGUOUS) return RDNA4M6_SKIP_AMBIG;" "    if (!e) return RDNA4M6_SKIP_UNKNOWN;" "the completion presents an ambiguous surface"
plant 7  $R "static inline int rdna4m6_may_present(int v) { return v == RDNA4M6_PRESENT; }" "static inline int rdna4m6_may_present(int v) { return v == RDNA4M6_PRESENT || v == RDNA4M6_TENTATIVE; }" "a tentative verdict may present"
plant 8  $R "    if (ver != RDNA4M6_BLOB_VERSION && ver != RDNA4M6_BLOB_VERSION2) return -1;" "" "the parser accepts any blob version"
plant 9  $R "if (n > RDNA4M6_MAX_SURF || len != (size_t)hdr + (size_t)n * RDNA4M6_BLOB_ENT) return -1;" "if (n > RDNA4M6_MAX_SURF) return -1;" "the parser accepts a blob of the wrong length"
plant 10 $R "if (tmp[i].id == 0 || tmp[i].inst >= RDNA4M6_NINST) return -1;" "if (tmp[i].id == 0) return -1;" "the parser accepts an entry on an instance that does not exist"
plant 11 $R "if (n > RDNA4M6_MAX_SURF || len" "if (len" "the parser accepts a count above the table (and overruns its copy)"
plant 12 $R "#define RDNA4M6_RETRIES   8" "#define RDNA4M6_RETRIES   0" "an unknown surface is never refreshed at the completion"
plant 13 $R "#define RDNA4M6_SCANQ_FLAG   (1u << 5)" "#define RDNA4M6_SCANQ_FLAG   (1u << 6)" "the bundle's copy of the latch flag differs from the kernel's"
plant 14 $A "#define RDNA4N_SCANQ_M6         (1u << 5)" "#define RDNA4N_SCANQ_M6         (1u << 6)" "the scan_query flag in the ABI copy moves"
plant 15 $KP "kBlobVersion = 1u, kBlobHdr = 8u, kBlobEnt = 12u" "kBlobVersion = 1u, kBlobHdr = 8u, kBlobEnt = 16u" "the kernel's blob entry size changes without the bundle"
plant 16 $KP '#define RDNA4M6_PROP_SURF    "Garnet,M6Surf"' '#define RDNA4M6_PROP_SURF    "Garnet,M6Surfaces"' "the kernel names the property differently"
plant 17 $M "        pi = rdna4x_plan_inst(&RDNA4S.m6, sid, xon, &tent);" "        pi = RDNA4X_PLAN_DP; (void)tent; (void)xon;" "the glue never asks the gate"
plant 18 $M "        if (pi == RDNA4X_PLAN_SKIP) { pthread_mutex_unlock(&RDNA4S.mu); return NO; }" "        (void)pi;" "the glue ignores the gate's verdict"
plant 19 $M "            const int plan = rdna4x_complete_plan(&RDNA4S.m6, sid, inst, tries, stale, xon);" "            const int plan = RDNA4X_C_PRESENT;" "the completion never asks the plan (the monitor B's frames are presented on the DP)"
plant 20 $R "    return RDNA4M6_C_SKIP;
}" "    return RDNA4M6_C_PRESENT;
}" "the plan presents a surface mapped to another display or ambiguous"
plant 21 $M "        if (hok && RDNA4S.m6.latch) (void)rdna4m6_final_count(&RDNA4S.m6, sid, RDNA4M6_PRESENT);   // bundle 12: the verdict was decided above (a presented frame is counted once, at its present)
" "" "a presented frame is never counted"
plant 22 $M "    RDNA4S.m6.latch = (q.flags & RDNA4N_SCANQ_M6) != 0;" "    RDNA4S.m6.latch = 1;" "the latch is forced on whatever the kernel says"
plant 23 $M "    RDNA4S.m6.latch = (q.flags & RDNA4N_SCANQ_M6) != 0;" "    RDNA4S.m6.latch = 0;" "the latch is forced off whatever the kernel says (the monitor B is flipped onto the DP)"
plant 24 $M "    if (self && rdna4_is_ws() && !rdna4_acc_port) rdna4_acc_port = port;" "    if (self && !rdna4_acc_port) rdna4_acc_port = port;" "any process records the accelerator port"
plant 25 $M "    pthread_mutex_unlock(&RDNA4S.mu);
    rdna4m6_t tmp; memset(&tmp, 0, sizeof tmp);
    const int rc = rdna4s_m6_read(&tmp);
    pthread_mutex_lock(&RDNA4S.mu);" "    rdna4m6_t tmp; memset(&tmp, 0, sizeof tmp);
    const int rc = rdna4s_m6_read(&tmp);" "the registry read runs with the scanout mutex held"
plant 26 $M "    RDNA4LOG(\"m6: surfaces for instance 1/2: %u/%u" "    RDNA4LOG(\"m6: surfaces: %u/%u" "the counter log loses 'surfaces for instance 1/2'"
plant 27 $M "            ok = size && pitch && (bt || test);" "            ok = pitch && (bt || test);" "the display-surface classifier changes (the size match is dropped)"
plant 28 $M "        if (rc == 0) { if (rdna4m6_store(&RDNA4S.m6, &tmp) != 0 && RDNA4S.m6.store_refused <= 3) RDNA4LOG(\"m6: an OLDER table read (generation %u < cached %u) was not stored (out-of-order refresh)\", tmp.gen, RDNA4S.m6.gen); }   // bundle 13 (R2)" "        if (rc == 0) { RDNA4S.m6.n = tmp.n; RDNA4S.m6.have_table = 1; }" "the async refresh forgets to copy the entries"
plant 29 $M "    rdna4_m6_report();   // bundle 11: one line when the kernel latch garnet-m6 is ON (nothing when OFF)
" "" "the T1 report never prints the m6 line"
plant 30 src/garnet-metal/Info.plist "<string>20</string>" "<string>11</string>" "the bundle still says build 11"
plant 31 src/garnet-metal/INSTALL.md "Build 11 (CFBundleVersion 11," "Build 13 (CFBundleVersion 11," "INSTALL.md loses build 11"
plant 32 $M "    static int lg; if (lg < 12) { lg++; RDNA4LOG(\"m6: frame for IOSurface" "    static int lg; if (0 && lg < 12) { lg++; RDNA4LOG(\"m6: frame for IOSurface" "a skipped frame is never logged"
# ---- bundle 12 (queue 290 S3) -------------------------------------------------------------------------------------------------------------------------------------------------------
DF=src/garnet-metal/rdna4_dispflip.h
plant 33 $R "    if (v == RDNA4M6_SKIP_UNKNOWN) return tries < RDNA4M6_RETRIES ? RDNA4M6_C_RETRY : RDNA4M6_C_DROP;" "    if (v == RDNA4M6_SKIP_UNKNOWN) return RDNA4M6_C_DROP;" "an unknown ID is never retried (the first frame of every surface is lost)"
plant 34 $R "    if (v == RDNA4M6_SKIP_UNKNOWN) return tries < RDNA4M6_RETRIES ? RDNA4M6_C_RETRY : RDNA4M6_C_DROP;" "    if (v == RDNA4M6_SKIP_UNKNOWN) return RDNA4M6_C_RETRY;" "an unknown ID is retried forever (the slot is held for ever)"
plant 35 $R "RDNA4M6_C_RETRY : RDNA4M6_C_DROP;" "RDNA4M6_C_RETRY : RDNA4M6_C_PRESENT;" "an ID still unknown after the last try is PRESENTED (fail open)"
plant 37 $M "            const int plan = rdna4x_complete_plan(&RDNA4S.m6, sid, inst, tries, stale, xon);" "            (void)rdna4df_complete_held(sm, slot, 0, seq, sid, 0, 0, tcommit, 0, NULL, NULL, NULL);
            const int plan = rdna4x_complete_plan(&RDNA4S.m6, sid, inst, tries, stale, xon);" "ORDER: the state machine releases the slot BEFORE the M6 verdict (build 11's bug)"
plant 38 $M "                rdna4s_m6_refresh_async();
                pthread_mutex_unlock(&RDNA4S.mu);" "                struct timespec ts_ = { 0, 3000000 }; nanosleep(&ts_, NULL);
                rdna4s_m6_refresh_async();
                pthread_mutex_unlock(&RDNA4S.mu);" "a sleep returns to the present path"
plant 39 $M "    rdna4df_skip_content(sm, slot);
    static int lg;" "    rdna4df_release_slot(sm, slot);
    static int lg;" "a skipped frame releases its slot like a plain frame (no chain restart, no poison)"
plant 40 $M "                dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)RDNA4M6_RETRY_NS), RDNA4S.pq, ^{ rdna4s_complete_run(inst, slot, r, seq, sid, rec, cls, tcommit, tries + 1); });" "                rdna4s_complete_run(inst, slot, r, seq, sid, rec, cls, tcommit, tries + 1);" "the retry recurses at once instead of queueing"
plant 41 $M "    if (atomic_exchange(&rdna4s_m6_refreshing, 1)) return;" "" "the async refresh is not limited to one in flight"
plant 42 $M "        const int rc = rdna4s_m6_read(&tmp);
        pthread_mutex_lock(&RDNA4S.mu);
        RDNA4S.m6.refreshes++;" "        pthread_mutex_lock(&RDNA4S.mu);
        const int rc = rdna4s_m6_read(&tmp);
        RDNA4S.m6.refreshes++;" "the async refresh reads the registry with the scanout mutex held"
plant 43 $M "                rdna4s_crc_free(rec);
                return;
            }
        }
        uint64_t stale0" "                return;
            }
        }
        uint64_t stale0" "a skipped frame leaks its CRC record"
plant 44 $DF "    s->m6_skip++; s->head = -1;" "    s->m6_skip++;" "skip_content leaves the chain head on the foreign slot"
plant 45 $DF "    if (myid) for (int j = 0; j < RDNA4DF_MAX_SLOTS; j++) if (s->inflight[j] && s->fid[j] > myid) s->poison[j] = 1;
    s->m6_skip++;" "    s->m6_skip++;" "skip_content does not poison later chained frames that read the slot"
plant 46 $DF "    const uint64_t myid = s->fid[slot];
    rdna4df_release_slot(s, slot);
" "    const uint64_t myid = s->fid[slot];
" "skip_content never releases the slot"
plant 47 $DF "    s->m6_skip++; s->head = -1;" "    s->m6_skip++; s->head = -1; s->last_seq = myid;" "skip_content moves last_seq (an older real frame would be stale)"
plant 48 $DF "    s->m6_skip++; s->head = -1;" "    s->m6_skip++; s->head = -1; s->gpu_fail++;" "skip_content counts a GPU failure"
plant 49 $DF "static inline void rdna4df_skip_content(rdna4df_t *s, int slot) {
    if (slot < 0 || slot >= RDNA4DF_MAX_SLOTS || !s->inflight[slot]) return;" "static inline void rdna4df_skip_content(rdna4df_t *s, int slot) {
    if (slot < 0 || slot >= RDNA4DF_MAX_SLOTS) return;" "skip_content acts on a slot that is not in flight"
echo "test-b11-plant: $((total - 1)) plants (+ the baseline), $escaped escaped or did not apply"
[ $escaped -eq 0 ]
