#!/usr/bin/env zsh
# test-b13-plant.sh - planted breaks for bundle build 13 (M6 Stage 1b: the monitor B's scanout from the bundle, instance 2): rdna4_m6x.h / test-m6x.c, rdna4_m6route.h (version-2 blob generation), rdna4_dispflip.h (R6), the glue in GarnetDevice.m, rdna4_scanabi.h and the
# kernel headers they mirror. Same discipline as test-b11-plant.sh: copy the REAL files into a scratch tree that keeps the repo's relative layout, apply ONE break (the script proves the text was there exactly once), build and run test-m6x,
# test-m6route and test-dispflip and demand that at least one FAILS (a compile error counts). A plant the tests let through is a hole: exit non-zero.
#   run from anywhere:  src/garnet-metal/test-b13-plant.sh        (TMPDIR selects where the scratch tree goes)
set -u
D="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$D/../.." && pwd)"
SCR="$(mktemp -d "${TMPDIR:-/tmp}/b13-plant.XXXXXX")"
case "$SCR" in */b13-plant.*) ;; *) echo "bad scratch dir $SCR"; exit 2;; esac
trap 'rm -rf -- "$SCR"' EXIT
escaped=0; total=0
W="$SCR/src/garnet-metal"
fresh() {
  rm -rf -- "$SCR/tools" "$SCR/src"; mkdir -p "$W" "$SCR/src/garnet/src/amd"
  cp "$D"/rdna4_m6route.h "$D"/rdna4_m6x.h "$D"/rdna4_scanabi.h "$D"/GarnetDevice.m "$D"/Info.plist "$D"/test-m6x.c "$D"/test-m6route.c "$D"/rdna4_dispflip.h "$D"/rdna4_impcache.h "$D"/test-dispflip.c "$W/"
  python3 "$D/assemble-device.py" "$W/GarnetDevice.m" --include-dir "$D" --output "$W/GarnetDevice.m"
  cp "$ROOT/src/garnet/src/amd/native_m6_pure.h" "$SCR/src/garnet/src/amd/"; cp "$ROOT/src/garnet/src/GarnetNativeABI.h" "$ROOT/src/garnet/src/GarnetDisplayOps.h" "$SCR/src/garnet/src/"; cp "$ROOT/CHANGES.md" "$SCR/"
}
run_test() {   # 0 = all passed; prints the first failure into OUT
  OUT=""
  ( cd "$W" && cc -O1 -D_GNU_SOURCE -Wall -Wextra -Werror -o t-m6x test-m6x.c ) >"$SCR/cc.out" 2>&1 || { OUT="test-m6x: compile error: $(grep -m1 error "$SCR/cc.out" | cut -c1-100)"; return 1; }
  ( cd "$W" && ./t-m6x . ) >"$SCR/o0" 2>&1 || { OUT="test-m6x: $(grep -m1 '^FAIL' "$SCR/o0" | cut -c1-110)"; return 1; }
  ( cd "$W" && cc -O1 -D_GNU_SOURCE -Wall -Wextra -Werror -o t-m6route test-m6route.c ) >"$SCR/cc1.out" 2>&1 || { OUT="test-m6route: compile error: $(grep -m1 error "$SCR/cc1.out" | cut -c1-100)"; return 1; }
  ( cd "$W" && ./t-m6route . ) >"$SCR/o1" 2>&1 || { OUT="test-m6route: $(grep -m1 '^FAIL' "$SCR/o1" | cut -c1-110)"; return 1; }
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
  if run_test; then echo "PLANT $id: $desc: *** ESCAPED (the tests passed) ***"; escaped=$((escaped+1)); else echo "PLANT $id: $desc: CAUGHT by $OUT"; fi
}
fresh; total=$((total+1)); if run_test; then echo "BASELINE: the unmodified scratch copy passes"; else echo "BASELINE: FAILED: $OUT"; escaped=$((escaped+1)); fi
X=src/garnet-metal/rdna4_m6x.h; R=src/garnet-metal/rdna4_m6route.h; M=src/garnet-metal/GarnetDevice.m; A=src/garnet-metal/rdna4_scanabi.h; F=src/garnet-metal/rdna4_dispflip.h; KP=src/garnet/src/amd/native_m6_pure.h
# ---- the routing (the spec's / the contract's bundle plants)
plant 1  $X "    if (!e) { if (tentative) *tentative = 1; return RDNA4X_PLAN_DP; }                 /* unknown: ONLY ever tentatively to instance 0, never to an HDMI instance */" "    if (!e) { if (tentative) *tentative = 1; return rdna4x_on(xon_mask, 2) ? RDNA4X_PLAN_MONB : RDNA4X_PLAN_DP; }" "an UNKNOWN surface is routed to instance 2"
plant 2  $X "    if (e->inst == RDNA4M6_INST_MONB && rdna4x_on(xon_mask, RDNA4X_PLAN_MONB)) return RDNA4X_PLAN_MONB;" "    if (e->inst == RDNA4M6_INST_MONB) return RDNA4X_PLAN_MONB;" "a monitor B surface plans instance 2 even with instance 2 OFF"
plant 3  $X "    if (e->flags & RDNA4M6_ENT_AMBIGUOUS) return RDNA4X_PLAN_SKIP;
    if (e->inst == RDNA4M6_INST_DP) return RDNA4X_PLAN_DP;" "    if (e->inst == RDNA4M6_INST_DP) return RDNA4X_PLAN_DP;" "an ambiguous surface takes a slot"
plant 4  $X "    if ((int)e->inst == slot_inst) return RDNA4X_C_PRESENT;" "    return RDNA4X_C_PRESENT;" "a frame is presented whatever the final instance is (inst_mismatch never drops)"
plant 5  $X "    if (!e || stale) return tries < RDNA4X_REFRESH_TRIES ? RDNA4X_C_RETRY : RDNA4X_C_DROP;" "    if (!e) return tries < RDNA4X_REFRESH_TRIES ? RDNA4X_C_RETRY : RDNA4X_C_DROP;" "a STALE table is trusted at the completion"
plant 6  $X "    if (slot_inst == RDNA4X_PLAN_DP && (e->inst == RDNA4M6_INST_MONA || e->inst == RDNA4M6_INST_MONB) && rdna4x_on(xon_mask, (int)e->inst)) return RDNA4X_C_RECOPY;" "" "a tentative frame that turns out to be the monitor B's is not remembered for the re-copy"
plant 7  $X "    if (e->inst == RDNA4M6_INST_DP || e->inst == RDNA4M6_INST_MONA || e->inst == RDNA4M6_INST_MONB) return RDNA4X_C_MISMATCH;" "    if (e->inst == RDNA4M6_INST_DP || e->inst == RDNA4M6_INST_MONA || e->inst == RDNA4M6_INST_MONB) return RDNA4X_C_PRESENT;" "a mismatch between a DP slot and the monitor B's surface (or the reverse) is presented"
plant 8  $X "static inline int rdna4x_may_present(int plan) { return plan == RDNA4X_C_PRESENT; }" "static inline int rdna4x_may_present(int plan) { return plan == RDNA4X_C_PRESENT || plan == RDNA4X_C_MISMATCH; }" "a mismatch may present"
# ---- the keep-alive and the tick
plant 10 $X "    a |= RDNA4X_T_STATUS;" "" "no keep-alive in the tick plan (a static monitor B would be reverted after 5 s)"
plant 11 $M "            (void)rdna4x_status_locked(xi, &X->st);
            tk.x_active" "            tk.x_active" "no keep-alive in the bundle's tick (the call is gone from the glue)"
plant 12 $M "        if (act & RDNA4X_T_STATUS) {
            X->xs.keepalive_calls++;" "        if (0) {
            X->xs.keepalive_calls++;" "the tick never reaches the keep-alive branch"
plant 13 $X "    if (t->killfile) return RDNA4X_T_RELEASE;" "" "the kill file does not release instance 2"
plant 14 $M "        if (kill && X->enabled) { X->enabled = 0;" "        if (0 && kill && X->enabled) { X->enabled = 0;" "the bundle never polls the kill file in the tick"
plant 15 $X "    if (s->latch && t->status_gen_known && rdna4m6_table_stale(s, t->status_gen)) a |= RDNA4X_T_REFRESH;" "" "the tick never refreshes a table the kernel has republished"
plant 16 $X "    if (t->recopy_pending) a |= RDNA4X_T_RECOPY;" "" "the tick never runs a pending re-copy (R3 bundle half)"
# ---- enable
plant 20 $X "static inline int rdna4x_enabled(const rdna4x_enable_t *e) { return rdna4x_desc(e->inst) != NULL && e->kernel_m6 && e->kernel_m6flip && (e->inst != RDNA4X_INST_MONA || e->kernel_m6flip1) && e->have_call && e->have_handle && !e->killfile; }" "static inline int rdna4x_enabled(const rdna4x_enable_t *e) { return rdna4x_desc(e->inst) != NULL && e->kernel_m6 && (e->inst != RDNA4X_INST_MONA || e->kernel_m6flip1) && e->have_call && e->have_handle && !e->killfile; }" "instance 2 is enabled without the kernel's m6flip latch"
plant 21 $X "static inline int rdna4x_enabled(const rdna4x_enable_t *e) { return rdna4x_desc(e->inst) != NULL && e->kernel_m6 && e->kernel_m6flip && (e->inst != RDNA4X_INST_MONA || e->kernel_m6flip1) && e->have_call && e->have_handle && !e->killfile; }" "static inline int rdna4x_enabled(const rdna4x_enable_t *e) { return rdna4x_desc(e->inst) != NULL && e->kernel_m6 && e->kernel_m6flip && (e->inst != RDNA4X_INST_MONA || e->kernel_m6flip1) && e->have_call && e->have_handle; }" "instance 2 is enabled with the kill file present"
plant 22 $X "static inline int rdna4x_enabled(const rdna4x_enable_t *e) { return rdna4x_desc(e->inst) != NULL && e->kernel_m6 && e->kernel_m6flip && (e->inst != RDNA4X_INST_MONA || e->kernel_m6flip1) && e->have_call && e->have_handle && !e->killfile; }" "static inline int rdna4x_enabled(const rdna4x_enable_t *e) { return rdna4x_desc(e->inst) != NULL && e->kernel_m6 && e->kernel_m6flip && (e->inst != RDNA4X_INST_MONA || e->kernel_m6flip1) && e->have_call && !e->killfile; }" "instance 2 is enabled without the bo_handle export"
plant 23 $X "static inline int rdna4x_surface_geom_ok(uint32_t inst, uint32_t w, uint32_t h, uint32_t bpr) { const rdna4x_desc_t *d = rdna4x_desc(inst); return d && w == d->w && h == d->h && bpr == d->pitch_bytes; }" "static inline int rdna4x_surface_geom_ok(uint32_t inst, uint32_t w, uint32_t h, uint32_t bpr) { (void)inst; (void)w; (void)h; (void)bpr; return 1; }" "any surface geometry passes the instance size gate"
plant 24 $M "            RDNA4XI(xi).enabled = rdna4x_enabled(&xe);" "            RDNA4XI(xi).enabled = 1;" "the glue enables instance 2 without asking rdna4x_enabled"
plant 25 $M "(q.flags & RDNA4X_SCANQ_M6FLIP) != 0" "(q.flags & RDNA4N_SCANQ_M6) != 0" "the glue reads the wrong kernel flag for m6flip"
# ---- the pool budget and the acquire driver
plant 30 $X "{ return (uint64_t)p->slots * p->slot_bytes + p->margin_bytes; }" "{ return p->margin_bytes; }" "S4: the pool budget forgets the monitor B's slots"
plant 31 $X "    return rdna4x_pool_need(p) <= p->free_bytes ? RDNA4X_POOL_OK : RDNA4X_POOL_TOO_SMALL;" "    return RDNA4X_POOL_OK;" "the pool verdict is always OK"
plant 32 $X "    if (o->pool_verdict(o->ctx) != RDNA4X_POOL_OK) { o->off(o->ctx, RDNA4DF_R_ALLOC, \"the free visible-VRAM figure does not cover this display's slots\"); return RDNA4DF_R_ALLOC; }" "" "the acquire driver ignores the pool verdict"
plant 33 $X "        if (o->alloc_slot(o->ctx, i) != 0 || o->register_slot(o->ctx, i) != 0) { o->off(o->ctx, RDNA4DF_R_ALLOC, \"a slot could not be allocated or registered\"); o->free_slots(o->ctx); return RDNA4DF_R_ALLOC; }" "        if (o->alloc_slot(o->ctx, i) != 0 || o->register_slot(o->ctx, i) != 0) { o->free_slots(o->ctx); return RDNA4DF_R_ALLOC; }" "a failed slot allocation does not fail instance 2 closed (it stays acquired)"
plant 34 $X "o->off(o->ctx, RDNA4DF_R_ALLOC, \"a slot could not be allocated or registered\"); o->free_slots(o->ctx); return RDNA4DF_R_ALLOC; }" "o->off(o->ctx, RDNA4DF_R_ALLOC, \"a slot could not be allocated or registered\"); return RDNA4DF_R_ALLOC; }" "a failed slot allocation leaks the slots already made"
plant 35 $X "    o->mark_acquired(o->ctx);
" "" "the driver never marks the plane acquired (a later failure would not release it)"
plant 36 $X "    if (o->k_acquire(o->ctx) != 0) { o->off(o->ctx, RDNA4DF_R_ERROR, \"SCANX_ACQUIRE refused\"); return RDNA4DF_R_ERROR; }" "    (void)o->k_acquire(o->ctx);" "a refused SCANX_ACQUIRE is ignored"
plant 38 $M "    if (RDNA4S.sm.state != RDNA4DF_ACTIVE) { rdna4x_off_locked(inst, RDNA4DF_R_ERROR, \"instance 0 (the DP) is not active: the kernel requires the session to hold it first\"); return NO; }" "" "instance 2 is acquired without the DP being active"
plant 39 $M "    return rdna4x_acquire_run(&ops) == 0;" "    rdna4s_off_locked(RDNA4DF_R_ALLOC, \"monb\"); return rdna4x_acquire_run(&ops) == 0;" "an instance-2 failure also fails the DP closed"
# ---- the glue: the instance of a frame
plant 40 $M "        pi = rdna4x_plan_inst(&RDNA4S.m6, sid, xon, &tent);" "        pi = xon ? RDNA4X_PLAN_MONB : RDNA4X_PLAN_DP; (void)tent;" "the gate plans from the enable flag instead of the table"
plant 41 $M "        if (pi == RDNA4X_PLAN_SKIP) { pthread_mutex_unlock(&RDNA4S.mu); return NO; }" "" "a SKIP plan (monitor A / ambiguous / monitor B with instance 2 OFF) still takes a slot"
plant 42 $M "int sl = take ? rdna4s_plan(dwr, pinst, ptent, &pl) : -1;" "int sl = take ? rdna4s_plan(dwr, 0, ptent, &pl) : -1;" "every frame is planned on the DP's slots"
plant 43 $M "_dslot = sl; _dinst = pl.inst;" "_dslot = sl; _dinst = 0;" "the command buffer forgets that its slot is the monitor B's (the completion presents it on the DP)"
plant 44 $M "rdna4s_complete(_dinst, s, r, _dseq, _dsid, rec, _dcls, _tcommit);" "rdna4s_complete(0, s, r, _dseq, _dsid, rec, _dcls, _tcommit);" "the completion is always told instance 0"
plant 45 $M "            const int plan = rdna4x_complete_plan(&RDNA4S.m6, sid, inst, tries, stale, xon);" "            const int plan = RDNA4X_C_PRESENT;" "the completion never asks the plan"
plant 46 $M "            int stale = 0; for (int xi = 1; xi <= 2; xi++) if (RDNA4XI(xi).sm.state == RDNA4DF_ACTIVE && rdna4m6_table_stale(&RDNA4S.m6, RDNA4XI(xi).gen_known ? RDNA4XI(xi).gen : 0u)) stale = 1;   // any ACTIVE HDMI instance has seen a newer table than the cache" "            int stale = 0;" "the completion never considers the table stale"
plant 47 $M "            int rc = xinst ? rdna4x_k_present(inst, sids[slot], o) : RDNA4S.fp(RDNA4R.dev, sids[slot], o);" "            int rc = RDNA4S.fp(RDNA4R.dev, sids[slot], o);" "a monitor B frame is presented through the DP's selector"
plant 48 $M "                if (plan == RDNA4X_C_MISMATCH || plan == RDNA4X_C_RECOPY) cx->xs.inst_mismatch++;" "" "inst_mismatch is never counted"
plant 50 $M "static void rdna4x_recopy_locked(int inst) {
    rdna4xi_t *const X = &RDNA4XI(inst); const rdna4x_desc_t *const d = rdna4x_desc((uint32_t)inst);" "static void rdna4x_recopy_locked(int inst) { return;
    rdna4xi_t *const X = &RDNA4XI(inst); const rdna4x_desc_t *const d = rdna4x_desc((uint32_t)inst);" "the re-copy never runs"
plant 51 $M "    rdna4df_t *const am = apx ? &RDNA4XI(pi).sm : &RDNA4S.sm;   // the machine of the planned instance accounts the frame (an HDMI machine is a full-copy machine)" "    rdna4df_t *const am = &RDNA4S.sm;" "the monitor B's frame is accounted on the DP's machine"
plant 52 $M "    if (wr && !apx) rdna4df_dmg_account(&RDNA4S.sm, c, wr, W, H);" "    if (wr) rdna4df_dmg_account(&RDNA4S.sm, c, wr, W, H);" "a monitor B frame feeds the DP's damage accounting"
plant 53 $M "            if (RDNA4S.m6.latch && rdna4m6_table_stale(&RDNA4S.m6, X->gen)) rdna4s_m6_refresh_async();" "" "the monitor B plan never asks for a refresh of a stale table"
plant 54 $M "        if (rdna4s_crc_on() && pinst == RDNA4X_PLAN_DP) { _dcrc = YES; _dtex = dt; }" "        if (rdna4s_crc_on()) { _dcrc = YES; _dtex = dt; }" "the CRC diagnostic runs on monitor B frames"
plant 55 $M "    rdna4x_release_all_locked();   // bundle 13/15: the HDMI instances' A go back with the DP's console" "    // no x release" "the DP's release does not give the monitor B's A back"
plant 56 $M "            if (xinst) rdna4x_do_release_locked(inst); else rdna4s_do_release_locked();" "            rdna4s_do_release_locked();" "a failed monitor B present releases the DP instead of instance 2"
# ---- R2 / R6 / scanabi
plant 60 $R "    if (ver == RDNA4M6_BLOB_VERSION2 && s->have_gen && (int32_t)(gen - s->gen) < 0) { s->store_refused++; return -2; }" "" "the parser stores an OLDER generation over a newer one"
plant 61 $R "    if (src->have_gen && dst->have_gen && (int32_t)(src->gen - dst->gen) < 0) { dst->store_refused++; return -2; }" "" "rdna4m6_store stores an older read over a newer one"
plant 62 $R "static inline int rdna4m6_table_stale(const rdna4m6_t *s, uint32_t status_gen) { return status_gen != 0u && (!s->have_gen || (int32_t)(status_gen - s->gen) > 0); }" "static inline int rdna4m6_table_stale(const rdna4m6_t *s, uint32_t status_gen) { (void)s; (void)status_gen; return 0; }" "a table older than the published generation is never stale"
plant 63 $R "(int32_t)(status_gen - s->gen) > 0); }" "(int32_t)(status_gen - s->gen) < 0); }" "the stale comparison points the wrong way"
plant 64 $R "    const uint32_t gen = ver == RDNA4M6_BLOB_VERSION2 ? rdna4m6_rd32(b + 8) : 0u;" "    const uint32_t gen = 0u;" "the parser drops the blob's generation"
plant 65 $R "    if (ver != RDNA4M6_BLOB_VERSION && ver != RDNA4M6_BLOB_VERSION2) return -1;" "    if (ver != RDNA4M6_BLOB_VERSION) return -1;" "the parser refuses version 2"
plant 66 $M "if (rc == 0) { if (rdna4m6_store(&RDNA4S.m6, &tmp) != 0 && RDNA4S.m6.store_refused <= 3) RDNA4LOG(\"m6: an OLDER table read (generation %u < cached %u) was not stored (out-of-order refresh)\", tmp.gen, RDNA4S.m6.gen); }   // bundle 13 (R2): never store an older generation over a newer one" "if (rc == 0) { memcpy(RDNA4S.m6.e, tmp.e, sizeof tmp.e); RDNA4S.m6.n = tmp.n; RDNA4S.m6.have_table = 1; }" "the synchronous refresh path stores unconditionally"
plant 67 $F "    if (tentative) { s->head = -1; s->head_id = 0; } else { s->head = slot; s->head_id = p->id; }" "    s->head = slot; s->head_id = p->id;" "a TENTATIVE frame becomes the chain head (R6)"
plant 68 $F "    if (tentative) kind = RDNA4DF_K_FULL;" "" "a TENTATIVE frame may be a partial copy (R6)"
plant 69 $M "        k = rdna4x_kind_for(tent, RDNA4DF_K_FULL, k);   // bundle 13 (R6): a TENTATIVE frame is a full copy" "" "the glue does not force a tentative frame to a full copy"
plant 70 $M "        slot = rdna4df_plan_ex(&RDNA4S.sm, fl, k, r, tent, pl);   // ... and never becomes the chain head" "        slot = rdna4df_plan(&RDNA4S.sm, fl, k, r, pl);" "the glue plans a tentative frame like any other"
plant 71 $A "#define RDNA4N_SCANQ_M6FLIP     (1u << 6)" "#define RDNA4N_SCANQ_M6FLIP     (1u << 7)" "the bundle's copy of the m6flip flag differs from the kernel's"
plant 72 $A "#define RDNA4N_SEL_SCANX_PRESENT  24u" "#define RDNA4N_SEL_SCANX_PRESENT  25u" "the bundle's copy of a selector number differs from the kernel's"
plant 74 src/garnet-metal/Info.plist "<string>43</string>" "<string>12</string>" "the bundle still says build 12"
plant 80 $X "    if (p->free_bytes == 0 || p->slot_bytes == 0 || p->slots == 0) return RDNA4X_POOL_NO_FIGURE;" "    if (p->slot_bytes == 0 || p->slots == 0) return RDNA4X_POOL_NO_FIGURE;" "S4: an unreported free-VRAM figure (0) is treated as a figure"
plant 81 $X "{ return (uint64_t)p->slots * p->slot_bytes + p->margin_bytes; }" "{ return (uint64_t)p->slots * p->slot_bytes; }" "S4: the margin is dropped from the budget"
plant 82 $M ".free_bytes = a->ao[3]" ".free_bytes = a->ao[0]" "S4: the glue budgets from the wrong word (not the kernel's free-VRAM figure)"
plant 83 $M "const uint64_t in[2] = { (uint64_t)inst, 0 }; uint32_t no = 5;" "const uint64_t in[2] = { (uint64_t)inst, 0 }; uint32_t no = 3;" "S4: the acquire call asks for 3 outputs (the free-VRAM figure is not returned)"
plant 84 $X "    o->mark_acquired(o->ctx);
    if (o->geom_verdict(o->ctx) != 0) { o->off(o->ctx, RDNA4DF_R_GEOM, \"the kernel's plane geometry for this instance is not the descriptor's\"); return RDNA4DF_R_GEOM; }
    if (o->pool_verdict" "    if (o->geom_verdict(o->ctx) != 0) { o->off(o->ctx, RDNA4DF_R_GEOM, \"the kernel's plane geometry for this instance is not the descriptor's\"); return RDNA4DF_R_GEOM; }
    if (o->pool_verdict" "S4: the pool verdict is not preceded by the acquire mark (an OFF after the verdict would not release)"
plant 75 $KP "kBlobVersion2 = 2u, kBlobHdr2 = 12u" "kBlobVersion2 = 2u, kBlobHdr2 = 16u" "the kernel's version-2 header size changes without the bundle"
echo "test-b13-plant: $total run (baseline included), $escaped escaped or did not apply"
exit $((escaped != 0))
