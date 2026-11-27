#!/bin/bash
set -euo pipefail

if [ "${1:-}" = --help ]; then
    printf 'Usage: sudo env RDNA4_VK_INC="-I/path/to/vulkan/include" bash test-execution.sh VM_TEST_BUNDLE RADV_BUNDLE LOG_DIRECTORY\nBuilds and runs pixel/compute/Core Image comparisons in the macOS VM. Requires an isolated MoltenVK test bundle; native GPU transport is not tested.\n'
    exit 0
fi
[ "$#" -eq 3 ] || { printf 'Expected VM_TEST_BUNDLE RADV_BUNDLE LOG_DIRECTORY\n' >&2; exit 2; }
[ "$(uname -s)" = Darwin ] && [ "$(id -u)" -eq 0 ] || { printf 'Run as root inside macOS.\n' >&2; exit 2; }
source_dir=$(cd "$(dirname "$0")" && pwd)
test_bundle=$(cd "$1" && pwd)
radv_bundle=$(cd "$2" && pwd)
mkdir -p "$3"
logs=$(cd "$3" && pwd)
mkdir -p "$logs/ci-reference"
clang -arch x86_64 -g -fobjc-arc -Wall -Wextra -Werror \
    -framework Foundation -framework CoreGraphics -framework CoreImage -framework Metal -framework IOSurface \
    "$source_dir/test-execution.m" -o "$logs/test-execution" > "$logs/build-execution.txt" 2>&1
clang -arch x86_64 -g -fobjc-arc -O1 \
    -framework Metal -framework Foundation -framework IOKit -framework CoreGraphics -framework ImageIO -framework IOSurface \
    "$source_dir/test-execution-probe.m" -o "$logs/test-execution-probe" > "$logs/build-probe.txt" 2>&1
clang -arch x86_64 -fobjc-arc -Wall -Wextra -Werror -framework Foundation -framework Metal -framework IOSurface \
    "$source_dir/test-texture-view.m" -o "$logs/test-texture-view"
clang -arch x86_64 -fobjc-arc -Wall -Wextra -Werror -framework Foundation -framework Metal \
    "$source_dir/test-indirect.m" -o "$logs/test-indirect"
clang -arch x86_64 -fobjc-arc -Wall -Wextra -Werror -framework Foundation -framework Metal \
    "$source_dir/test-indirect-compute.m" -o "$logs/test-indirect-compute"
clang -arch x86_64 -fobjc-arc -Wall -Wextra -Werror -framework Foundation -framework Metal \
    "$source_dir/test-render-fallback.m" -o "$logs/test-render-fallback"
clang -arch x86_64 -fobjc-arc -Wall -Wextra -Werror -framework Foundation -framework Metal -framework IOSurface \
    "$source_dir/test-surface-capture.m" -o "$logs/test-surface-capture"
clang -arch x86_64 -fobjc-arc -Wall -Wextra -Werror -framework Foundation -framework Metal -framework IOSurface \
    "$source_dir/test-texture-host-access.m" -o "$logs/test-texture-host-access"
clang -arch x86_64 -fobjc-arc -Wall -Wextra -Werror -framework Foundation -framework Metal \
    "$source_dir/test-blit-subresources.m" -o "$logs/test-blit-subresources"
failures=0
cases=0
run_case() {
    local name=$1 result
    shift
    cases=$((cases + 1))
    mkdir -p "$logs/cache/$name"
    if env RDNA4M_TEST_USERCACHE="$logs/cache/$name" "$@" > "$logs/$name.txt" 2>&1; then
        printf 'EXECUTION_CASE_PASS %s\n' "$name"
    else
        result=$?
        failures=$((failures + 1))
        printf 'EXECUTION_CASE_FAIL %s exit=%s log=%s\n' "$name" "$result" "$logs/$name.txt"
    fi
}
run_case reference-differential "$logs/test-execution" --reference "$logs/ci-reference"
run_case native-differential "$logs/test-execution" "$test_bundle" "$logs/ci-reference"
run_case native-render-fallback "$logs/test-render-fallback" "$test_bundle" "$logs/cache/native-render-fallback"
run_case native-no-backend "$logs/test-execution" "$radv_bundle" --expect-no-backend
run_case surface-capture "$logs/test-surface-capture" "$test_bundle"
run_case texture-host-readonly "$logs/test-texture-host-access" "$test_bundle" readonly
run_case texture-host-iosurface "$logs/test-texture-host-access" "$test_bundle" iosurface
run_case native-iosurface-scanout bash "$source_dir/test-iosurface-scanout.sh" "$test_bundle" "$logs/iosurface-scanout"
run_case reference-texture-view "$logs/test-texture-view" --reference
run_case native-texture-view "$logs/test-texture-view" "$test_bundle"
for mode in draw indexed multi zero; do
    run_case "reference-indirect-$mode" "$logs/test-indirect" --reference "$mode"
    run_case "native-indirect-$mode" "$logs/test-indirect" "$test_bundle" "$mode"
done
for mode in bad-offset bad-size bad-index; do
    run_case "native-indirect-$mode" "$logs/test-indirect" "$test_bundle" "$mode"
done
for mode in grid generated zero; do
    run_case "reference-indirect-compute-$mode" "$logs/test-indirect-compute" --reference "$mode"
    run_case "native-indirect-compute-$mode" "$logs/test-indirect-compute" "$test_bundle" "$mode"
done
for mode in bad-offset bad-size bad-local; do
    run_case "native-indirect-compute-$mode" "$logs/test-indirect-compute" "$test_bundle" "$mode"
done
for mode in whole-array whole-upsize whole-downsize range-array whole-3d range-3d whole-cube range-cube alias-array sample-cube render-cube; do
    run_case "reference-blit-$mode" "$logs/test-blit-subresources" --reference "$mode"
    run_case "native-blit-$mode" "$logs/test-blit-subresources" "$test_bundle" "$mode"
done
for mode in bad-slice bad-level bad-count bad-size bad-format bad-overlap bad-origin; do
    run_case "native-blit-$mode" "$logs/test-blit-subresources" "$test_bundle" "$mode"
done
for command in quad blend compute sysdraw cube depth msaa rg16uint qcluma skydraw cull passbreak fbfetch fbsover fbcopy fbwithin iosurface ioalias fetch-builtin-guards windowserver-lut; do
    run_case "reference-$command" "$logs/test-execution-probe" --reference "$command"
    run_case "native-$command" "$logs/test-execution-probe" "$test_bundle" "$command"
done
printf 'EXECUTION_SUITE cases=%s failures=%s logs=%s native_hardware_transport=untested\n' "$cases" "$failures" "$logs"
[ "$failures" -eq 0 ]
