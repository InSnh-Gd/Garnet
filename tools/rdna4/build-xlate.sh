#!/bin/bash
# Build libgarnetxlate.dylib (x86_64, LGPL, third-party/metal2vulkan-rdna4) for the GarnetMetal bundle.
#   build-xlate.sh M2V_SRC OUT_DIR
# M2V_SRC: a clone of https://github.com/steelbrain/metal2vulkan at 43c46ac8a24adf1a6e872b8a52c706ec9614fad0; it is not
# modified (copied to OUT_DIR/src, patched there). Needs cargo with the x86_64-apple-darwin target. Prints the dylib path last.
set -euo pipefail
[ "$#" -eq 2 ] || { echo "usage: build-xlate.sh M2V_SRC OUT_DIR" >&2; exit 64; }
P=$(cd "$(dirname "$0")/../.." && pwd)
SRC=$(cd "$1" && pwd); OUT=$2
T="$P/third-party/metal2vulkan-rdna4"
[ "$(git -C "$SRC" rev-parse HEAD)" = 43c46ac8a24adf1a6e872b8a52c706ec9614fad0 ] || { echo "build-xlate.sh: $SRC is not at 43c46ac" >&2; exit 1; }
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
rm -rf "$OUT/src"; mkdir -p "$OUT/src"
(cd "$SRC" && tar --exclude=.git --exclude=target -cf - .) | (cd "$OUT/src" && tar -xf -)
for f in "$T"/patches/*.patch; do
    (cd "$OUT/src" && git apply --check "$f" && git apply "$f") || { echo "build-xlate.sh: ${f##*/} does not apply" >&2; exit 1; }
    echo "applied ${f##*/}" >&2
done
rm -rf "$OUT/src/garnetxlate" && cp -R "$T/garnetxlate" "$OUT/src/"
sed -i.bak 's/"validation", //' "$OUT/src/Cargo.toml" && rm -f "$OUT/src/Cargo.toml.bak"
( cd "$OUT/src" && MACOSX_DEPLOYMENT_TARGET=12.0 cargo build --release -p garnetxlate --target x86_64-apple-darwin >&2 )
LIB="$OUT/src/target/x86_64-apple-darwin/release/libgarnetxlate.dylib"
[ "$(lipo -archs "$LIB" | tr -d '[:space:]')" = x86_64 ] || { echo "build-xlate.sh: $LIB is not thin x86_64" >&2; exit 1; }
nm -gU "$LIB" > "$OUT/exports.txt"
bash "$P/tools/rdna4/xlate-guard.sh" write-manifest "$P" "$LIB"
bash "$P/tools/rdna4/xlate-guard.sh" verify "$P" "$LIB"
echo "$LIB"
