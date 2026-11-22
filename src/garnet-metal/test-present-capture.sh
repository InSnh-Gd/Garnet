#!/bin/bash
set -euo pipefail
test_dir=$(cd "$(dirname "$0")" && pwd)
source_dir=${1:-$test_dir}
out=${2:?Expected source directory and output directory}
mkdir -p "$out"
out=$(cd "$out" && pwd)
if [ ! -f "$source_dir/rdna4_present_capture.inc" ]; then
    printf 'PRESENT_CAPTURE_FAIL no observer of accepted scanout contents\n'
    exit 1
fi
python3 - "$source_dir" "$out" <<'PY'
from pathlib import Path
import sys
source, out = map(Path, sys.argv[1:])
text = (source / 'rdna4_shader_dump_impl.inc').read_text()
helpers = []
for signature in ('static NSString *rdna4_sha256hex(', 'static int rdna4_dump_write('):
    start = text.index(signature)
    end = text.index('\n}', start) + 2
    helpers.append(text[start:end])
(out / 'dump-write.inc').write_text('\n'.join(helpers) + '\n')
text = (source / 'rdna4_surface_impl.inc').read_text()
assert text.count('rdna4_capture_present(inst, slot, sid, seq,') == 2, 'normal and retry presents must both be observed'
PY
clang -std=gnu11 -fobjc-arc -Wall -Wextra -Werror -Wno-unused-function \
    -I "$out" -I "$source_dir" -framework Foundation -framework IOSurface \
    "$test_dir/test-present-capture.m" -o "$out/test-present-capture"
"$out/test-present-capture" "$out"
