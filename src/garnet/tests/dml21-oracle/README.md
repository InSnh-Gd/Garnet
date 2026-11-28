# Linux DCN 4.01 reference vectors

`expected.inc` comes from the unmodified Linux DML2.1 solver at
`9437ee8b6cf607e883b36e44bd755968daaebd48`. `reference.c` calls the Linux
initialization, mode support and mode programming entry points directly.
It does not include or link the Garnet port. It uses a small libc shim, and
the original Linux `dmub_cmd.h` supplies the packed firmware structures.

## Inputs and outputs

`fixture.h` defines six fixed cases: DP 4K60 (approximately 59.997 Hz), DP 4K160 with DSC,
and DP 4K60 + HDMI TMDS 1080p60, each with 32 and 64 MiB display MALL budgets.
The Linux DCN 4.01 bounding box, variant-A QoS parameters and IP capabilities
come from `dcn4_soc_bb.h`. The memory geometry, clock table and blackout
latencies in `fixture.h` are **synthetic solver inputs**, not SMU/VBIOS
measurements. The test budgets match the respective chips' physical MALL
capacities, but do not establish how much MALL a board can dedicate to display.

The harness maps a single full-screen, unscaled linear ARGB8888 plane per
stream in stream order. It uses RGB444 output, no audio bandwidth, DCC, VM,
VRR, borders or DSC padding, and one 256-pixel, 32-bpp cursor per stream.
Immediate flips, SubVP and FAMS2 are enabled; dynamic ODM and DRR are disabled.
The solver assigns logical DPP/ODM counts; there are no physical pipe or OTG
IDs. This is not a run of the full Linux kernel wrapper: the harness maps
configurations at the solver API and supplies explicit link settings.

The harness requires both support and programming to succeed. Each row
records support, COMPBUF and per-pipe DET in 64-KiB segments, DCHUB watermark
sets in 50-MHz reference cycles, DPP and ODM counts, the Linux P-state method,
and full-surface/SubVP MALL sizes in bytes. It also records the raw packed
FAMS2 v2.1 global, base and sub-state bytes, before physical pipe mapping or
firmware submission. Both 4K60 cases use VACTIVE (method 1); the DSC and
mixed-link cases use firmware SubVP (method 6). Zero phantom DET and the
zero global FAMS2 config in the VACTIVE cases are direct solver outputs.

For SubVP, the oracle also records the phantom stream's active height, total
height and front porch, and each phantom DPP's luma QoS and request-delivery
TTU fields. The Garnet test replays those fields through its existing TTU
register helper against a simulated register bank. It does not program a
complete phantom pipe or submit FAMS2 to firmware.

## Reproduce offline

Build on little-endian x86_64 Linux with Clang (vectors captured with 21.1.8).
Run from the Garnet checkout root with `LINUX_TREE` set to the local Linux
checkout and `TMPDIR` to a writable scratch directory. On NixOS, run the
build in `nix-shell -p clang gnumake python3 llvm`, with those paths already
exported. The recipe intentionally excludes `dml2_top_legacy.c`, an orphan
translation unit that includes a header not present at the pin.

```bash
set -euo pipefail
export GIT_NO_LAZY_FETCH=1 GIT_ALLOW_PROTOCOL=file
pin=9437ee8b6cf607e883b36e44bd755968daaebd48
amd=drivers/gpu/drm/amd
solver="$amd/display/dc/dml2_0/dml21"
test "$(git -C "$LINUX_TREE" rev-parse HEAD)" = "$pin"
test "$(git -C "$LINUX_TREE" rev-parse "HEAD:$solver/inc")" = 76503077907f83419bb32239a47a2cd2d31205dd
test "$(git -C "$LINUX_TREE" rev-parse "HEAD:$solver/src")" = b1b2159d4d8d4e8d9b3edcb85ef199ae88e5162b
oracle=src/garnet/tests/dml21-oracle
build=$(mktemp -d "$TMPDIR/dml21-reference.XXXXXX")
git -C "$LINUX_TREE" archive "$pin" "$solver" "$amd/display/dmub/inc/dmub_cmd.h" \
    "$amd/include/atomfirmware.h" "$amd/include/atomfirmwareid.h" | tar -x -C "$build"
amd="$build/$amd"
solver="$build/$solver"
includes=(-I"$oracle/compat" -I"$solver/inc" -I"$solver/inc/bounding_boxes" -I"$amd/display/dmub/inc" -I"$amd/include")
for dir in "$solver"/src/*/; do includes+=(-I"$dir"); done
sources=()
for source in "$solver"/src/*/*.c; do
    [[ "${source##*/}" == dml2_top_legacy.c ]] || sources+=("$source")
done

common=(-std=gnu11 -g -msse2 -mno-avx -ffp-contract=off "${includes[@]}" "$oracle/reference.c" "${sources[@]}" -lm)
clang -O1 "${common[@]}" -o "$build/reference"
"$build/reference" > "$build/expected.inc"
cmp "$oracle/expected.inc" "$build/expected.inc"
```

For sanitizer diagnostics, run in the same shell:

```bash
clang -O1 -fno-omit-frame-pointer -fsanitize=address,undefined "${common[@]}" -o "$build/reference-audit"
UBSAN_OPTIONS=print_stacktrace=1:external_symbolizer_path="$(command -v llvm-symbolizer)" \
    "$build/reference-audit" > "$build/expected-audit.inc"
cmp "$build/expected.inc" "$build/expected-audit.inc"
```

Stock Linux produces identical vectors at `-O0`, `-O1`, `-O1` with ASan and
`-O1` with ASan+UBSan in recovery mode. **The unmodified reference is not
UBSan-clean.** With `-fno-sanitize-recover=all`, it stops on the first
float-cast-overflow diagnostic. Recovery mode reports these three cast sites:

| Diagnostic site | Relevant caller stack at the pin |
| --- | --- |
| `lib_float_math.c:66` (`math_ceil2`, NaN to `int`) | `CalculateSwathWidth:922` → `CalculateSwathAndDETConfiguration:3788` → `dml_core_mode_support:8453` |
| `lib_float_math.c:51` (`math_floor2`, infinity to `int`) | `get_num_lb_source_lines:6028` → `dml_core_ms_prefetch_check:7484` → `dml_core_mode_support:9591` |
| `dml2_core_dcn4_calcs.c:6027` (`get_num_lb_source_lines`, negative result to `unsigned int`) | `dml_core_ms_prefetch_check:7484` → `dml_core_mode_support:9591` |

All first reports descend from `dml2_core_calcs_mode_support_ex:9741`,
`core_dcn4_mode_support:561` and `dml2_top_soc15_check_mode_supported:794`.
UBSan reports each cast location once; recovery output is not proof that the
stock code is safe. Compare the Garnet implementation separately under strict
ASan+UBSan. These vectors do not verify a board clock table, actual link
training, kernel FPU context, scanout or firmware behavior.
