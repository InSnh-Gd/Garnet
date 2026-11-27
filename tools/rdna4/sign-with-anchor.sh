#!/bin/bash
# Sign a standalone Mach-O binary or a .bundle (inner dylibs first, then bundle seal)
# with an AMFIAnchor-compatible certificate chain via rcodesign.
# Usage: bash tools/rdna4/sign-with-anchor.sh <CA_DIR> <TARGET_PATH>
set -euo pipefail

fail() { printf 'sign-with-anchor.sh: %s\n' "$*" >&2; exit 1; }
[ "$#" -eq 2 ] || fail 'Usage: sign-with-anchor.sh <CA_DIR> <TARGET_PATH>'

ca_dir=$1
target=$2
RCODESIGN=${RCODESIGN:-rcodesign}

command -v "$RCODESIGN" >/dev/null 2>&1 || fail "Required tool not found: $RCODESIGN (install rcodesign >= 0.29.0)"
for f in leaf.key.pem leaf.cert.pem intermediate.cert.pem root.cert.pem; do
    [ -f "$ca_dir/$f" ] && [ ! -L "$ca_dir/$f" ] || fail "Missing signing material: $ca_dir/$f"
done
[ -e "$target" ] && [ ! -L "$target" ] || fail "Target missing or symlink: $target"

sign_one() {
    local item=$1
    local parent base tmp
    parent=$(dirname "$item")
    base=$(basename "$item")
    tmp=$(mktemp -d "${TMPDIR:-/tmp}/garnet-sign.XXXXXXXX")
    trap 'rm -rf "$tmp"' RETURN
    "$RCODESIGN" sign -C /dev/null --timestamp-url none \
        --pem-file "$ca_dir/leaf.key.pem" \
        --pem-file "$ca_dir/leaf.cert.pem" \
        --pem-file "$ca_dir/intermediate.cert.pem" \
        --pem-file "$ca_dir/root.cert.pem" \
        "$item" "$tmp/$base"
    "$RCODESIGN" verify "$tmp/$base" >/dev/null || fail "rcodesign verify failed: $item"
    rm -rf "$item"
    mv "$tmp/$base" "$parent/$base"
}

if [ -d "$target" ]; then
    # Sign inner dylibs first so outer bundle CodeResources seals stay valid.
    if [ -d "$target/Contents/Resources" ]; then
        while IFS= read -r -d '' dylib; do
            sign_one "$dylib"
        done < <(find "$target/Contents/Resources" -maxdepth 2 -type f -name '*.dylib' -print0)
    fi
    sign_one "$target"
else
    sign_one "$target"
fi
