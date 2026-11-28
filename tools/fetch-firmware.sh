#!/bin/bash
#
# fetch-firmware.sh - copy the AMD firmware Garnet.kext embeds from a local
# linux-firmware checkout into src/garnet/firmware/ (gitignored; never commit it).
#
# This script does NOT use the network. Clone linux-firmware yourself first:
#     git clone https://gitlab.com/kernel-firmware/linux-firmware.git
#   (mirror: https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git)
#
# Usage:  tools/fetch-firmware.sh <path-to-linux-firmware-checkout>
#         CHIPS="navi44" tools/fetch-firmware.sh <path>     (default CHIPS="navi48 navi44", as in the Makefile)
#
# The blobs are AMD's, under AMD's redistribution licence (LICENSE.amdgpu in linux-firmware).
set -euo pipefail

if [ $# -ne 1 ] || [ ! -d "$1" ]; then
    echo "usage: $0 <path-to-linux-firmware-checkout>" >&2
    echo "clone it first: git clone https://gitlab.com/kernel-firmware/linux-firmware.git" >&2
    exit 2
fi
SRC="$1/amdgpu"
[ -d "$SRC" ] || { echo "fetch-firmware.sh: no amdgpu/ directory in $1" >&2; exit 2; }

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DST="$REPO/src/garnet/firmware"
mkdir -p "$DST"

CHIPS="${CHIPS:-navi48 navi44}"

# chip  name  sha256 of the copy this project was developed against (a different linux-firmware
# revision may differ: a mismatch is a warning, not an error). The Navi 44 hashes are linux-firmware
# bda6eccc3a8f (2026-10-31; parsed offline, NOT yet run on a 9060 XT by this project). "-" = no reference hash.
BLOBS="
navi48 gc_12_0_1_imu     b3b301fb636efc77b63ce4d2ced0f90c851d03c19681852faa45598e6f5773fd
navi48 gc_12_0_1_me      5549655093724b3e3b5fa016e8f1ef6ce3976209432230852cd94acbc1c245cb
navi48 gc_12_0_1_mec     72b25088072d630d115748ee3607d0db481b22a15260af3d0d599e516a0d7abb
navi48 gc_12_0_1_pfp     6dc9e6d22e0f9ddd04f12eedb5d1f6affae6d76d2993567aafde0e1619071c15
navi48 gc_12_0_1_rlc     6ba4459532246a5c415d3cb33c9b1248294e48f67b827e2accb292a8d1a5c0ec
navi48 gc_12_0_1_uni_mes bf26e0300989ebd011e991d76396e2c1673e1caa502fb6d8413e387eb1456676
navi48 psp_14_0_3_sos    23bea01a0c6f36d00759d0765d46cb4cb4aa87398b2fbccacbf547a890c0bf51
navi48 psp_14_0_3_ta     38d6691bf64232946cd5ce5735363c7c2877830ff640b9b95516403257fe2922
navi48 sdma_7_0_1        73c29e1c1714ebc95d2221ba56e187910902891593010653bf9518937e414a59
navi48 smu_14_0_3        3221ef2ddb341570eeb727e1e16f170bfb2ea1230be4a7eb248d0b183fbfbf15
navi44 gc_12_0_0_imu     aa15e5b3156bffc45e0c50bccbcd364fbd3f958531b695b7487a803d780b8328
navi44 gc_12_0_0_me      c288e326b9a5ccccac377455db3c25ace047a42a8e9e5a2239b5554dd2a9a9e7
navi44 gc_12_0_0_mec     9b0788eb46efa7d53a1d87cfe3031cd403ec1eef04314af2a55257883a4560d1
navi44 gc_12_0_0_pfp     cb6eb3ce40c79b0ada234b70ed038983627af78287afd7e5e26123dbcf7e79f8
navi44 gc_12_0_0_rlc     6436b582734a413456fff3d3c7195e71cc9e78a7ed31ee21c83ffd6fae1ad186
navi44 gc_12_0_0_uni_mes 309d60ad4a0210f8a68e27cae88beddb22b5ae900d70848b1e8ca0eac9d1decd
navi44 psp_14_0_2_sos    7b538448b57d4f9dd06b2eea90d4f86a16e65e3027cdecee8db71c2c5f1fa243
navi44 psp_14_0_2_ta     21b8898e56524240c19a1f834fcea3dde7d98c33044658a1641f0cb3c3b1bddd
navi44 sdma_7_0_0        beaafb53993a106edd392392d5896245ae2a957c6d0f495d0002eec72ad8ad38
navi44 smu_14_0_2        1b2054e3f710d1ab8dbdf6ff35914ad376b51caa6337831260c955add874b2ee
shared dcn_4_0_1_dmcub   74b567655ac89bfe03389b660e973dc3c067aebd464c6389a071662cfbe6237f
"

while read -r chip name want; do
    [ -n "$name" ] || continue
    if [ "$chip" != shared ]; then
        case " $CHIPS " in *" $chip "*) ;; *) continue ;; esac
    fi
    if [ -f "$SRC/$name.bin" ]; then
        cp "$SRC/$name.bin" "$DST/$name.bin"
    elif [ -f "$SRC/$name.bin.zst" ] && command -v zstd >/dev/null 2>&1; then
        zstd -dqf -o "$DST/$name.bin" "$SRC/$name.bin.zst"
    else
        echo "MISSING  $name.bin (not in $SRC)" >&2
        exit 1
    fi
    got="$(shasum -a 256 "$DST/$name.bin" | awk '{print $1}')"
    if [ "$want" = "-" ]; then echo "copied   $name.bin ($(wc -c < "$DST/$name.bin" | tr -d ' ') bytes, sha256 $got; no reference hash yet)"
    elif [ "$got" = "$want" ]; then echo "ok       $name.bin ($(wc -c < "$DST/$name.bin" | tr -d ' ') bytes)"
    else echo "DIFFERS  $name.bin (sha256 $got; developed against $want) - copied anyway"; fi
done <<< "$BLOBS"
cp "$1/LICENSE.amdgpu" "$DST/" 2>/dev/null || true
echo "copied into $DST"
