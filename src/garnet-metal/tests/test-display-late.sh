#!/usr/bin/env bash
set -euo pipefail
test_dir=$(cd -- "$(dirname -- "$0")" && pwd)
source_dir=${1:-$(cd -- "$test_dir/.." && pwd)}
out=${2:-$(mktemp -d "${TMPDIR:-/tmp}/rdna4-display-late.XXXXXX")}
mkdir -p "$out"
python3 "$source_dir/assemble-device.py" "$source_dir/GarnetDevice.m" --output "$out/device.m"
python3 - "$out" <<'PY'
from pathlib import Path
import hashlib
import sys
out = Path(sys.argv[1])
text = (out / 'device.m').read_text()
methods = out / 'methods.inc'
methods.write_text('')
for method in ('rdna4DispDone:(VkResult)r', 'rdna4DispSeq', 'rdna4DisplayCommit', 'rdna4DisplayUncommit',
               'rdna4CBRead:(RDNA4Texture *)t',
               'rdna4NoteSurfaceOverwrite:(RDNA4Texture *)texture width:(uint32_t)width height:(uint32_t)height level:(uint32_t)level',
               'rdna4NoteHostWriteback:(RDNA4Texture *)texture', 'rdna4DispSourceWrites:(RDNA4CommandSegment *)segment'):
    needle = '- (void)' + method + ' {'
    if needle not in text:
        continue
    start = text.index(needle)
    end = text.index('\n}', start) + 2
    with methods.open('a') as f:
        f.write(text[start:end] + '\n')
print('Assembled source SHA256:', hashlib.sha256(text.encode()).hexdigest())
if '[self rdna4DisplayCommit];' in text:
    assert text.index('[self rdna4DisplayCommit];', text.index('- (void)commit {')) < text.index('[super commit];', text.index('- (void)commit {'))
if '- (void)rdna4DispSourceWrites:' in text:
    assert '[self rdna4DispSourceWrites:segment];\n        if (_shaderFallback' in text
if 'if (_dlate)' not in (out / 'methods.inc').read_text():
    (out / 'legacy.h').write_text('#define TEST_LEGACY 1\n')
else:
    (out / 'legacy.h').write_text('')
PY
clang -std=gnu11 -fobjc-arc -Wall -Wextra -Werror -Wno-unused-function \
    -I "$out" -I "$test_dir" -include "$out/legacy.h" \
    -framework Foundation -framework IOSurface "$test_dir/test-display-late.m" -o "$out/test-display-late"
"$out/test-display-late"
