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
LAZY="${RDNA4_LAZY:-1}"; F9D="${RDNA4_9D:-1}"; RELEASE="${RDNA4_RELEASE:-0}"
RADV="${RDNA4_RADV_LIB:-}"   # x86_64 RADV dylib shipped in Resources
[ -n "$RADV" ] || { echo "build.sh: set RDNA4_RADV_LIB=/path/to/libvulkan_radeon.dylib" >&2; exit 1; }
[ -f "$RADV" ] || { echo "build.sh: RADV dylib not found: $RADV" >&2; exit 1; }
# VM backends can have the same filename and ICD entry point; never package them for the PC.
nm -arch x86_64 -gU "$RADV" > "$W/radv-exports.txt"
for symbol in radv_darwin_set_service_class radv_darwin_get_argument_limits_v1 \
              radv_darwin_buffer_copy radv_darwin_buffer_fill radv_darwin_buffer_wait; do
  awk -v want="_$symbol" '$NF == want { found=1 } END { exit !found }' "$W/radv-exports.txt" || \
    { echo "build.sh: not a production RADV: missing $symbol in $RADV" >&2; exit 1; }
done
# libgarnetxlate.dylib (x86_64): LGPL in-process Metal AIR -> SPIR-V translator.
# Pass a prebuilt library via RDNA4_XLATE_LIB, or set RDNA4_M2V_DIR to build garnetxlate via Cargo.
XL="${RDNA4_XLATE_LIB:-}"
if [ -z "$XL" ]; then
  M2V="${RDNA4_M2V_DIR:-}"
  [ -n "$M2V" ] && [ -f "$M2V/garnetxlate/Cargo.toml" ] || { echo "build.sh: set RDNA4_XLATE_LIB or RDNA4_M2V_DIR (containing garnetxlate/Cargo.toml)" >&2; exit 1; }
  ( cd "$M2V" && PATH="/opt/homebrew/opt/rustup/bin:/opt/homebrew/bin:$PATH" MACOSX_DEPLOYMENT_TARGET=12.0 cargo build --release -p garnetxlate --target x86_64-apple-darwin >&2 ) || { echo "build.sh: cargo build of garnetxlate failed" >&2; exit 1; }
  XL="$M2V/target/x86_64-apple-darwin/release/libgarnetxlate.dylib"
fi
[ -f "$XL" ] || { echo "build.sh: libgarnetxlate.dylib not found: $XL" >&2; exit 1; }
[ "$(lipo -archs "$XL" 2>/dev/null)" = "x86_64" ] || { echo "build.sh: $XL is not a thin x86_64 library" >&2; exit 1; }
bash "$D/../../tools/rdna4/xlate-guard.sh" verify "$D/../.." "$XL" || exit 1
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
GARNET_UNITS=(
  garnet_backend
  garnet_resources
  garnet_surface
  garnet_shaders
  garnet_pipelines
  garnet_commands
  garnet_arguments
  garnet_icb
  garnet_sparse_io
  garnet_mesh_rt
  garnet_device
)
compile_modular() {
  local arch="$1" objdir="$2" out="$3" minver_flag="$4"
  mkdir -p "$objdir"
  local pids=() objs=()
  for u in "${GARNET_UNITS[@]}"; do
    local obj="$objdir/$u.o"
    objs+=("$obj")
    clang $VKINC -arch "$arch" ${=minver_flag} -fobjc-arc -O2 -Wall -Wextra -Werror \
          -fvisibility=hidden -DGARNET_METAL_MODULAR_BUILD=1 -DRDNA4_LAZY=$LAZY -DRDNA4_9D=$F9D -DRDNA4_RELEASE=$RELEASE \
          -I"$D" -c "$D/$u.m" -o "$obj" &
    pids+=($!)
  done
  local fail=0
  for p in "${pids[@]}"; do
    wait "$p" || fail=1
  done
  [ "$fail" -eq 0 ] || { echo "build.sh: modular compile failed for arch=$arch" >&2; exit 1; }
  clang -arch "$arch" ${=minver_flag} -fobjc-arc -bundle \
        -framework Foundation -framework Metal -framework IOKit -framework IOSurface \
        -o "$out" "${objs[@]}"
}
compile_modular x86_64 "$W/obj-x86_64" "$APP/Contents/MacOS/GarnetMetal" "-mmacosx-version-min=12.0"
xattr -cr "$APP"
codesign --force --sign - "$APP/Contents/Resources/libvulkan_radeon.dylib"
codesign --force --sign - "$APP/Contents/Resources/libgarnetxlate.dylib"
codesign --force --sign - "$APP"
# arm64 build of the same source: compile/link sanity only (there is no Rosetta here to load x86_64); never install it.
compile_modular arm64 "$W/obj-arm64" "$B/sanity/GarnetMetal-arm64.NOT-SHIPPED" ""
codesign --verify --strict -v "$APP"
tar --no-xattrs --no-mac-metadata -C "$W" -cf "$B/GarnetMetal.bundle.tar" GarnetMetal.bundle 2>/dev/null || \
  tar --no-xattrs -C "$W" -cf "$B/GarnetMetal.bundle.tar" GarnetMetal.bundle
file "$APP/Contents/MacOS/GarnetMetal"
echo "built (lazy=$LAZY 9d=$F9D): $APP  and  $B/GarnetMetal.bundle.tar ; install: see INSTALL.md"
