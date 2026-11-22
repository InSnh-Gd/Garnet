#!/usr/bin/env zsh
# test-b19-plant.sh - planted breaks for bundle build 19 (missing menus: RG16Uint, an internal design note "Fix contract"): the table entry, the integer clear, the fallback write mask, the kill file, the header's
# conversion rules, the version, and the diff guard (apps mode, base 2d779c19). Same discipline as test-b18-plant.sh: copy the REAL files into a scratch tree, apply ONE break (the script proves the text was there exactly
# once), run the suites and demand at least one FAILS (a compile error counts). A plant the tests let through is a hole: exit non-zero.
#   usage: test-b19-plant.sh           env: TMPDIR (where the scratch tree goes)        Fast mode: C compiles at -O0, no sanitizers.
set -u
D="$(cd "$(dirname "$0")" && pwd)"
SCR="$(mktemp -d "${TMPDIR:-/tmp}/b19-plant.XXXXXX")"
case "$SCR" in */b19-plant.*) ;; *) echo "bad scratch dir $SCR"; exit 2;; esac
trap 'rm -rf -- "$SCR"' EXIT
escaped=0; total=0
W="$SCR/c"
cfresh() { rm -rf -- "$W"; mkdir -p "$W"; cp "$D"/rdna4_*.h "$D"/GarnetDevice.m "$D"/Info.plist "$D"/test-intfmt.c "$D"/test-intclear-semantics.m  "$W/"; python3 "$D/assemble-device.py" "$W/GarnetDevice.m" --include-dir "$D" --output "$W/GarnetDevice.m"; }
c_test() {
  OUT=""
  ( cd "$W" && cc -O0 -Wall -Wextra -Werror -o t-intfmt test-intfmt.c -lm ) >"$SCR/cc.out" 2>&1 || { OUT="test-intfmt: compile error: $(grep -m1 error "$SCR/cc.out" | cut -c1-100)"; return 1; }
  ( cd "$W" && ./t-intfmt . ) >"$SCR/o0" 2>&1 || { OUT="test-intfmt: $(grep -m1 '^FAIL' "$SCR/o0" | cut -c1-110)"; return 1; }
  ( cd "$W" && clang -fobjc-arc -framework Metal -framework Foundation -I. -o t-intclear test-intclear-semantics.m ) >"$SCR/cc.out" 2>&1 || { OUT="test-intclear-semantics: compile error: $(grep -m1 error "$SCR/cc.out" | cut -c1-100)"; return 1; }
  ( cd "$W" && ./t-intclear ) >"$SCR/o0" 2>&1; local rc=$?
  [ $rc -eq 3 ] && { OUT="test-intclear-semantics: SKIP (no Metal device): the plant run cannot judge the conversion"; return 1; }
  [ $rc -eq 0 ] || { OUT="test-intclear-semantics: $(grep -m1 '^FAIL' "$SCR/o0" | cut -c1-110)"; return 1; }
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
M=GarnetDevice.m; H=rdna4_intfmt.h
# ---- the contract's four named breaks
cplant 1  $M "{ MTLPixelFormatRG16Uint,        VK_FORMAT_R16G16_UINT,              4, RDNA4F_UINT }," "{ MTLPixelFormatRG16Uint,        VK_FORMAT_R16G16_UNORM,             4, RDNA4F_UINT }," "the table maps RG16Uint to R16G16_UNORM (a float format: the shaders' uint outputs and reads would be wrong)"
cplant 3  $M "        if ([t rdna4IsUInt]) {   // bundle 19" "        if (0 && [t rdna4IsUInt]) {   // bundle 19" "the clear goes back to float32 for integer attachments (the uint branch is dead)"
cplant 4  $M "        wm = rdna4if_write_mask(fb, (f->flags & RDNA4F_UINT) != 0, wm);   // bundle 19: the float magenta fallback shader must not write into an integer attachment" "        (void)fb;" "the fallback write-mask rule is dropped (float magenta bits written into the uint attachment)"
cplant 5  $M "v = stat(RDNA4_NOUINT_FILE, &st) == 0;" "v = NO;" "the kill-file read is removed (/private/tmp/rdna4m-nouint does nothing)"
# ---- further breaks of the same four, different sites
cplant 6  $M "return ((rdna4_fmts[i].flags & RDNA4F_UINT) && rdna4_nouint()) ? NULL : &rdna4_fmts[i];" "return &rdna4_fmts[i];" "rdna4_fmt() ignores the kill file (the file is read but nothing consults it)"
cplant 7  $M "#define RDNA4F_UINT 4u" "#define RDNA4F_UINT 2u" "the new flag collides with RDNA4F_DS"
cplant 8  $M "rdna4if_clear_u(c.green, 16), rdna4if_clear_u(c.blue, 16)" "rdna4if_clear_u(c.green, 16), (uint32_t)c.blue" "one channel of the integer clear skips the Metal conversion"
cplant 10 $M "        } else
        _cvs[na] = (VkClearValue){ .color = { .float32" "        }
        _cvs[na] = (VkClearValue){ .color = { .float32" "the float32 fill is no longer the else arm: it runs after the uint fill and overwrites it"
cplant 11 $M "- (BOOL)rdna4IsUInt { return _fmt != NULL && (_fmt->flags & RDNA4F_UINT) != 0; }" "- (BOOL)rdna4IsUInt { return NO; }" "rdna4IsUInt is always NO (the branch is unreachable)"
cplant 12 $M "wm = rdna4if_write_mask(fb, (f->flags & RDNA4F_UINT) != 0, wm);" "wm = rdna4if_write_mask(0, (f->flags & RDNA4F_UINT) != 0, wm);" "the mask rule is told the pipeline is never a fallback"
cplant 13 $M "#include \"rdna4_intfmt.h\"" "#include \"rdna4_texdesc.h\"" "rdna4_intfmt.h is no longer included"
# ---- the header's rules
cplant 14 $H "    return (uint32_t)v;                        // truncate toward zero" "    return (uint32_t)(v + 0.5);                // round" "the conversion rounds instead of truncating"
cplant 15 $H "    if (v >= (double)mx) return mx;            // saturate" "    if (v >= (double)mx) return (uint32_t)v;   // no saturation" "the conversion does not saturate"
cplant 16 $H "    if (!(v > 0.0)) return 0;                  // NaN, -0, negatives" "    if (v < 0.0) return 0;                     // negatives only" "NaN is not mapped to 0"
cplant 17 $H "return (fallback && is_int_attachment) ? 0u : wm; }" "return (fallback || is_int_attachment) ? 0u : wm; }" "the write mask is cleared for every integer attachment, even the real shader's"
cplant 18 $H "return (fallback && is_int_attachment) ? 0u : wm; }" "return (fallback && 0) ? 0u : wm; }" "the header's mask rule does nothing"
# ---- version
cplant 19 Info.plist "<key>CFBundleVersion</key>
	<string>43</string>" "<key>CFBundleVersion</key>
	<string>18</string>" "the bundle is still build 18"
echo "test-b19-plant: $total planted/baseline runs, $escaped escaped"
[ $escaped -eq 0 ]
