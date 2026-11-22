#!/bin/bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../.." && pwd)
output=${1:?pass an evidence output directory}
mkdir -p "$output"
output=$(cd "$output" && pwd)
source_file=${DCN_DP_SOURCE:-"$repo/src/garnet/src/dcn/dcn.cpp"}
python3 - "$source_file" "$output" <<'PY'
from pathlib import Path
import hashlib
import sys
path, output = map(Path, sys.argv[1:])
raw = path.read_bytes()
source = raw.decode()
begin = source.index('uint32_t dpSnapshot(')
end = source.index('\n}', source.index('static void dp_log_preflight(', begin)) + 2
(output / 'dcn_dp_actual.inc').write_text(source[begin:end])
print(f'{path}: SHA256={hashlib.sha256(raw).hexdigest()}')
PY
clang++ -std=c++17 -Wall -Wextra -Werror -O1 -g \
    -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
    -I "$output" -I "$repo/src/garnet/src/dcn" -I "$repo/src/garnet/src" \
    -I "$repo/src/dcn41" -x c++ \
    "$repo/src/garnet/tests/dcn_dp_link_test.cpp" \
    "$repo/src/dcn41/dcn41_core.c" -o "$output/dcn_dp_link_test"
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1 "$output/dcn_dp_link_test"
