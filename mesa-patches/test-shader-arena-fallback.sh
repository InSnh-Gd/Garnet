#!/usr/bin/env bash
set -euo pipefail
[ "$#" -eq 2 ] || { printf 'Usage: %s MESA_SOURCE OUTPUT_DIR\n' "$0" >&2; exit 2; }
source_file="$1/src/amd/vulkan/radv_shader.c"
fixture=$(cd "$(dirname "$0")" && pwd)/shader_arena_fallback_test.c
mkdir -p "$2"
sed -n '/^static struct radv_shader_arena \*$/ { N; /\nradv_create_shader_arena(/ { :function; p; n; /^}$/ { p; b; }; b function; }; }' \
    "$source_file" > "$2/shader_arena_production.inc"
for backend in darwin other; do
    flags=()
    if [ "$backend" = darwin ]; then flags+=(-DHAVE_AMDGPU_DARWIN); fi
    "${CC:-cc}" -std=c11 -Wall -Wextra -Werror "${flags[@]}" -I "$2" "$fixture" -o "$2/shader-arena-$backend"
    "$2/shader-arena-$backend"
done
