#!/bin/bash
set -euo pipefail
test_dir=$(cd "$(dirname "$0")" && pwd)
source_dir=${1:-$(cd "$test_dir/.." && pwd)}
out=${2:?Expected source directory and output directory}
mkdir -p "$out"
out=$(cd "$out" && pwd)
python3 - "$source_dir" "$out" <<'PY'
from pathlib import Path
import sys
source, out = map(Path, sys.argv[1:])
text = (source / 'garnet_commands.m').read_text()
start = text.index('static rdna4crc_rec_t *rdna4s_crc_check(')
end = text.index('\n}', start) + 2
(out / 'crc-check.inc').write_text(text[start:end] + '\n')
text = (source / 'garnet_metal_internal.h').read_text()
start = text.index('typedef struct { IOSurfaceRef ios;')
end = text.index('\n', start)
(out / 'crc-record.inc').write_text(text[start:end] + '\n')
PY
clang -std=gnu11 -fobjc-arc -Wall -Wextra -Werror -Wno-unused-function \
    -I "$out" -I "$source_dir" -framework Foundation -framework IOSurface \
    "$test_dir/test-crc-observer.m" -o "$out/test-crc-observer"
"$out/test-crc-observer"
