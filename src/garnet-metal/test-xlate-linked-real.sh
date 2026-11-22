#!/usr/bin/env zsh
# test-xlate-linked-real.sh - P3 (build 18) end-to-end on REAL AIR: Apple's compiler makes a fragment shader that calls a [[visible]] function from another library; the pipeline's linkedFunctions would link it.
# The AIR comes out of the host Mac's real Metal (MTLFunction.bitcodeData), goes through the OS's GPUCompiler LLVM reader and the HOST build of libgarnetxlate (aarch64: `cargo build --release -p garnetxlate` in the fork).
#   usage (host Mac only): src/garnet-metal/test-xlate-linked-real.sh [path to the host libgarnetxlate.dylib]
set -u
D="$(cd "$(dirname "$0")" && pwd)"
XL="${1:-$HOME/rdna4-native/metal2vulkan/target/release/libgarnetxlate.dylib}"
[ -f "$XL" ] || { echo "no libgarnetxlate at $XL (build it: cargo build --release -p garnetxlate)"; exit 2; }
W="$(mktemp -d "${TMPDIR:-/tmp}/rdna4real.XXXXXX")"; case "$W" in */rdna4real.??????) ;; *) echo bad scratch; exit 2;; esac
trap 'rm -rf -- "$W"' EXIT
clang -fobjc-arc -framework Metal -framework Foundation -o "$W/gen" "$D/test-xlate-linked-real.m" 2>"$W/cc.err" || { cat "$W/cc.err"; exit 2; }
clang -O1 -Wall -Wextra -Werror -I"$D" -o "$W/real" "$D/test-xlate-linked-real.c" 2>"$W/cc2.err" || { cat "$W/cc2.err"; exit 2; }
"$W/gen" "$W" 2>&1 | grep -v "^20" ; [ -f "$W/fs.air" ] && [ -f "$W/shade.air" ] || { echo "no AIR produced"; exit 2; }
"$W/real" "$XL" "$W/fs.air" "$W/shade.air" shade
