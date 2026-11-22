#!/usr/bin/env zsh
# test-b17-plant.sh - planted breaks for the two Stage 2 review fixes folded into bundle 17: R-B1 (the size gate also judges the DP's plan - an unknown monitor A-size surface planned tentatively to the DP must take no slot -
# and rdna4Finish checks width / height / bytes-per-row on EVERY frame) and R-S2 (the monitor B's four-word Acquire retry only after the -EINVAL shape refusal). Same discipline as test-b15-plant.sh: copy the REAL files into a scratch tree,
# apply ONE break (proved to be there exactly once), build and run test-m6x, test-m6route and test-dispflip and demand that one FAILS. A plant the tests let through is a hole: exit non-zero.
#   run from anywhere:  src/garnet-metal/test-b17-plant.sh
set -u
D="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$D/../.." && pwd)"
SCR="$(mktemp -d "${TMPDIR:-/tmp}/b17-plant.XXXXXX")"
case "$SCR" in */b17-plant.*) ;; *) echo "bad scratch dir $SCR"; exit 2;; esac
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
# ---- R-B1: the size gate and the rdna4Finish check
plant 1  $X "    if (plan_inst == RDNA4X_PLAN_DP) return dp_w != 0u && w == dp_w && h == dp_h && bpr == dp_pitch;" "    if (plan_inst == RDNA4X_PLAN_DP) return (dp_w | dp_h | dp_pitch | w | h | bpr) | 1u;" "R-B1: the DP's plan is not judged (an unknown monitor A-size surface gets a DP slot)"
plant 2  $X "dp_w != 0u && w == dp_w && h == dp_h && bpr == dp_pitch;" "dp_w != 0u && w == dp_w && h == dp_h && (bpr | dp_pitch) != 0xFFFFFFFFu;" "R-B1: the DP gate ignores the pitch"
plant 3  $X "dp_w != 0u && w == dp_w && h == dp_h && bpr == dp_pitch;" "dp_w != 0u && w == dp_w && (h | dp_h) != 0xFFFFFFFFu && bpr == dp_pitch;" "R-B1: the DP gate ignores the height"
plant 4  $X "dp_w != 0u && w == dp_w && h == dp_h && bpr == dp_pitch;" "dp_w != 0u && (w | dp_w) != 0xFFFFFFFFu && h == dp_h && bpr == dp_pitch;" "R-B1: the DP gate ignores the width"
plant 5  $X "dp_w != 0u && w == dp_w" "w == dp_w" "R-B1: an unset DP plane admits a zero-sized surface"
plant 6  $M "        if (pi == RDNA4X_PLAN_DP || pi == RDNA4X_PLAN_MONA || pi == RDNA4X_PLAN_MONB) {" "        if (pi == RDNA4X_PLAN_MONA || pi == RDNA4X_PLAN_MONB) {" "R-B1: the glue applies the size gate to the HDMI plans only (the original hole)"
plant 7  $M "if (pi != RDNA4X_PLAN_DP) RDNA4XI(pi).plan_count++;" "RDNA4XI(pi).plan_count++;" "R-B1: the DP's plan is counted as an HDMI plan"
plant 8  $M "(imgsrc ? rdna4df_img_ok(gw_, gh_, gp_, 4) : (BOOL)YES) && [dt bytesPerPixel] == 4 && [dt width] == gw_ && [dt height] == gh_ && [dt rdna4Levels] == 1 && (imgsrc || [dt rdna4IOSBytesPerRow] == gp_)" "(imgsrc ? rdna4df_img_ok(gw_, gh_, gp_, 4) : (BOOL)YES) && [dt bytesPerPixel] == 4 && [dt rdna4Levels] == 1 && (imgsrc || [dt rdna4IOSBytesPerRow] == gp_)" "R-B1: rdna4Finish does not check width and height"
plant 9  $M "[dt width] == gw_ && [dt height] == gh_ && [dt rdna4Levels] == 1 && (imgsrc || [dt rdna4IOSBytesPerRow] == gp_)" "[dt width] == gw_ && [dt height] == gh_ && [dt rdna4Levels] == 1" "R-B1: rdna4Finish does not check the bytes-per-row of a buffer-sourced frame"
plant 10 $M "[dt width] == gw_ && [dt height] == gh_ && [dt rdna4Levels] == 1 && (imgsrc || [dt rdna4IOSBytesPerRow] == gp_)" "[dt width] == gw_ && [dt height] == gh_ && [dt rdna4Levels] == 1 && (!imgsrc || [dt rdna4IOSBytesPerRow] == gp_)" "R-B1: the bytes-per-row check is inverted (image frames only)"
plant 11 $M "            if (take && !((imgsrc ? rdna4df_img_ok" "            if (take && imgsrc && !((imgsrc ? rdna4df_img_ok" "R-B1: the rdna4Finish check applies to image-sourced frames only (the original hole)"
plant 12 $M "            if (!have || !rdna4x_plan_geom_ok(pi, gw, gh, gb, RDNA4S.pw, RDNA4S.ph, RDNA4S.pitch))" "            if (!have || !rdna4x_plan_geom_ok(pi, gw, gh, gb, gw, gh, gb))" "R-B1: the glue passes the surface's own geometry as the DP plane"
plant 13 $M "pi = rdna4x_plan_inst(&RDNA4S.m6, sid, xon, &tent);" "pi = rdna4x_plan_inst(&RDNA4S.m6, sid, xon, &tent); if (tent) pi = RDNA4X_PLAN_DP, tent = 0;" "R-B1: a tentative plan is no longer marked tentative"
# ---- R-S2: the Acquire retry
plant 14 $X "return inst == (int)RDNA4X_INST_MONB && rc == -EINVAL; }" "return inst == (int)RDNA4X_INST_MONB && rc != 0; }" "R-S2: the monitor B is retried after ANY error"
plant 15 $X "return inst == (int)RDNA4X_INST_MONB && rc == -EINVAL; }" "return (inst != 77) && rc == -EINVAL; }" "R-S2: the monitor A is retried with four words too"
plant 16 $X "return inst == (int)RDNA4X_INST_MONB && rc == -EINVAL; }" "return inst == (int)RDNA4X_INST_MONB && (rc == -EINVAL || rc == -EBUSY); }" "R-S2: -EBUSY is retried"
plant 17 $X "return inst == (int)RDNA4X_INST_MONB && rc == -EINVAL; }" "return inst == 2 && (rc & 0) != 0; }" "R-S2: a 0.0.661 kernel (four words only) is never retried"
plant 18 $M "    if (rdna4x_acquire_retry4(inst, rc)) { no = 4;" "    if (rc && inst == (int)RDNA4X_INST_MONB) { no = 4;" "R-S2: the glue retries after any error (does not call rdna4x_acquire_retry4)"
echo "test-b17-plant: $total run (baseline included), $escaped escaped or did not apply"
exit $((escaped != 0))
