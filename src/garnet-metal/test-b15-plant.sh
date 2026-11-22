#!/usr/bin/env zsh
# test-b15-plant.sh - planted breaks for bundle build 15 (M6 Stage 2: the monitor A as the third GPU-composited display, instance 1; the per-instance state array [DP, monitor A, monitor B]): rdna4_m6x.h / test-m6x.c, rdna4_m6route.h (version-2 blob generation), rdna4_dispflip.h (R6), the glue in GarnetDevice.m, rdna4_scanabi.h and the
# kernel headers they mirror. Same discipline as test-b11-plant.sh: copy the REAL files into a scratch tree that keeps the repo's relative layout, apply ONE break (the script proves the text was there exactly once), build and run test-m6x,
# test-m6route and test-dispflip and demand that at least one FAILS (a compile error counts). A plant the tests let through is a hole: exit non-zero.
#   run from anywhere:  src/garnet-metal/test-b15-plant.sh        (TMPDIR selects where the scratch tree goes)
set -u
D="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$D/../.." && pwd)"
SCR="$(mktemp -d "${TMPDIR:-/tmp}/b15-plant.XXXXXX")"
case "$SCR" in */b15-plant.*) ;; *) echo "bad scratch dir $SCR"; exit 2;; esac
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
X=src/garnet-metal/rdna4_m6x.h; M=src/garnet-metal/GarnetDevice.m; A=src/garnet-metal/rdna4_scanabi.h
# ---- THE INSTANCE-SWAP PLANTS (bundle side) and the per-instance rules
plant 1  $X "    if (e->inst == RDNA4M6_INST_MONA && rdna4x_on(xon_mask, RDNA4X_PLAN_MONA)) return RDNA4X_PLAN_MONA;
    if (e->inst == RDNA4M6_INST_MONB && rdna4x_on(xon_mask, RDNA4X_PLAN_MONB)) return RDNA4X_PLAN_MONB;" "    if (e->inst == RDNA4M6_INST_MONB && rdna4x_on(xon_mask, RDNA4X_PLAN_MONA)) return RDNA4X_PLAN_MONA;
    if (e->inst == RDNA4M6_INST_MONA && rdna4x_on(xon_mask, RDNA4X_PLAN_MONB)) return RDNA4X_PLAN_MONB;" "S6: the table's MONA and MONB are swapped in the plan (RDNA4M6_INST_MONA <-> MONB)"
plant 2  $X "    { RDNA4X_INST_MONA, 0x10u, 1920u, 1080u, 7680u,  8323072ull,  RDNA4X_NOFLIP1_FILE, \"monitor A\" }," "    { RDNA4X_INST_MONA, 0x10u, 2560u, 1440u, 10240u, 14745600ull, RDNA4X_NOFLIP1_FILE, \"monitor A\" }," "S8: the monitor A's descriptor carries the DP's / monitor B's rectangle and slot size"
plant 3  $M "    VkDeviceSize all = xd ? (VkDeviceSize)rdna4x_frame_bytes(xd) : (VkDeviceSize)RDNA4S.pitch * RDNA4S.ph;" "    VkDeviceSize all = (VkDeviceSize)RDNA4S.pitch * RDNA4S.ph;" "S8: the monitor A's copy uses the DP's rectangle (a 14.7 MB copy into an 8.3 MB slot)"
plant 4  $M "    const uint32_t gw = xd ? xd->w : RDNA4S.pw, gh = xd ? xd->h : RDNA4S.ph, gp = xd ? xd->pitch_bytes : RDNA4S.pitch;" "    const uint32_t gw = RDNA4S.pw, gh = RDNA4S.ph, gp = RDNA4S.pitch;" "S8: the image copy uses the DP's geometry for an HDMI instance"
plant 5  $X "    { RDNA4X_INST_MONA, 0x10u, 1920u, 1080u, 7680u,  8323072ull,  RDNA4X_NOFLIP1_FILE, \"monitor A\" },
    { RDNA4X_INST_MONB, 0x20u, 2560u, 1440u, 10240u, 14745600ull, RDNA4X_NOFLIP2_FILE, \"monitor B\" } };" "    { RDNA4X_INST_MONA, 0x10u, 1920u, 1080u, 7680u,  8323072ull,  RDNA4X_NOFLIP2_FILE, \"monitor A\" },
    { RDNA4X_INST_MONB, 0x20u, 2560u, 1440u, 10240u, 14745600ull, RDNA4X_NOFLIP1_FILE, \"monitor B\" } };" "S9: the kill-file names are swapped"
plant 6  $X "    { RDNA4X_INST_MONA, 0x10u, 1920u," "    { RDNA4X_INST_MONA, 0x20u, 1920u," "S3: the monitor A's slot tag is the monitor B's"
plant 7  $X "static inline const rdna4x_desc_t *rdna4x_desc(uint32_t inst) { return (inst == RDNA4X_INST_MONA || inst == RDNA4X_INST_MONB) ? &rdna4x_descs[inst] : NULL; }" "static inline const rdna4x_desc_t *rdna4x_desc(uint32_t inst) { return (inst == RDNA4X_INST_MONA || inst == RDNA4X_INST_MONB) ? &rdna4x_descs[3u - inst] : NULL; }" "S1: the descriptor lookup returns the OTHER row"
plant 8  $X "    return rdna4x_surface_geom_ok((uint32_t)plan_inst, w, h, bpr);" "    (void)w; (void)h; (void)bpr; (void)dp_pitch; return 1;" "the size gate lets any HDMI-planned surface through (inst_geom_mismatch never fires)"
plant 9  $M "!rdna4x_plan_geom_ok(pi, gw, gh, gb, RDNA4S.pw, RDNA4S.ph, RDNA4S.pitch)) { RDNA4XI(pi).xs.inst_geom_mismatch++;" "0) { RDNA4XI(pi).xs.inst_geom_mismatch++;" "the glue never applies the size gate"
plant 10 $M "if (!have || !rdna4x_plan_geom_ok(pi, gw, gh, gb, RDNA4S.pw, RDNA4S.ph, RDNA4S.pitch))" "if (!rdna4x_plan_geom_ok(pi, gw, gh, gb, RDNA4S.pw, RDNA4S.ph, RDNA4S.pitch))" "an HDMI-planned surface with no recorded geometry is let through"
plant 11 $X "static inline int rdna4x_enabled(const rdna4x_enable_t *e) { return rdna4x_desc(e->inst) != NULL && e->kernel_m6 && e->kernel_m6flip && (e->inst != RDNA4X_INST_MONA || e->kernel_m6flip1) && e->have_call && e->have_handle && !e->killfile; }" "static inline int rdna4x_enabled(const rdna4x_enable_t *e) { return rdna4x_desc(e->inst) != NULL && e->kernel_m6 && e->kernel_m6flip && e->have_call && e->have_handle && !e->killfile; }" "the monitor A is enabled without the kernel's garnet-m6flip1 latch"
plant 12 $M ".kernel_m6flip1 = (q.flags & RDNA4X_SCANQ_M6FLIP1) != 0" ".kernel_m6flip1 = (q.flags & RDNA4X_SCANQ_M6FLIP) != 0" "the glue reads the m6flip bit for the third latch"
plant 13 $X "static inline int rdna4x_acq_geom_ok(const rdna4x_desc_t *d, uint32_t nout, uint64_t word) {
    if (!d) return 0;" "static inline int rdna4x_acq_geom_ok(const rdna4x_desc_t *d, uint32_t nout, uint64_t word) {
    (void)nout; (void)word; return d != 0;" "the Acquire's geometry word is never cross-checked"
plant 14 $X "    if (nout < 5u) return d->inst == RDNA4X_INST_MONB;" "    if (nout < 5u) return 1;" "the monitor A accepts a four-word Acquire answer (no geometry)"
plant 15 $X "(word >> 32) * 4u == d->pitch_bytes;" "(word >> 32) * 4u == 10240u;" "the pitch of the geometry word is the monitor B's for both"
plant 16 $X "    if (o->geom_verdict(o->ctx) != 0) { o->off(o->ctx, RDNA4DF_R_GEOM, \"the kernel's plane geometry for this instance is not the descriptor's\"); return RDNA4DF_R_GEOM; }
" "" "the acquire driver ignores the geometry verdict"
plant 17 $M "    const int ok = rdna4x_acq_geom_ok(d, a->nout, a->ao[4]);" "    const int ok = 1;" "the glue's geometry operation always says yes"
plant 18 $M "    if (rdna4x_acquire_retry4(inst, rc)) { no = 4;" "    if (rc) { no = 4;" "the monitor A may fall back to a four-word Acquire"
plant 19 $X "static inline int rdna4x_id_ok(uint32_t inst, uint32_t id) { const rdna4x_desc_t *d = rdna4x_desc(inst); return d && id >= d->tag && id < d->tag + 3u; }" "static inline int rdna4x_id_ok(uint32_t inst, uint32_t id) { (void)inst; return id >= 0x10u && id < 0x23u; }" "a slot id of the other instance is accepted at registration"
plant 20 $X "    for (uint32_t i = 1; i <= 2; i++) if (((xon_mask >> i) & 1u) && rdna4x_surface_geom_ok(i, w, h, bpr)) m |= 1u << i;" "    for (uint32_t i = 2; i <= 2; i++) if (((xon_mask >> i) & 1u) && rdna4x_surface_geom_ok(i, w, h, bpr)) m |= 1u << i;" "the classifier never accepts an monitor A-sized surface (the monitor A would never be planned)"
plant 21 $M "    for (int xi = 1; xi <= 2; xi++) {   // bundle 13/15: each HDMI instance's 1 s duties" "    for (int xi = 2; xi <= 2; xi++) {   // bundle 13/15: each HDMI instance's 1 s duties" "the 1 s tick keeps only the monitor B alive (the monitor A would be reverted after 5 s)"
plant 22 $M "rdna4x_release_all_locked(void) { for (int i = 1; i <= 2; i++) rdna4x_do_release_locked(i); }" "rdna4x_release_all_locked(void) { for (int i = 2; i <= 2; i++) rdna4x_do_release_locked(i); }" "the DP's release forgets the monitor A's A"
plant 23 $M "    for (int xi = 1; xi <= 2; xi++) if (RDNA4XI(xi).sm.acquired) { RDNA4XI(xi).sm.state = RDNA4DF_OFF; rdna4x_do_release_locked(xi); }" "    for (int xi = 2; xi <= 2; xi++) if (RDNA4XI(xi).sm.acquired) { RDNA4XI(xi).sm.state = RDNA4DF_OFF; rdna4x_do_release_locked(xi); }" "the explicit release request forgets the monitor A"
plant 24 $M "static uint32_t rdna4x_onmask_locked(void) { uint32_t m = 0; for (int i = 1; i <= 2; i++)" "static uint32_t rdna4x_onmask_locked(void) { uint32_t m = 0; for (int i = 2; i <= 2; i++)" "the enable mask never names the monitor A"
plant 25 $M ".size = d->slot_bytes, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT" ".size = RDNA4S.slotBytes, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT" "the HDMI slots are allocated with the DP's slot size"
plant 26 $M ".slot_bytes = d->slot_bytes, .slots = RDNA4DF_MAX_SLOTS" ".slot_bytes = RDNA4S.slotBytes, .slots = RDNA4DF_MAX_SLOTS" "the pool budget uses the DP's slot size"
plant 27 $M "rdna4x_k_register(a->inst, X->mem[i], d->pitch_bytes, d->w, d->h, &sl, &mc)" "rdna4x_k_register(a->inst, X->mem[i], RDNA4S.pitch, RDNA4S.pw, RDNA4S.ph, &sl, &mc)" "the monitor A's slots are registered with the DP's rectangle"
plant 28 $M "const uint64_t in[3] = { (uint64_t)inst, slot, 0 };" "const uint64_t in[3] = { 2u, slot, 0 };" "every present goes to instance 2"
plant 29 $M "RDNA4X.call(RDNA4R.dev, RDNA4N_SEL_SCANX_STATUS, in, 1, NULL, 0, o, &no, st, &sz);" "RDNA4X.call(RDNA4R.dev, RDNA4N_SEL_SCANX_STATUS, (const uint64_t[1]){ 2u }, 1, NULL, 0, o, &no, st, &sz);" "every status call asks instance 2 (the monitor A's keep-alive would be the monitor B's)"
plant 30 $M "stat(d->killfile, &kb) == 0" "stat(RDNA4X_NOFLIP2_FILE, &kb) == 0" "both instances poll the monitor B's kill file"
plant 31 $M "fl[i] = X->st.slot[X->sid[i] - d->tag].flags;
            if (RDNA4S.m6.latch" "fl[i] = X->st.slot[X->sid[i] - 0x20u].flags;
            if (RDNA4S.m6.latch" "the plan indexes the kernel's slots by the monitor B's tag for both"
plant 32 $M "            const rdna4df_rect_t r = { 0, 0, (int32_t)d->w, (int32_t)d->h };" "            const rdna4df_rect_t r = { 0, 0, (int32_t)RDNA4S.pw, (int32_t)RDNA4S.ph };" "the HDMI plan's rectangle is the DP's"
plant 33 $M "IOSurfaceGetWidth(ios) == d->w && IOSurfaceGetHeight(ios) == d->h && IOSurfaceGetBytesPerRow(ios) == d->pitch_bytes" "IOSurfaceGetWidth(ios) == RDNA4S.pw && IOSurfaceGetHeight(ios) == RDNA4S.ph && IOSurfaceGetBytesPerRow(ios) == RDNA4S.pitch" "the re-copy checks the DP's geometry"
plant 34 $M "if (RDNA4XI(xi).sm.state == RDNA4DF_ACTIVE && rdna4m6_table_stale(&RDNA4S.m6, RDNA4XI(xi).gen_known ? RDNA4XI(xi).gen : 0u)) stale = 1;" "if (xi == 2 && RDNA4XI(xi).sm.state == RDNA4DF_ACTIVE && rdna4m6_table_stale(&RDNA4S.m6, RDNA4XI(xi).gen_known ? RDNA4XI(xi).gen : 0u)) stale = 1;" "a stale monitor A table is not noticed at the completion"
plant 35 $M "        rdna4df_t *const sm = xinst ? &RDNA4XI(inst).sm : &RDNA4S.sm;" "        rdna4df_t *const sm = inst == 2 ? &RDNA4XI(inst).sm : &RDNA4S.sm;" "an monitor A frame completes on the DP's machine"
plant 37 $M "rdna4df_t *const dsm = rdna4x_desc((uint32_t)_dinst) ? &RDNA4XI(_dinst).sm : &RDNA4S.sm;" "rdna4df_t *const dsm = _dinst == 2 ? &RDNA4XI(_dinst).sm : &RDNA4S.sm;" "an monitor A submit is bookkept on the DP's machine"
plant 38 $A "#define RDNA4N_SCANQ_M6FLIP1    (1u << 7)" "#define RDNA4N_SCANQ_M6FLIP1    (1u << 6)" "the bundle's third-latch bit is the m6flip bit"
plant 39 $M "RDNA4XI(pi).plan_count++;" "" "plan counters are not kept"
plant 40 src/garnet-metal/Info.plist "<string>43</string>" "<string>14</string>" "the bundle still says build 14"
plant 41 $M "rdna4x_classify_mask(RDNA4S.pw, RDNA4S.ph, RDNA4S.pitch, rdna4x_onmask_locked(), (uint32_t)w, (uint32_t)h, (uint32_t)bpr)" "((w == RDNA4S.pw && h == RDNA4S.ph && bpr == RDNA4S.pitch) ? 1u : 0u)" "the classifier accepts only the DP's plane size"
plant 42 $M "RDNA4S.sg[k].sid = sid; RDNA4S.sg[k].w = (uint32_t)w;" "RDNA4S.sg[k].sid = sid; RDNA4S.sg[k].w = (uint32_t)0;" "the classifier records no geometry"
plant 43 $X "    if (slot_inst == RDNA4X_PLAN_DP && (e->inst == RDNA4M6_INST_MONA || e->inst == RDNA4M6_INST_MONB) && rdna4x_on(xon_mask, (int)e->inst)) return RDNA4X_C_RECOPY;" "    if (slot_inst == RDNA4X_PLAN_DP && e->inst == RDNA4M6_INST_MONB && rdna4x_on(xon_mask, (int)e->inst)) return RDNA4X_C_RECOPY;" "a tentative frame that turns out to be the monitor A's is not remembered for the re-copy"
echo "test-b15-plant: $total run (baseline included), $escaped escaped or did not apply"
exit $((escaped != 0))
