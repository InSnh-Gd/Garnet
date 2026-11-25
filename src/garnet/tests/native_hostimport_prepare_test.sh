#!/bin/bash
set -euo pipefail
if [ "${1:-}" = --help ]; then
    printf 'Usage: native_hostimport_prepare_test.sh OUTPUT_DIR [NATIVE_S1C_CPP]\nExercises the production descriptor wiring block against XNU direction/protection semantics.\n'
    exit 0
fi
[ "$#" -ge 1 ] && [ "$#" -le 2 ] || exit 2
tests=$(cd "$(dirname "$0")" && pwd)
source_file=${2:-"$tests/../src/amd/garnet_vram_alloc.cpp"}
mkdir -p "$1"
out=$(cd "$1" && pwd)
awk '/IOMemoryDescriptor \*md = IOMemoryDescriptor::withAddressRange/ { copying=1 }
     copying && /DescSeg seg \{ md \};/ { exit }
     copying { print }' "$source_file" > "$out/host_import_prepare.inc"
[ -s "$out/host_import_prepare.inc" ] || exit 1
flags=()
if grep -q 'uint8_t hro' "$out/host_import_prepare.inc"; then flags+=(-DRDNA4_TEST_HAS_READONLY_IMPORT); fi
${CXX:-c++} -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -fno-omit-frame-pointer "${flags[@]}" -I "$out" "$tests/native_hostimport_prepare_test.cpp" -o "$out/test-host-import-prepare"
"$out/test-host-import-prepare"
