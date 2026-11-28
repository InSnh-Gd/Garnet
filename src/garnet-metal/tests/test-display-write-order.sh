#!/usr/bin/env bash
set -euo pipefail
if [ "${1:-}" = --help ]; then
    printf 'Usage: bash test-display-write-order.sh [GarnetDevice.m [OUTPUT_DIR]]\nExtracts real write bookkeeping and surface selection. Mock textures; no Vulkan, GPU or bundle load. Linux: libobjc. macOS: Foundation/ARC.\n'
    exit 0
fi
[ "$#" -le 2 ] || exit 2
test_dir=$(cd -- "$(dirname -- "$0")" && pwd)
source_dir=$(cd -- "$test_dir/.." && pwd)
source_file=${1:-$source_dir/GarnetDevice.m}
if [ "$#" -eq 2 ]; then
    mkdir -p -- "$2"
    out=$(cd -- "$2" && pwd)
else
    out=$(mktemp -d "${TMPDIR:-/tmp}/rdna4-write-order.XXXXXX")
    trap 'rm -rf -- "$out"' EXIT
fi
python3 "$source_dir/assemble-device.py" "$source_file" --output "$out/device.m"
python3 - "$out/device.m" "$out" <<'PY'
import hashlib
import json
from pathlib import Path
import sys

source, out = Path(sys.argv[1]).resolve(), Path(sys.argv[2])
text = source.read_text()
written = text.index('- (void)rdna4IOWritten:(RDNA4Texture *)t {')
fallback = text.index('- (void)rdna4NoteShaderFallback {')
root = text.index('- (id)rootResource {')
finish = text.index('- (BOOL)rdna4Finish:(NSError **)err {')
select = text.index('        RDNA4Texture *dt = nil; int nd = 0;', finish)
for name, start, end in [
    ('written', written, text.index('\n}', written) + 2),
    ('fallback', fallback, text.index('\n', fallback)),
    ('root', root, text.index('\n', root)),
    ('select', select, text.index('        if (nd > 1)', select)),
]:
    line = text.count('\n', 0, start) + 1
    (out / f'{name}.inc').write_text(f'#line {line} {json.dumps(str(source))}\n' + text[start:end] + '\n')
print(f'Source: {source}\nSHA256: {hashlib.sha256(source.read_bytes()).hexdigest()}')
PY
common=(-std=gnu11 -Wall -Wextra -Werror -Wno-unused-function -I "$test_dir" -I "$out")
case "$(uname -s)" in
    Darwin) clang "${common[@]}" -fobjc-arc -framework Foundation \
        "$test_dir/test-display-write-order.m" -o "$out/test-display-write-order" ;;
    Linux) objc_lib=$(cc -print-file-name=libobjc.so.4)
        clang "${common[@]}" -fobjc-runtime=gcc "$test_dir/test-display-write-order.m" \
            "$objc_lib" -o "$out/test-display-write-order" ;;
    *) printf 'Unsupported host\n' >&2; exit 2 ;;
esac
"$out/test-display-write-order"
