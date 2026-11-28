#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../.." && pwd)
ev=${1:-"${TMPDIR:-$repo/temp/scratch}/scan-present-guard"}
mkdir -p "$ev"
ev=$(cd "$ev" && pwd)
exec > >(tee "$ev/validation.log") 2>&1
set -x
python3 - "$repo" "$ev" <<'PY'
from pathlib import Path
import hashlib
import sys

repo, ev = map(Path, sys.argv[1:])
path = repo / 'src/garnet/src/dcn/dcn_scanout_mt.cpp'
hdr_path = repo / 'src/garnet/src/dcn/dcn_internal.hpp'
dcn_path = repo / 'src/garnet/src/dcn/dcn.cpp'
raw = path.read_bytes()
source = raw.decode()
hdr = hdr_path.read_text()
dcn_src = dcn_path.read_text()
assert '\nScanState gScan;' in dcn_src
def function(head):
    assert source.count(head) == 1, head
    start = source.index(head)
    end = source.index('\n}\n', start) + 3
    return source[start:end]
present = function('uint32_t scanPresent(uint64_t slotIn, uint64_t flags, uint64_t out[3]) {')
register = function('uint32_t scanRegister(')
poll = function('static bool scan_poll_locked(bool byIrq) {')
restore = function('static uint32_t scan_restore(')
status = function('uint32_t scanStatus(struct rdna4n_scan_status *o) {')
start = hdr.index('struct ScanState {')
end = hdr.index('\n};', start) + len('\n};')
state = hdr[start:end] + '\nScanState gScan;\n'
actual = '\n'.join((poll, restore, register, present, status))
(ev / 'scan_present_state.inc').write_text(state)
(ev / 'scan_present_actual.inc').write_text(actual)
assert (ev / 'scan_present_actual.inc').read_text() == actual
# Counterfactual baseline: remove only the live-read/reuse guard added by the fix.
begin = present.index('\t\tuint64_t plane = 0, early = 0; bool pend = true;')
end = present.index('\t\tconst Table saved = gScan.tbl;', begin)
guard = present[begin:end]
assert 'present_target_reusable' in guard and 'present_begin(' not in guard
baseline = present[:begin] + present[end:]
old = ev / 'guard-elided'
old.mkdir(exist_ok=True)
# Restore the pre-protection Status logic in the control, without editing source.
status_old = status
changes = [
    ('const bool planeOk = dcn41_hubp_read_primary_addr(&gDcn.d, 0u, &plane) == DCN41_OK;',
     '(void)dcn41_hubp_read_primary_addr(&gDcn.d, 0u, &plane);'),
    ('((earlyOk && early == s.mc) || (planeOk && plane == s.mc))', '(earlyOk && early == s.mc)'),
    ('present_target_reusable(gScan.tbl, i, plane, early, planeOk, earlyOk)',
     'slot_reusable(gScan.tbl, i, early, earlyOk)'),
]
for before, after in changes:
    assert status_old.count(before) == 1, before
    status_old = status_old.replace(before, after)
register_old = register.replace('boSize, boDcc }', 'boSize, (static_cast<void>(boDcc), false) }')
assert register_old != register
restore_old = restore.replace('if (dcn41_hubp_require_linear(&gDcn.d, 0u) != DCN41_OK) break;', '(void)0;')
assert restore_old != restore
(old / 'scan_present_actual.inc').write_text('\n'.join((poll, restore_old, register_old, baseline, status_old)))
(ev / 'status-control.txt').write_text(status_old)
(ev / 'removed-guard.txt').write_text(guard)
(ev / 'extraction.txt').write_text(
    f'Source: {path}\nSHA256: {hashlib.sha256(raw).hexdigest()}\n'
    f'scanPresent: original line {source[:source.index(present)].count(chr(10)) + 1}, {len(present.encode())} bytes\n'
    f'scanRegister: {len(register.encode())} bytes; scan_restore: {len(restore.encode())} bytes; '
    f'scan_poll_locked: {len(poll.encode())} bytes; scanStatus: {len(status.encode())} bytes; all five functions extracted verbatim.\n'
    'The negative control removes the changed guards; it is not a historical checkout.\n')
print((ev / 'extraction.txt').read_text())
PY
cxx=${CXX:-clang++}
"$cxx" --version
flags=(-std=c++17 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
src="$repo/src/garnet/tests/scan_present_guard_test.cpp"
inc="$repo/src/garnet/src/dcn"
export ASAN_OPTIONS=detect_leaks=1
export UBSAN_OPTIONS=print_stacktrace=1
"$cxx" "${flags[@]}" -I "$ev/guard-elided" -I "$ev" -I "$inc" "$src" -o "$ev/scan_present_guard_before"
set +e
"$ev/scan_present_guard_before" > "$ev/before.log" 2>&1
before_rc=$?
set -e
cat "$ev/before.log"
test "$before_rc" -eq 1
for reason in 'primary reuse' 'front reuse' 'pending reuse' 'earliest reuse' 'primary read failure' 'earliest read failure'; do
    grep -F "FAIL [$reason]: no program_flip on refusal" "$ev/before.log"
done
grep -F 'FAIL [status primary reuse]: PRIMARY slot is not REUSABLE' "$ev/before.log"
grep -F 'FAIL [status primary reuse]: PRIMARY slot is INUSE' "$ev/before.log"
grep -F 'FAIL [status primary read failure]: unknown live address exposes no REUSABLE slot' "$ev/before.log"
grep -F 'FAIL [register DCC backing]: DCC backing not admitted as linear' "$ev/before.log"
grep -F 'FAIL [restore after DCC appeared]: no address write on non-linear layout' "$ev/before.log"
"$cxx" "${flags[@]}" -I "$ev" -I "$inc" "$src" -o "$ev/scan_present_guard_test"
"$ev/scan_present_guard_test" | tee "$ev/after.log"
printf 'PASS: the negative control fails; production Register, Present, Status and Restore pass under ASan/UBSan\n'
