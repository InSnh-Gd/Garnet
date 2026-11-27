#!/bin/bash
set -euo pipefail
source_dir=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/rdna4dumpwrite.XXXXXX")
trap 'rm -rf -- "$work"' EXIT
python3 "$source_dir/assemble-device.py" "$source_dir/GarnetDevice.m" --output "$work/device.m"
python3 - "$work/device.m" "$work/writer.inc" <<'PY'
from pathlib import Path
import sys

source = Path(sys.argv[1]).read_text()
start = source.index('static int rdna4_dump_write(const char *path, NSData *data) {')
end = source.index("\n// Dumps one function's bitcodeData", start)
# Keep the production body; adapt only NSData's two read-only accessors for C.
body = source[start:end].replace('NSData *data', 'rdna4_test_data *data')
body = body.replace('data.length', 'data->length').replace('data.bytes', 'data->bytes')
Path(sys.argv[2]).write_text(body)
PY
"${CC:-cc}" -std=c11 -D_DEFAULT_SOURCE -O1 -g -Wall -Wextra -Werror -pthread \
    -DRDNA4_DUMP_WRITER="\"$work/writer.inc\"" "$source_dir/test-dump-write.c" -o "$work/test"
"$work/test" "$work"
