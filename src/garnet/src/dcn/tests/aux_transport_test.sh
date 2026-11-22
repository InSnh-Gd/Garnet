#!/bin/bash
set -euo pipefail
repo_root=$(cd "$(dirname "$0")/../../../../.." && pwd)
workspace=$(cd "$repo_root/.." && pwd)
output=${1:?pass evidence directory}
mkdir -p "$output"
output=$(cd "$output" && pwd)
dcn="$repo_root/src/garnet/src/dcn"
python3 - "$workspace/EFI-7600X-B650M-Tahoe/spare/RDNA4FB-5ff69aa/src/framebuffer.cpp" "$output" <<'PY'
from pathlib import Path
import sys, hashlib
path, out = map(Path, sys.argv[1:])
s = path.read_text()
start = s.index('namespace {\nconstexpr uint8_t kActI2CWriteMot')
end = s.index('void RDNA4FB::probeEDID()', start)
power = s.index('void RDNA4FB::setDisplayPower(bool on)')
power_end = s.index('// ---------------------------------------------------------------------------', power)
(out/'fb_aux_actual.inc').write_text(s[start:end]+s[power:power_end])
print(f'{path}: SHA256={hashlib.sha256(path.read_bytes()).hexdigest()}')
PY
python3 - "$dcn/dcn.cpp" "$output" <<'PYCODE'
from pathlib import Path
import sys, hashlib
path, out = map(Path, sys.argv[1:])
s = path.read_text()
a = s.index('uint32_t dpConnectorSnapshot(')
b = s.index('uint32_t dpSnapshot(', a)
(out/'dpcd_bridge_actual.inc').write_text(s[a:b])
c = s.index('static void dp_log_preflight(', b)
(out/'dpcd_snapshot_actual.inc').write_text(s[b:c])
print(f'{path}: SHA256={hashlib.sha256(path.read_bytes()).hexdigest()}')
PYCODE
printf '#include "dp_binding.h"\n_Static_assert(sizeof(rdna4_dpcd_request)==32, "request ABI");\n_Static_assert(sizeof(rdna4_dpcd_snapshot)==80, "snapshot ABI");\n_Static_assert(sizeof(rdna4_bound_request)==40, "bound request ABI");\n_Static_assert(sizeof(rdna4_bound_snapshot)==120, "bound snapshot ABI");\nint main(void) { return 0; }\n' > "$output/contract.c"
clang -std=c11 -Wall -Wextra -Werror -I "$dcn" "$output/contract.c" -o "$output/contract_c"
"$output/contract_c"
echo 'plain C contract ABI: PASS (request 32, snapshot 80 bytes)'
for suite in aux_transport fb_aux dpcd_bridge binding; do
    extra=()
    if [ "$suite" = dpcd_bridge ]; then extra=(-x c++ "$repo_root/src/dcn41/dcn41_core.c"); fi
    clang++ -std=c++17 -Wall -Wextra -Werror -O1 -g -pthread \
        -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
        -I "$dcn" -I "$output" "$dcn/tests/${suite}_test.cpp" -I "$repo_root/src/dcn41" "${extra[@]}" -o "$output/${suite}_test"
    ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1 "$output/${suite}_test"
done
