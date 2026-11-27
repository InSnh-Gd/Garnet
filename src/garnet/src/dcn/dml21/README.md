# DCN 4.01 DML2.1 solver

The fifteen C translation units listed in `sources.mk` and their MIT-licensed headers come from `drivers/gpu/drm/amd/display/dc/dml2_0/dml21` in Linux commit `9437ee8b6cf607e883b36e44bd755968daaebd48`. The original `inc` and `src` tree IDs are `76503077907f83419bb32239a47a2cd2d31205dd` and `b1b2159d4d8d4e8d9b3edcb85ef199ae88e5162b`. The source paths are relative to that directory; the headers retain their MIT notices. `port/dmub_cmd.h` selects the required FAMS2 declarations from the authoritative MIT header `drivers/gpu/drm/amd/display/dmub/inc/dmub_cmd.h` at the same commit and retains its one-byte FAMS2 packing.

## Changes from Linux

- `dml2_top_interfaces.c`, `dml2_core_factory.c`, `dml2_dpmm_factory.c`, `dml2_mcg_factory.c`, `dml2_pmo_factory.c`: remove DCN 4.2, DCN5 and DCN6 project dispatch and UTM dependencies. Unsupported projects return false. The shared structures retain upstream layouts and enum values.
- `dml2_core_dcn4_calcs.c`: skip work for absent chroma and DSC slices. If the line buffer has no samples to hold on an absent component, its source-line count is limited only by the hardware line limit. These branches avoid converting nonfinite FP intermediates to integers without changing arithmetic for present samples or slices.
- `dml2_mcg_dcn4.c`: reject empty clock tables before they become mode-state indices, and keep bandwidth-table compaction reads inside the populated entries.
- `port/os_types.h` provides the compiler and C library shims without importing Linux operating-system headers or the display driver wrapper. Host builds check assertions; the kernel configuration only compiles objects and has no runtime logging or assertion policy.

`make -C src/garnet check-dml21-kernel` compiles these sources and the FP adapter with SSE2, no AVX and a 3072-byte per-function frame limit on x86_64. The objects are not linked into `Garnet.kext`: a supported kernel xstate ownership mechanism is required before any runtime use. The host test consumes the same `sources.mk` and compares against separately compiled pinned Linux reference outputs.
