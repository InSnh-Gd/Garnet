#!/usr/bin/env bash
# Compile the actual patched Mesa backend; first argument is an already-patched source tree.
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
test_dir=$(cd "$(dirname "$0")" && pwd)
mesa=$(cd "${1:?usage: bash mesa-patches/tests/test-gfx1200-runtime.sh PATCHED_MESA [OUTPUT_DIR]}" && pwd)
out=${2:-"${TMPDIR:-/tmp}/garnet-mesa-hosttest"}
mkdir -p "$out"
out=$(cd "$out" && pwd)
cc=${CC:-clang}
cxx=${CXX:-clang++}
flags=(-std=gnu11 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer
       -DHAVE_LIBDRM=1 -I"$mesa/include" -I"$mesa/include/darwin"
       -I"$mesa/src" -I"$mesa/src/amd" -I"$mesa/src/amd/common/darwin"
       -I"$mesa/src/amd/addrlib/src")
if [[ $(uname -s) == Linux ]]; then flags+=(-DHAVE_ENDIAN_H=1); fi
set -x
"$cc" "${flags[@]}" -Wall -Wextra -Werror -c "$test_dir/gfx1200_runtime_test.c" -o "$out/test.o"
objects=("$out/test.o")
for source in ac_darwin_drm ac_darwin_kext ac_darwin_mock ac_darwin_sync amdgpu_vamgr; do
    if [ "$source" = ac_darwin_sync ]; then
        "$cc" "${flags[@]}" -Drealloc=acd_test_sync_realloc -Wall -c "$mesa/src/amd/common/darwin/$source.c" -o "$out/$source.o"
    else
        "$cc" "${flags[@]}" -Wall -c "$mesa/src/amd/common/darwin/$source.c" -o "$out/$source.o"
    fi
    objects+=("$out/$source.o")
done
"$cc" "${flags[@]}" -Wall -c "$mesa/src/amd/common/amdgpu_devices.c" -o "$out/amdgpu_devices.o"
"$cc" -fsanitize=address,undefined "${objects[@]}" "$out/amdgpu_devices.o" -pthread -lm -o "$out/runtime-test"
"$cc" "${flags[@]}" -Wall -Wextra -Werror "$test_dir/gfx1200_submit_test.c" \
    "$out/ac_darwin_kext.o" -lm -o "$out/submit-test"
"$cc" "${flags[@]}" -Wall -c "$mesa/src/amd/common/darwin/ac_darwin_sync.c" -o "$out/ac_darwin_sync_plain.o"
"$cc" "${flags[@]}" -Wall -Wextra -Werror "$test_dir/dual_ring_sync_test.c" \
    "$out/ac_darwin_kext.o" "$out/ac_darwin_sync_plain.o" -pthread -lm -o "$out/dual-ring-sync-test"
"$cxx" -std=c++17 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"$repo/src/garnet/src" -I"$repo/src/garnet/src/amd" \
    "$test_dir/gfx1200_kernel_contract_test.cpp" -o "$out/kernel-contract-test"
export ASAN_OPTIONS=detect_leaks=1:halt_on_error=1
if [[ $(uname -s) == Darwin ]]; then export ASAN_OPTIONS=detect_leaks=0:halt_on_error=1; fi
export UBSAN_OPTIONS=halt_on_error=1
"$out/kernel-contract-test"
"$out/runtime-test"
"$out/submit-test"
"$out/dual-ring-sync-test"
