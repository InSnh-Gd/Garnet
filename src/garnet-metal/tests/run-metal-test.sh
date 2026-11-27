#!/usr/bin/env bash
# Unified macOS builder/runner replacing standalone thin test-*.sh wrappers.
# Usage:
#   bash run-metal-test.sh [--vk-inc DIR] [--iokit] [--quartz] [--build-only] \
#       TEST_NAME OUTPUT_DIR [-- TEST_ARGS...]
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: bash run-metal-test.sh [OPTIONS] TEST_NAME OUTPUT_DIR [-- TEST_ARGS...]

Options:
  --vk-inc DIR    Add Vulkan header include path (-I DIR)
  --iokit         Link -framework IOKit -framework IOSurface
  --quartz        Link -framework QuartzCore -framework CoreGraphics
  --build-only    Compile without executing
  -h, --help      Show this help

TEST_NAME may omit the leading "test-" prefix and trailing ".m" suffix.
EOF
}

vk_inc=""
extra_flags=()
build_only=0

while [ "$#" -gt 0 ]; do
    case "$1" in
        -h|--help) usage; exit 0 ;;
        --vk-inc) vk_inc=${2:?--vk-inc requires a directory}; shift 2 ;;
        --iokit) extra_flags+=(-framework IOKit -framework IOSurface); shift ;;
        --quartz) extra_flags+=(-framework QuartzCore -framework CoreGraphics); shift ;;
        --build-only) build_only=1; shift ;;
        --) shift; break ;;
        -*) printf 'Unknown option: %s\n' "$1" >&2; usage >&2; exit 2 ;;
        *) break ;;
    esac
done

[ "$#" -ge 2 ] || { usage >&2; exit 2; }
[ "$(uname -s)" = Darwin ] || { printf 'run-metal-test.sh requires macOS.\n' >&2; exit 2; }

raw_name=$1
out_arg=$2
shift 2
if [ "${1:-}" = "--" ]; then shift; fi

test_dir=$(cd -- "$(dirname -- "$0")" && pwd)
source_dir=$(cd -- "$test_dir/.." && pwd)
name=${raw_name%.m}
case "$name" in test-*) ;; *) name="test-$name" ;; esac
src="$test_dir/$name.m"
[ -f "$src" ] || { printf 'Test source not found: %s\n' "$src" >&2; exit 1; }

mkdir -p -- "$out_arg"
out=$(cd -- "$out_arg" && pwd)
bin="$out/$name"

cmd=(clang -arch x86_64 -fobjc-arc -O1 -Wall -Wextra -Werror
     -I "$source_dir" -I "$test_dir"
     -framework Foundation -framework Metal)
[ -n "$vk_inc" ] && cmd+=(-I "$vk_inc")
[ "${#extra_flags[@]}" -gt 0 ] && cmd+=("${extra_flags[@]}")
cmd+=("$src" -o "$bin")

shasum -a 256 "$src" > "$out/fixture.sha256"
"${cmd[@]}" 2>&1 | tee "$out/build.log"
if [ "$build_only" -eq 0 ]; then
    "$bin" "$@" 2>&1 | tee "$out/result.log"
fi
