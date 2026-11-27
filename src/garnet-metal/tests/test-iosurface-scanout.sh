#!/bin/bash
set -euo pipefail
if [ "${1:-}" = --help ]; then
    printf 'Usage: sudo bash test-iosurface-scanout.sh VM_TEST_BUNDLE LOG_DIRECTORY\nBuilds a test-only bundle around the production source and checks linear IOSurface/scanout buffer aliases. RDNA4_VK_INC supplies Vulkan include flags. No display takeover.\n'
    exit 0
fi
[ "$#" -eq 2 ] || exit 2
[ "$(uname -s)" = Darwin ] && [ "$(id -u)" -eq 0 ] || exit 2
source_dir=$(cd "$(dirname "$0")" && pwd)
input=$(cd "$1" && pwd)
mkdir -p "$2"
logs=$(cd "$2" && pwd)
bundle="$logs/GarnetMetal-IOSAliasTest.bundle"
[ "$input" != "$bundle" ] || exit 2
ditto "$input" "$bundle"
clang ${RDNA4_VK_INC:--I/opt/homebrew/include} -arch x86_64 -mmacosx-version-min=12.0 \
    -fobjc-arc -O2 -Wall -Wextra -Werror -DRDNA4_IOS_SCANOUT_BUNDLE=1 -bundle \
    -framework Foundation -framework Metal -framework IOKit -framework IOSurface \
    "$source_dir/test-iosurface-scanout.m" -o "$bundle/Contents/MacOS/GarnetMetal" \
    > "$logs/build-bundle.txt" 2>&1
codesign --force --sign - "$bundle" > "$logs/sign-bundle.txt" 2>&1
clang -arch x86_64 -fobjc-arc -O1 -Wall -Wextra -Werror -framework Foundation \
    "$source_dir/test-iosurface-scanout.m" -o "$logs/test-iosurface-scanout" \
    > "$logs/build-runner.txt" 2>&1
"$logs/test-iosurface-scanout" "$bundle"
