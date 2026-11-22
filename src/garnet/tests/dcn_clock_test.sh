#!/bin/bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../.." && pwd)
output=${1:-"$repo/../.omo/evidence/b37-high-refresh/clock"}
output=${1:-"$repo/temp/scratch/dcn-clock"}
mkdir -p "$output"
output=$(cd "$output" && pwd)
source_file=${DCN_CLOCK_SOURCE:-"$repo/src/garnet/src/dcn/dcn.cpp"}
source_file=${DCN_CLOCK_SOURCE:-"$repo/src/garnet/src/dcn/dcn_scanout_mt.cpp"}
python3 - "$source_file" "$output" <<'PY'
from pathlib import Path
import hashlib
import sys

path, output = map(Path, sys.argv[1:])
raw = path.read_bytes()
source = raw.decode()
begin = source.index('struct VblCache {')
end = source.index('\n}  // namespace', begin)
cache = source[begin:end]
def function(head):
    begin = source.index(head)
    return source[begin:source.index('\n}', begin) + 2]
sample = function('bool vblSample(uint64_t *periodNs, uint64_t *delayNs, uint64_t *nowAbs) {')
query = function('uint32_t scanQuery(struct rdna4n_scan_query *o) {')
(output / 'dcn_clock_actual.inc').write_text(cache + '\n' + sample + '\n' + query)
print(f'{path}: SHA256={hashlib.sha256(raw).hexdigest()}')
PY
clang++ -std=c++17 -Wall -Wextra -Werror -O1 -g \
    -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
    -I "$output" -I "$repo/src/garnet/src/dcn" -I "$repo/src/garnet/src" \
    -I "$repo/src/dcn41" -x c++ \
    "$repo/src/garnet/tests/dcn_clock_test.cpp" \
    "$repo/src/dcn41/dcn41_core.c" "$repo/src/dcn41/dcn41_otg.c" \
    "$repo/src/dcn41/dcn41_otg_timing.c" "$repo/src/dcn41/dcn41_hubp.c" \
    -o "$output/dcn_clock_test"
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1 "$output/dcn_clock_test"
