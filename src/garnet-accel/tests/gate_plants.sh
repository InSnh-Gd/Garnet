#!/usr/bin/env zsh
# gate_plants.sh - planted breaks for the LINK gates of the GarnetAccel kext (gates A/B/C in gates/gate_link.py, the link check in the Makefile, and the host twin of the
# runtime layout gate, gates/layout_gate_host.py). For each plant: copy the aux dir and the ioaccel-layout tools into a scratch tree, apply the break, run the REAL `make`
# and demand that it FAILS; where the kext still linked, the host twin is also run on the planted binary and must say what the gate says (it FAILs when a slot is not
# exactly the family's). The CONTROL (no break) must build and both gates PASS. Slow (about 30 s per plant).
#   GARNET_KC_WORK=/path/to/kc-work src/garnet-accel/tests/gate_plants.sh [first last]
set -u
HERE="$(cd "$(dirname "$0")/.." && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
SCR="${TMPDIR:-/tmp}/garnetaccel-gplant.$$"
trap 'rm -rf "$SCR"' EXIT
escaped=0; total=0
FIRST=${1:-0}; LAST=${2:-9999}
A=src/garnet-accel
L=tools/ioaccel-layout
LAYOUT_DATA="${GARNET_LAYOUT_OUT:-$ROOT/$L/layout-out}"
KC_WORK="${GARNET_KC_WORK:?set GARNET_KC_WORK to the target KC work directory}"

fresh() {
  rm -rf "$SCR"; mkdir -p "$SCR/src" "$SCR/tools"
  rsync -a --exclude build "$HERE/" "$SCR/$A/"
  rsync -a --exclude build --exclude vtables --exclude layout-out "$ROOT/$L/" "$SCR/$L/"
  rsync -a --exclude build --exclude vtables "$LAYOUT_DATA/" "$SCR/$L/layout-out/"
}
run_make() {   # -> 0 if make succeeded (gates passed), 1 if it failed
  (cd "$SCR/$A" && GARNET_LAYOUT_OUT="$SCR/$L/layout-out" make CXX="${CXX:-clang++}" CC="${CC:-clang}" MKSDK="${MKSDK:-$ROOT/build-toolchain/sdk}" KEXT_LD="${KEXT_LD:-$ROOT/build-toolchain/cctools/bin/x86_64-apple-darwin-ld}" GARNET_KC_WORK="$KC_WORK" > "$SCR/make.log" 2>&1)
}
twin() {       # the host twin on the planted binary, if it linked
  local exe="$SCR/$A/build/GarnetAccel.kext/Contents/MacOS/GarnetAccel"
  [ -f "$exe" ] || { echo "no binary"; return 9; }
  local nm_bin="${GARNET_NM:-}"
  if [ -z "$nm_bin" ] && [ "$(uname -s)" != Darwin ]; then nm_bin=llvm-nm; fi
  (cd "$SCR/$A" && GARNET_NM="$nm_bin" GARNET_KC_WORK="$KC_WORK" GARNET_LAYOUT_OUT="$SCR/$L/layout-out" python3 gates/layout_gate_host.py "$exe" 2>&1 | tail -1)
}
plant() {   # plant <id> <file relative to the scratch root> <description> <old1> <new1> ...
  local id="$1" file="$2" desc="$3" want_twin="$4"; shift 4
  if [ "$id" -lt "$FIRST" ] || [ "$id" -gt "$LAST" ]; then return; fi
  fresh
  local pairs=""
  while [ $# -ge 2 ]; do pairs="${pairs}${1}"$'\x1e'"${2}"$'\x1d'; shift 2; done
  PAIRS="$pairs" FILE="$SCR/$file" python3 - <<'PY'
import os,sys
p=os.environ['FILE']; s=open(p).read()
for pair in os.environ['PAIRS'].split('\x1d'):
    if not pair: continue
    o,n=pair.split('\x1e')
    if s.count(o)!=1:
        print("PLANT TEXT NOT FOUND EXACTLY ONCE (%d): %r" % (s.count(o), o[:80])); sys.exit(3)
    s=s.replace(o,n)
open(p,'w').write(s)
PY
  local rc=$?
  total=$((total+1))
  if [ $rc -ne 0 ]; then echo "PLANT $id: $desc: THE PLANT ITSELF DID NOT APPLY"; escaped=$((escaped+1)); return; fi
  if run_make; then echo "PLANT $id: $desc: *** ESCAPED (make passed) ***"; escaped=$((escaped+1)); return; fi
  local why; why=$(grep -m1 -E 'LINK ERROR|GATE FAIL|error:|import not exported|MISMATCH|FAIL' "$SCR/make.log" | cut -c1-150)
  local extra=""
  if [ "$want_twin" = twin ]; then
    local t; t=$(twin)
    if echo "$t" | grep -q '^MISMATCH'; then extra="; host twin: $t"; else extra="; *** host twin did NOT fail: $t ***"; escaped=$((escaped+1)); fi
  fi
  echo "PLANT $id: $desc: CAUGHT: $why$extra"
}
control() {
  fresh; total=$((total+1))
  if run_make && grep -q 'GATES A/B/C PASS' "$SCR/make.log" && grep -q 'ALL MATCH' "$SCR/make.log"; then echo "CONTROL (no break): make passed; $(grep 'GATES A/B/C PASS' "$SCR/make.log" | cut -c1-110); $(grep 'ALL MATCH' "$SCR/make.log")"
  else echo "CONTROL (no break): *** FAILED ***"; tail -5 "$SCR/make.log"; escaped=$((escaped+1)); fi
}
S=$A/src/GarnetAccel.cpp
H=$L/layout-out/IOAccelFamily2_decl.h
control
plant 1 $S "a leaf class introduces a virtual (the vtable grows past the family's)" twin 'class GarnetTask : public IOAccelTask {
	OSDeclareDefaultStructors(GarnetTask)
public:' 'class GarnetTask : public IOAccelTask {
	OSDeclareDefaultStructors(GarnetTask)
public:
	virtual void extra();' 'bool GarnetTask::setup(IOGraphicsAccelerator2 *accel, uint32_t kind) {' 'void GarnetTask::extra() {}
bool GarnetTask::setup(IOGraphicsAccelerator2 *accel, uint32_t kind) {'
plant 3 $S "a forwarder is declared but its definition is gone (the kernel linker would refuse)" none 'GARNET_FWD_DEFS_IOAccelSysMemory(GarnetSysMemory)' ''
plant 4 $S "an import that no image exports" none '"__ZN11IOAccelTask4initEP22IOGraphicsAccelerator2jPP16IORangeAllocator"' '"__ZN11IOAccelTask4initEP22IOGraphicsAccelerator2jPP16IORangeAllocatorx"'
plant 5 $S "a leaf overrides a slot the family implements (an unexpected override of IOService::start)" twin 'class GarnetDisplayMachine : public IOAccelDisplayMachine {
	OSDeclareDefaultStructors(GarnetDisplayMachine)
public:' 'class GarnetDisplayMachine : public IOAccelDisplayMachine {
	OSDeclareDefaultStructors(GarnetDisplayMachine)
public:
	bool start(IOService *provider) override { return IOService::start(provider); }'
plant 6 $H "the generated header is stale (one asm label changed)" none 'virtual bool start(IOService *) override __asm__("__ZN22IOGraphicsAccelerator25startEP9IOService");' 'virtual bool start(IOService *) override __asm__("__ZN22IOGraphicsAccelerator24stopEP9IOService");'
plant 7 $A/gates/leaves.py "the expected-override list forgets probe / start / free" none "[183, 184, 18, 322, 348, 329, 239, 238]),   # IOService::probe" "[]),   # IOService::probe"
plant 9 $S "a leaf inherits a slot by a placeholder name (a reference to a symbol the family never defined)" none '	GARNET_R_IOAccelDisplayMachine_269 displayModeWillChange() override;' '	GARNET_R_IOAccelDisplayMachine_269 displayModeWillChange() override;
	virtual void _vslot300();'
plant 10 $L/gen_header.py "the generator stops labelling family slots (the vtable would reference _vslotN)" none "    asm = ' __asm__(\"%s\")' % lab" "    asm = ''"
plant 11 $A/gates/leaves.py "a hooked pure slot is dropped from the table (the class stays abstract)" none "84: 'zero', 85: 'zero', 86: '1', 87: 'zero'" "84: 'zero', 85: 'zero', 86: '1'"
plant 12 $A/gates/leaves.py "a trampoline leaf's hooked slot is renumbered (override set and header disagree)" none "{43: 'zero', 61: 'zero', 62: 'zero'}" "{43: 'zero', 61: 'zero', 63: 'zero'}"

# ---- aux 0.0.3: the display pipe in the gates ----
P=$A/src/garnetaccel_pure.h
plant 13 $P "FAIL-OPEN: the runtime gate accepts an IOAccelDisplayPipe whose inherited slot differs (the twin's mismatched-pipe controls run the SAME gate_compare)" none '} else if (ours[i] != fam[i]) {' '} else if (false) {'
plant 14 $P "FAIL-OPEN: the runtime gate accepts a longer IOAccelDisplayPipe vtable (the twin's control: 307 slots)" none '    if (fam[nslots] != 0) { r.v = kGateFamilyLonger;' '    if (false) { r.v = kGateFamilyLonger;'
plant 15 $A/gates/leaves.py "the display pipe leaf is left out of the gate's leaf list (13 classes / 2720 slots expected)" none "    ('GarnetDisplayPipe',   'IOAccelDisplayPipe',       [277]),            # aux 0.0.3: performTransaction, hand-written (its generated declaration is a placeholder)
" ""
plant 16 $A/gates/leaves.py "the expected totals still say 11 classes" none "EXPECT_CLASSES = 13" "EXPECT_CLASSES = 11"

# ---- aux 0.0.4: GarnetFramebuffer vs IOFramebuffer in the gates (350 slots; the slots 305-348 of the M5 spec are covered one by one by the twin's 65 negative controls) ----
plant 17 $S "the framebuffer introduces a virtual (its vtable grows past IOFramebuffer's 350 slots)" twin 'class GarnetFramebuffer : public IOFramebuffer {
	OSDeclareDefaultStructors(GarnetFramebuffer)
public:' 'class GarnetFramebuffer : public IOFramebuffer {
	OSDeclareDefaultStructors(GarnetFramebuffer)
public:
	virtual void extraFb();' '#include "garnet_accel_display_impl.inc"' 'void GarnetFramebuffer::extraFb() {}
#include "garnet_accel_display_impl.inc"'
plant 18 $S "the framebuffer overrides isConsoleDevice (slot 305: it would claim the console)" twin 'class GarnetFramebuffer : public IOFramebuffer {
	OSDeclareDefaultStructors(GarnetFramebuffer)
public:' 'class GarnetFramebuffer : public IOFramebuffer {
	OSDeclareDefaultStructors(GarnetFramebuffer)
public:
	bool isConsoleDevice() override { return true; }'
plant 19 $S "the framebuffer overrides setInterruptState (slot 348: an interrupt source of its own)" twin 'class GarnetFramebuffer : public IOFramebuffer {
	OSDeclareDefaultStructors(GarnetFramebuffer)
public:' 'class GarnetFramebuffer : public IOFramebuffer {
	OSDeclareDefaultStructors(GarnetFramebuffer)
public:
	IOReturn setInterruptState(void *interruptRef, UInt32 state) override { return IOFramebuffer::setInterruptState(interruptRef, state); }'
plant 20 $A/gates/leaves.py "the framebuffer's expected-override list forgets getDDCBlock (slot 345)" twin "FB_EXTRA = [184, 310, 311, 319, 322, 325, 326, 330, 331, 332, 344, 345]" "FB_EXTRA = [184, 310, 311, 319, 322, 325, 326, 330, 331, 332, 344]"
plant 21 $A/gates/leaves.py "the framebuffer's expected-override list claims an override that is not there (slot 305)" twin "FB_EXTRA = [184, 310, 311, 319, 322, 325, 326, 330, 331, 332, 344, 345]" "FB_EXTRA = [184, 305, 310, 311, 319, 322, 325, 326, 330, 331, 332, 344, 345]"
plant 22 $A/gates/layout_gate_host.py "the host twin cannot resolve IOGraphicsFamily symbols (the System KC dropped from its table)" none "    for lvl, p in ((1, FAMILY), (0, BOOT_KC), (1, SYS_KC)):" "    for lvl, p in ((1, FAMILY), (0, BOOT_KC)):"
plant 23 $L/gen_header.py "the generated header drops IOFramebuffer's size / slot count (the SDK class is no longer checked against the kernel)" none "EXTRA_SDK = {'IOFramebuffer': 'IOKit/graphics/IOFramebuffer.h'}" "EXTRA_SDK = {}"

# ---- aux 0.0.5 (G6): slot 239, newUserClient, in the gates ----
plant 24 $S "SLOT 239 NOT OVERRIDDEN: the class has no newUserClient override but the expected list says it does (the vtable slot is the family's)" twin 'IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type, IOUserClient **handler) override;' 'IOReturn garnetNotNewUserClient(task_t owningTask, void *securityID, UInt32 type, IOUserClient **handler);' 'IOReturn GarnetAccelerator::newUserClient(task_t owningTask, void *securityID, UInt32 type, IOUserClient **handler)' 'IOReturn GarnetAccelerator::garnetNotNewUserClient(task_t owningTask, void *securityID, UInt32 type, IOUserClient **handler)' 'if (type == GARNET_METAL_UC_GARNETN) return newUserClient(owningTask, securityID, type, handler);' 'if (type == GARNET_METAL_UC_GARNETN) return IOGraphicsAccelerator2::newUserClient(owningTask, securityID, type, handler);'
plant 25 $A/gates/leaves.py "slot 239 is overridden but the expected-override list forgets it (an unexpected override)" twin "[183, 184, 18, 322, 348, 329, 239, 238])" "[183, 184, 18, 322, 348, 329, 238])"
plant 26 $A/gates/leaves.py "a class is added to the expected totals for the 239 change (the slot count must NOT change)" none "EXPECT_SLOTS = 2720" "EXPECT_SLOTS = 2721"
plant 27 $S "slot 238 inherits IOService's error-masking fallback" twin 'IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type, OSDictionary *properties, IOUserClient **handler) override;' 'IOReturn garnetNotNewUserClient5(task_t owningTask, void *securityID, UInt32 type, OSDictionary *properties, IOUserClient **handler);' 'IOReturn GarnetAccelerator::newUserClient(task_t owningTask, void *securityID, UInt32 type, OSDictionary *properties,' 'IOReturn GarnetAccelerator::garnetNotNewUserClient5(task_t owningTask, void *securityID, UInt32 type, OSDictionary *properties,'
plant 28 $A/gates/leaves.py "slot 238 is overridden but omitted from the gate" twin "[183, 184, 18, 322, 348, 329, 239, 238])" "[183, 184, 18, 322, 348, 329, 239])"

echo "gate plants: $total run, $escaped escaped/failed"
[ "$escaped" -eq 0 ]
