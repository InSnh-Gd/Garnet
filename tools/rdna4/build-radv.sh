#!/bin/bash
# Build the x86_64 RADV (libvulkan_radeon.dylib) the GarnetMetal bundle ships, from Mesa f5cb8ee plus mesa-patches/0001..0026.
#   build-radv.sh MESA_SRC OUT_DIR
# MESA_SRC: a pristine Mesa tree at f5cb8ee032adabef599ae892ec4e42d796b84da9 (a git checkout or a plain copy). It is not modified:
# the patches are applied to a copy in OUT_DIR/src (git apply --check first, so a series that does not apply stops here).
# Needs meson, ninja and python3 with mako + pyyaml + packaging on PATH (a venv is fine), glslangValidator, and the CLT.
# Prints the dylib path on the last line.
set -euo pipefail
[ "$#" -eq 2 ] || { echo "usage: build-radv.sh MESA_SRC OUT_DIR" >&2; exit 64; }
P=$(cd "$(dirname "$0")/../.." && pwd)
SRC=$(cd "$1" && pwd); OUT=$2
BASE=f5cb8ee032adabef599ae892ec4e42d796b84da9
if [ -d "$SRC/.git" ] || [ -f "$SRC/.git" ]; then
    [ "$(git -C "$SRC" rev-parse HEAD)" = "$BASE" ] || { echo "build-radv.sh: $SRC is not at $BASE" >&2; exit 1; }
fi
grep -q '^26\.3\.0-devel' "$SRC/VERSION" || { echo "build-radv.sh: $SRC/VERSION is not 26.3.0-devel" >&2; exit 1; }
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
rm -rf "$OUT/src" "$OUT/build"
mkdir -p "$OUT/src"
(cd "$SRC" && tar --exclude=.git -cf - .) | (cd "$OUT/src" && tar -xf -)
git -C "$OUT/src" init -q && git -C "$OUT/src" add -A >/dev/null && \
    git -C "$OUT/src" -c user.name=build -c user.email=build@localhost commit -q -m base
for f in "$P"/mesa-patches/*.patch; do
    git -C "$OUT/src" apply --check "$f" || { echo "build-radv.sh: ${f##*/} does not apply" >&2; exit 1; }
    git -C "$OUT/src" apply --index "$f"
    echo "applied ${f##*/}" >&2
done
( cd "$OUT/src" && meson setup "$OUT/build" -Dbuildtype=release -Dgallium-drivers= -Dvulkan-drivers=amd -Dplatforms=macos \
    -Dllvm=disabled -Damd-use-llvm=false -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dopengl=false -Dgles1=disabled \
    -Dgles2=disabled -Dspirv-tools=disabled -Dzstd=disabled -Dvalgrind=disabled -Dlibunwind=disabled -Dbuild-tests=false \
    -Dbuild-aco-tests=false >&2 )
ninja -C "$OUT/build" src/amd/vulkan/libvulkan_radeon.dylib >&2
LIB="$OUT/build/src/amd/vulkan/libvulkan_radeon.dylib"
[ "$(lipo -archs "$LIB")" = x86_64 ] || { echo "build-radv.sh: $LIB is not thin x86_64" >&2; exit 1; }
strings "$LIB" > "$OUT/strings.txt"
grep -q 'GarnetDriver' "$OUT/strings.txt" || { echo "build-radv.sh: $LIB does not name the GarnetDriver service" >&2; exit 1; }
if grep -Eq 'Navi48Bringup|RDNA4Bringup|AMDRadeonRDNA4' "$OUT/strings.txt"; then echo "build-radv.sh: $LIB still names retired kext symbols" >&2; exit 1; fi
echo "$LIB"
