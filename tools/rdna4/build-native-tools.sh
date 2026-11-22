#!/bin/bash
# Build signed x86_64 userspace tools without generating files in the repository.
# Usage: build-native-tools.sh OUT_DIR
set -euo pipefail
fail() { printf 'build-native-tools.sh: %s\n' "$*" >&2; exit 1; }
[ "$#" -eq 1 ] && [ -n "$1" ] || fail 'Usage: build-native-tools.sh OUT_DIR'
[ "$(uname -s)" = Darwin ] || fail 'macOS is required'
repo=$(cd "$(dirname "$0")/../.." && pwd -P)

# Resolve even a not-yet-created output directory before allowing any writes.
resolve_output() {
    local path=$1 suffix=
    case "$path" in /*) ;; *) path="$PWD/$path";; esac
    while [ ! -d "$path" ]; do
        [ ! -e "$path" ] && [ ! -L "$path" ] || fail "Not a directory: $path"
        suffix="/$(basename "$path")$suffix"
        path=$(dirname "$path")
    done
    path=$(cd "$path" && pwd -P)
    # Reject traversal in the suffix rather than checking a misleading prefix.
    case "$suffix/" in */../*|*/./*) fail 'OUT_DIR must not contain unresolved . or .. components';; esac
    printf '%s%s\n' "${path%/}" "$suffix"
}
out=$(resolve_output "$1")
case "$out/" in "$repo/"*) fail 'OUT_DIR must be outside the repository';; esac
mkdir -p "$out"
out=$(cd "$out" && pwd -P)
export TMPDIR="$out/"
for command in clang codesign file; do
    command -v "$command" >/dev/null 2>&1 || fail "Required tool not found: $command"
done
scratch=$(mktemp -d "$out/.native-tools.XXXXXXXX")
trap 'rm -rf "$scratch"' EXIT
common=(-arch x86_64 -mmacosx-version-min=12.0 -O2 -Wall -Wextra)

build_tool() {
    local name=$1 source=$2 description
    shift 2
    if clang "${common[@]}" -Werror "$@" "$source" -o "$scratch/$name" >"$scratch/clang.log" 2>&1; then
        cat "$scratch/clang.log" >&2
    else
        cat "$scratch/clang.log" >&2
        # Only existing warning diagnostics justify relaxing -Werror for this file.
        grep -Eq '\[-Werror([,=]|\])|warnings? treated as errors' "$scratch/clang.log" || fail "Compilation failed: $source"
        printf 'build-native-tools.sh: %s: retrying without -Werror for this source only\n' "$source" >&2
        clang "${common[@]}" "$@" "$source" -o "$scratch/$name"
    fi
    codesign --force --sign - "$scratch/$name"
    codesign --verify --strict "$scratch/$name"
    description=$(file "$scratch/$name")
    printf '%s\n' "$description" >&2
    case "$description" in *x86_64*) ;; *) fail "$name is not x86_64";; esac
    mv -f "$scratch/$name" "$out/$name"
}

build_tool native-translate "$repo/tools/rdna4/native/native-translate.m" \
    -fobjc-arc -I "$repo/src/garnet-metal" -lproc -framework Foundation
build_tool native-bundle-check "$repo/tools/rdna4/native/native-bundle-check.m" \
    -fobjc-arc -framework Foundation
build_tool publish-metal "$repo/tools/rdna4/native/publish-metal.c" \
    -I "$repo/src/garnet/src" -framework IOKit -framework CoreFoundation
# Preserve the probe's ARC and framework flags; use the shared deployment/optimization flags.
build_tool mtlprobe-direct "$repo/tools/native/mtlprobe/mtlprobe-direct.m" \
    -fobjc-arc -framework Metal -framework Foundation -framework IOKit \
    -framework CoreGraphics -framework ImageIO -framework IOSurface
build_tool garnettest "$repo/tools/rdna4/native/garnettest.c" \
    -I "$repo/src/garnet/src" -framework IOKit -framework CoreFoundation
printf '%s\n' "$out"
