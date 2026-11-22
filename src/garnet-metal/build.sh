#!/usr/bin/env zsh
# Build GarnetMetal.bundle (x86_64, ad-hoc signed) into src/garnet-metal/build/.
#   build.sh            -> build/GarnetMetal.bundle (x86_64, for the PC) + arm64 compile/link sanity build (NOT shipped)
#   RDNA4_LAZY=0 build.sh -> 9e variant (supportLazyInitialization not YES)
#   RDNA4_9D=0  build.sh  -> without the 9d feature overrides
set -euo pipefail
D="$(cd "$(dirname "$0")" && pwd)"
# Work dir is OUTSIDE the repo: the repo lives in a file-provider folder that stamps com.apple.FinderInfo on
# *.bundle directories, which codesign --verify rejects ("detritus not allowed").  The shippable artefact is
# build/GarnetMetal.bundle.tar (bsdtar --no-xattrs); extract it on the PC.
B="$D/build"; W="$(mktemp -d "${TMPDIR:-/tmp}/garnetmetal.XXXXXX")"; APP="$W/GarnetMetal.bundle"
LAZY="${RDNA4_LAZY:-1}"; F9D="${RDNA4_9D:-1}"
RADV="${RDNA4_RADV_LIB:-$HOME/rdna4-native/mesa-mac/build-x86_64/src/amd/vulkan/libvulkan_radeon.dylib}"   # x86_64 RADV (10a: shipped in Resources)
[ -f "$RADV" ] || { echo "build.sh: RADV dylib not found: $RADV" >&2; exit 1; }
# VM backends can have the same filename and ICD entry point; never package them for the PC.
nm -arch x86_64 -gU "$RADV" > "$W/radv-exports.txt"
for symbol in radv_darwin_set_service_class radv_darwin_get_argument_limits_v1 \
              radv_darwin_buffer_copy radv_darwin_buffer_fill radv_darwin_buffer_wait; do
  awk -v want="_$symbol" '$NF == want { found=1 } END { exit !found }' "$W/radv-exports.txt" || \
    { echo "build.sh: not a production RADV: missing $symbol in $RADV" >&2; exit 1; }
done
# Bundle 14: libgarnetxlate.dylib (x86_64), the LGPL in-process translator of the fork (NATIVE-S8-INPROC.md). RDNA4_XLATE_LIB takes a prebuilt one; otherwise it is built from the fork
# (branch rdna4-inproc of ~/rdna4-native/metal2vulkan, or RDNA4_M2V_DIR) with `cargo build --release -p garnetxlate --target x86_64-apple-darwin`.
XL="${RDNA4_XLATE_LIB:-}"
if [ -z "$XL" ]; then
  M2V="${RDNA4_M2V_DIR:-$HOME/rdna4-native/metal2vulkan}"
  [ -f "$M2V/garnetxlate/Cargo.toml" ] || { echo "build.sh: the fork with garnetxlate is not at $M2V (set RDNA4_M2V_DIR or RDNA4_XLATE_LIB)" >&2; exit 1; }
  ( cd "$M2V" && PATH="/opt/homebrew/opt/rustup/bin:/opt/homebrew/bin:$PATH" MACOSX_DEPLOYMENT_TARGET=12.0 cargo build --release -p garnetxlate --target x86_64-apple-darwin >&2 ) || { echo "build.sh: cargo build of garnetxlate failed" >&2; exit 1; }
  XL="$M2V/target/x86_64-apple-darwin/release/libgarnetxlate.dylib"
fi
[ -f "$XL" ] || { echo "build.sh: libgarnetxlate.dylib not found: $XL" >&2; exit 1; }
[ "$(lipo -archs "$XL" 2>/dev/null)" = "x86_64" ] || { echo "build.sh: $XL is not a thin x86_64 library" >&2; exit 1; }
VKINC="${RDNA4_VK_INC:--I/opt/homebrew/include}"
rm -rf "$B"; mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources" "$B/sanity"
cp "$D/Info.plist" "$APP/Contents/Info.plist"
cp "$RADV" "$APP/Contents/Resources/libvulkan_radeon.dylib"
cp "$XL" "$APP/Contents/Resources/libgarnetxlate.dylib"
mkdir -p "$APP/Contents/Resources/spvcache"
cache_files=("$D"/spvcache/*.spv(N) "$D"/spvcache/*.meta.json(N))
for f in "${cache_files[@]}"; do cat "$f" > "$APP/Contents/Resources/spvcache/${f:t}"; done
if [ ${#cache_files[@]} -eq 0 ]; then
  echo "build.sh: no precompiled shader cache; applications need in-process translation, WindowServer cache is unprimed" >&2
fi
clang $VKINC -arch x86_64 -mmacosx-version-min=12.0 -fobjc-arc -O2 -Wall -Wextra -Werror \
      -DRDNA4_LAZY=$LAZY -DRDNA4_9D=$F9D -I"$D" -bundle -framework Foundation -framework Metal -framework IOKit -framework IOSurface \
      -o "$APP/Contents/MacOS/GarnetMetal" "$D/GarnetDevice.m"
xattr -cr "$APP"
codesign --force --sign - "$APP/Contents/Resources/libvulkan_radeon.dylib"
codesign --force --sign - "$APP/Contents/Resources/libgarnetxlate.dylib"
codesign --force --sign - "$APP"
# arm64 build of the same source: compile/link sanity only (there is no Rosetta here to load x86_64); never install it.
clang $VKINC -arch arm64 -fobjc-arc -O2 -Wall -Wextra -Werror -DRDNA4_LAZY=$LAZY -DRDNA4_9D=$F9D \
      -I"$D" -bundle -framework Foundation -framework Metal -framework IOKit -framework IOSurface -o "$B/sanity/GarnetMetal-arm64.NOT-SHIPPED" "$D/GarnetDevice.m"
codesign --verify --strict -v "$APP"
tar --no-xattrs --no-mac-metadata -C "$W" -cf "$B/GarnetMetal.bundle.tar" GarnetMetal.bundle 2>/dev/null || \
  tar --no-xattrs -C "$W" -cf "$B/GarnetMetal.bundle.tar" GarnetMetal.bundle
file "$APP/Contents/MacOS/GarnetMetal"
echo "built (lazy=$LAZY 9d=$F9D): $APP  and  $B/GarnetMetal.bundle.tar ; install: see INSTALL.md"
