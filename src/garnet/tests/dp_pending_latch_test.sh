#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../.." && pwd)
ev=${1:?usage: dp_pending_latch_test.sh evidence-directory}
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
start = hdr.index('struct ScanState {')
end = hdr.index('\n};', start) + len('\n};')
(ev / 'scan_present_state.inc').write_text(hdr[start:end] + '\nScanState gScan;\n')
(ev / 'scan_present_actual.inc').write_text('\n'.join(function(head) for head in (
    'static bool scan_poll_locked(bool byIrq) {',
    'uint32_t scanPresent(uint64_t slotIn, uint64_t flags, uint64_t out[3]) {',
    'uint32_t scanStatus(struct rdna4n_scan_status *o) {')))
(ev / 'extraction.txt').write_text(f'{path}\nSHA256: {hashlib.sha256(raw).hexdigest()}\nVerbatim production state, poll, Present and Status.\n')
PY
"${CXX:-clang++}" -std=c++17 -Wall -Wextra -Werror -O1 -g \
  -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
  -I "$ev" -I "$repo/src/garnet/src/dcn" \
  "$repo/src/garnet/tests/dp_pending_latch_test.cpp" -o "$ev/dp_pending_latch_test"
"$ev/dp_pending_latch_test"
