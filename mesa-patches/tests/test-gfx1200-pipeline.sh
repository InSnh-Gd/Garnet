#!/usr/bin/env bash
# Use an existing native Darwin ICD; --submit also exercises mock queue acceptance.
set -euo pipefail
if [[ ${1:-} == --help ]]; then
    printf 'Usage: bash mesa-patches/tests/test-gfx1200-pipeline.sh MESA_SOURCE DYLIB OUTPUT_DIR [--submit|--query-hang|--wait-hang]\nCompiles gfx1200 shaders; --submit validates real RADV queue marshalling; --query-hang and --wait-hang check fault propagation. No GPU work executes.\n'
    exit 0
fi
[[ $# == 3 || ( $# == 4 && ( $4 == --submit || $4 == --query-hang || $4 == --wait-hang ) ) ]] || { printf 'Expected three arguments and an optional test mode; use --help.\n' >&2; exit 2; }
test_dir=$(cd "$(dirname "$0")" && pwd)
mesa=$(cd "${1:?usage: bash mesa-patches/tests/test-gfx1200-pipeline.sh MESA_SOURCE DYLIB OUTPUT_DIR}" && pwd)
dylib=${2:?native Darwin RADV dylib required}
out=${3:?output directory required}
[[ $(uname -s) == Darwin ]] || { echo 'Requires native macOS' >&2; exit 1; }
[[ -f "$dylib" ]] || { echo "Missing dylib: $dylib" >&2; exit 1; }
mkdir -p "$out"
out=$(cd "$out" && pwd)
cc=${CC:-clang}
glslang=${GLSLANG_VALIDATOR:-glslangValidator}
set -x
shasum -a256 "$dylib" "$test_dir/gfx1200_pipeline_test.c" "$test_dir/gfx1200_pipeline_test.comp"
"$cc" -std=gnu11 -O1 -g -Wall -Wextra -Werror -I"$mesa/include" \
    "$test_dir/gfx1200_pipeline_test.c" -o "$out/gfx1200-pipeline-test"
"$glslang" -V --target-env vulkan1.3 "$test_dir/gfx1200_pipeline_test.comp" -o "$out/owned-compute.spv"
mode=()
if [[ $# == 4 ]]; then mode=("$4"); fi
"$out/gfx1200-pipeline-test" "$dylib" "$out/owned-compute.spv" "$out" "${mode[@]}"
shasum -a256 "$out/gfx1200-pipeline-test" "$out/owned-compute.spv" "$out"/pipeline-ir-*.txt
