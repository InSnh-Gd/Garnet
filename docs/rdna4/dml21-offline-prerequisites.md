# DCN 4.01 DML2.1 offline prerequisites

Wave B2 of `advance-rdna4-display-mall-and-power` is not complete. No DML2.1 solver, kernel FPU entry, or Linux reference vectors were substituted for the missing pinned source.

## Verify source before importing

The handoff pins Linux `9437ee8b6cf607e883b36e44bd755968daaebd48`. The prior research cached the upstream directory listing for `drivers/gpu/drm/amd/display/dc/dml2_0/dml21`. Its Git tree IDs distinguish the pinned source from the two locally available copies:

| Source | `inc` tree | `src` tree |
| --- | --- | --- |
| Pinned commit (cached upstream listing) | `76503077907f83419bb32239a47a2cd2d31205dd` | `b1b2159d4d8d4e8d9b3edcb85ef199ae88e5162b` |
| Stela Linux 7.2.12 (`cc9de5d4b5ab6459f5774946aa3195ad51236fa8`) | `fa4d8c6ee4f00147c0e033ef72f5c657b4ee8fe1` | `8545a44bddeea4bdca8887f24272625c9f02df1f` |
| Nix `linux-src-patched` 7.3.6 (calculated from the unpacked tree) | `ac38085dadf6e970b4b09310cfd57fefbdb79aca` | `c112defd83788b7e575cfd587e90e9a5a3ccbae8` |

Use a complete local checkout or bundle at the pin, with the wrapper and its DCN 4.01 dependencies as well as the solver. The pin's wrapper is under `dc/dml2_wrapper/dml21_wrapper`, not in the solver directory. Verify the object and trees without triggering a partial clone fetch:

```bash
export GIT_NO_LAZY_FETCH=1 GIT_ALLOW_PROTOCOL=file
pin=9437ee8b6cf607e883b36e44bd755968daaebd48
solver=drivers/gpu/drm/amd/display/dc/dml2_0/dml21
git -C "$LINUX_TREE" cat-file -t "$pin" &&
git -C "$LINUX_TREE" ls-tree "$pin" "$solver/inc" "$solver/src"
```

Expect `commit` and the two pinned tree IDs. For an archive without Git objects, authenticate its bytes against the pinned recursive tree or a manifest derived from a verified checkout; a version string is not enough. The local copies differ in the DCN 4.01 core and shared headers, not only in post-DCN 4.01 source files. Neither is a verified replacement. Preserve MIT notices and record port changes separately from the source import.

## Build and validate the bridge

- Independently run the pinned, unmodified Linux solver in a host oracle. Record complete inputs, revision and build command, then compare the Garnet bridge against those outputs under ASan+UBSan. Cover 4K@60, 4K@160 DSC, and mixed DP+HDMI on both Navi 44 and Navi 48. Compare mode support, urgent/stutter/UCLK/FCLK watermarks, DET and COMPBUF allocations, UCLK P-state method, per-plane MALL bytes and FAMS2 programming. Verify units before comparing: e.g. raw register cycles vs. time, and bytes/KiB vs. hardware segments.
- Record each mode's timing, link rate and lane count, color depth, DSC settings, scanout layout, cursor, and pipe mapping. `logs/rdna4-hardware-test/20261110-high-refresh/os-edid.txt` provides a 4K60 timing (533.250 MHz; totals 4000 × 2222). `logs/rdna4-perf-baseline/stage3-fix-post-20261125T112015Z/REPORT.md` provides observed Navi 44 clock/power points, not a DCN clock table. The local Linux `dc/soc_and_ip_translator/dcn401/dcn401_soc_and_ip_translator.c` explicitly calls the default clock table a placeholder. No complete per-chip SMU clock/BIOS latency inputs were found in the checked evidence. Do not turn the observed points into a production bounding box or conflate physical MALL (32/64 MiB) with the usable display budget.
- Allocate the solver instance, initialization inputs, display config/scratch and programming outputs on the heap, outside the FPU critical section. Expose integer inputs/outputs at the kernel boundary; do the conversions inside the isolated `-msse -msse2 -mno-avx` objects with a `3072`-byte per-frame limit. Keep clock access and MMIO out of the solver. The absence of FP/SIMD in non-DML object disassembly does not prove the solver's stack or FPU context safety.
- Before enabling any entry, establish the supported XNU preemption/FPU ownership hooks for the target x86_64 kernel. The locally available SDK headers describe the interrupt-context/interrupt-state routines and `CR0.TS`/`CR0.EM`; the sparse XNU checkout lacked the implementations needed to verify FPU ownership. Reject IRQ or unsupported FPU state before the first `fxsave64`; do not clear `CR0.TS` speculatively. Verify save/restore of x87/SSE and MXCSR around a non-sleeping call, outside `gScanoutLock` and `rdna4_display_txn`, before passing results to B3.
