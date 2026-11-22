#!/bin/bash
set -euo pipefail
test_dir=$(cd "$(dirname "$0")" && pwd)
source_dir=${1:-$test_dir}
out=${2:?Expected source directory and output directory}
mkdir -p "$out"
out=$(cd "$out" && pwd)
python3 - "$source_dir" "$out" <<'PY'
from pathlib import Path
import sys
source, out = map(Path, sys.argv[1:])
text = (source / 'rdna4_backend_impl.inc').read_text()
start = text.index(' rdna4_pc_derive_key(void) {')
start = text.rfind('\n', 0, start) + 1
end = text.index('\n}', start) + 2
(out / 'derive-key.inc').write_text(text[start:end] + '\n')
PY
for build in first second; do
    clang -fobjc-arc -Wall -Wextra -Werror -Wno-deprecated-declarations -Wno-builtin-macro-redefined \
        '-D__DATE__="Nov 10 2026"' "-D__TIME__=\"$build\"" -I "$out" \
        -framework Foundation "$test_dir/test-pipeline-cache-key.m" -o "$out/$build"
done
python3 - "$out" <<'PY'
from pathlib import Path
import os
import subprocess
import sys
out = Path(sys.argv[1]); resources = out / 'resources'; resources.mkdir(exist_ok=True)
backend = resources / 'libvulkan_radeon.dylib'
backend.write_bytes(b'one backend identity')
stamp = backend.stat().st_mtime_ns
def key(build):
    return subprocess.check_output([str(out / build), str(resources)], text=True).strip()
first, second = key('first'), key('second')
backend.write_bytes(b'two backend identity')
os.utime(backend, ns=(stamp, stamp))
changed = key('first')
backend.unlink()
missing = subprocess.run([str(out / 'first'), str(resources)], stdout=subprocess.PIPE).returncode
checks = [('bundle rebuild reuses unchanged backend cache', first == second),
          ('same-size same-mtime backend replacement invalidates cache', first != changed),
          ('unreadable backend disables cache instead of inventing an identity', missing == 3)]
for name, okay in checks:
    print(('PASS ' if okay else 'FAIL ') + name)
print('PIPELINE_CACHE_KEY_' + ('PASS' if all(ok for _, ok in checks) else 'FAIL'))
sys.exit(0 if all(ok for _, ok in checks) else 1)
PY
