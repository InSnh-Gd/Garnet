# RDNA 4 Display MALL, FAMS2, DML2.1, GFXOFF, and Multi-Display Power Research

- **Date**: 2026-11-27
- **Targets**: Navi 44 (RX 9060 XT, `gfx1200`, `1002:7590`, 32 MiB MALL) and Navi 48 (RX 9070 XT, `gfx1201`, 64 MiB MALL)
- **Primary Sources**:
  - Local Garnet repository (`src/garnet/`, `src/dcn41/`, `logs/rdna4-perf-baseline/`)
  - Upstream Linux `torvalds/linux` (`ref=9437ee8b6cf607e883b36e44bd755968daaebd48`): `drivers/gpu/drm/amd/display/` and `drivers/gpu/drm/amd/amdgpu/`

---

## 1. Baseline & Problem Statement

Stage 3 (`optimize-garnet-display-vbl-and-power`) eliminated major idle power waste on the RX 9060 XT (Navi 44) by enabling GFX12 clock gating via RLC safe mode, hardware `VUPDATE_NO_LOCK` VBLANK IRQs, DPMS for primary DisplayPort, single-pipe `DET0` expansion (`192 KiB -> 576 KiB`), and self-protected `UCLK` unpinning (`logs/rdna4-perf-baseline/stage3-fix-post-20261125T112015Z/REPORT.md`):

| Mode | Idle Socket W | GFX / UCLK / FCLK MHz | `DET0` Buffer Time | VBLANK Duration | Result |
|---|---:|---:|---:|---:|---|
| 4K@60Hz (533.25 MHz, non-DSC) | **16.6 W** | 1000 / **96** / 1096 | **288 µs** | **465 µs** | `UCLK` unpinned safely (`0` OPTC0 underflows) |
| 4K@160Hz (1479 MHz, DSC) | **27.6 W** | 1000 / **1258** / 2016 | **103 µs** | **108 µs** | Forced `UCLK` top-DPM pin (**+11.0 W** gap) |

Because GDDR6 `UCLK` P-state transitions introduce a `~150–250 µs` memory blackout window (`src/garnet/src/amd/garnet_power_policy_pure.h:183-219`), modes whose VBLANK and `DET0` drain time fall below `350 µs` / `250 µs` (4K@160Hz DSC, multi-display configs) cannot lower `UCLK` without DMUB **FAMS2** / **Display MALL (SubVP / Static Screen CAB)** and **DML2.1** watermarks.

Meanwhile, Wave 9 compute benchmarks (`logs/rdna4-perf-baseline/perf-160fps-wave9-summary.md`) established Navi 44's 32 MiB Infinity Cache (MALL) bandwidth at `819–928 GB/s` (vs `284–312 GB/s` GDDR6). Any Display MALL policy must therefore preserve Wave 9/11 compute & render cache behavior when Display MALL is inactive or strictly scoped per upstream `amdgpu_dm`.

---

## 2. Area 1 — GFXOFF (`garnet-gfxoff`) Readiness Audit

### 2.1 Existing Infrastructure & Synchronous Bounce Bottleneck
- State machine (`src/garnet/src/amd/garnet_gfxoff_pure.h:4-143`): refcounted `GuardState` with `garnet-gfxoff` absent (default OFF), `=0` kill-switch, `=1` opt-in hardware `AllowGfxOff`, sticky `faulted` fail-safe latch, and underflow refusal.
- Ten existing `ScopedGfxOffGuard` call sites across four files:
  - `src/garnet/src/GarnetUserClient.cpp:270` (`doRegRead`)
  - `src/garnet/src/amd/cp_v12_0.cpp:250` (`cp_dump_state`), `:1401` (`cp_dump_gfx_hqd`), `:2956` (`cp_dump_compute_hqd`)
  - `src/garnet/src/amd/garnet_ring_submit.cpp:689` (`g2_recover`), `:1436` (`kMethMesReset`), `:1448` (`kMethRemap`)
  - `src/garnet/src/amd/gmc_v12_0.cpp:1709` (`gmc_flush_gpu_tlb` GC hub), `:1968` (`gmc_log_vm_faults`), `:1983` (`gmc_clear_vm_faults`)
- **Missing delayed-idle hysteresis**: `src/garnet/src/amd/smu_v14_0.cpp:979-1038` (`gfx_off_ctrl`) dispatches `PPSMC::AllowGfxOff` synchronously on every `1 -> 0` refcount transition (each SMU mailbox transaction polls with `IOSleep(1)` at `smu_v14_0.cpp:197-205`). On hot-path VM flushes, this causes synchronous `DisallowGfxOff` -> `AllowGfxOff` mailbox thrashing unless a delayed-idle timer (`~100 ms`, matching Linux `AMD_GFXOFF_DELAY_MS`) coalesces `AllowGfxOff` re-arming.

### 2.2 Remaining Unguarded Post-Bringup `IPBlock::GC` MMIO Paths
`amdgpu_init.cpp:908-916` arms `gfx_off_boot_init()` at the end of `bringup_to()`, and `bringup_teardown()` disables GFXOFF at `:802-806`. Six runtime/post-bringup GC MMIO paths currently lack `ScopedGfxOffGuard` or GC-MMIO elimination:

1. **Boot-time S1b self-test** (`src/garnet/src/GarnetDriverStages.cpp:650,693` -> `src/garnet/src/amd/native_s1b.cpp:93,99-102,353-354,405-420`): runs *after* `bringup_to()`, touching GFXHUB `vm_l2_protection_fault_cntl`, `CONTEXT8` `CNTL/BASE/START/END`, and `GC[0]+0x1e1f` (`cpDebug`) unguarded.
2. **SDMA7 doorbell kick** (`src/garnet/src/amd/sdma_v7_0.cpp:1459-1473`, `sdma_kick_doorbell`): writes `SDMA_QUEUE0_RB_WPTR`/`WPTR_HI` and reads `RB_WPTR`/`RB_RPTR` via `IPBlock::GC` MMIO on every scanout/fastcopy blit. **Lock-order hazard**: `:1410` notes callers may hold `gScanoutLock`; acquiring `SmuSeq` inside `gScanoutLock` would risk lock-order inversion. Switching SDMA7 doorbell kick to sysmem `wptr` + NBIO HDP flush + BAR2 `WDOORBELL64` (matching `cp_kick_doorbell` at `cp_v12_0.cpp:2230-2254`) wakes GFXOFF in hardware with zero GC MMIO and zero SMU lock dependency.
3. **Native VM / Session paths** (`src/garnet/src/amd/garnet_session_vm.cpp`):
   - `program_root()` (`:58-69`): writes 6 GFXHUB `CONTEXT<vmid>` registers unguarded before calling `flush_vmid` (`:68`).
   - `ctx_prepare_vmid()` (`:76-89`): reads and RMW-writes GFXHUB `CONTEXT<vmid>_CNTL` unguarded.
   - `fault_snap()` (`:138-143`): reads 4 GFXHUB fault registers via `hub_rd(0x15d0..0x15d3)` (`garnet_submit_internal.hpp:220-223`) unguarded.
   - `n1c_query_info()` (`:520`) and `n1c_read_regs()` (`:553`): read `GB_ADDR_CONFIG` (`0x263e..0x2641`) via `IPBlock::GC` unguarded (cacheable or guarded).
4. **Native hang diagnostics** (`src/garnet/src/amd/garnet_ring_submit.cpp`): `hang_log()` (`:243-248`) and `g1_cp_stat()` (`:1324`) called unguarded at `:1412`, `:1513`, `:1516`, and `:1561` (`kActHangTest`).
5. **SDMA DCC control / fastcopy**: `src/garnet/src/GarnetDriver.cpp:759-794` (`rdna4_sdmadcc_control`, action 82) and `src/garnet/src/dcn/garnet_sdma_fastcopy.cpp:256,841` (`fc_dcc_raw`) read/write `SDMA*_DCC_CNTL` (`GC[0]`) unguarded.
6. **Runtime compute self-test** (`src/garnet/src/GarnetUserClient.cpp:302` -> `src/garnet/src/amd/compute_test.cpp:398,406,460-474`): reads `GRBM_STATUS`, `CP_STAT`, and `COMPUTE_*` SH registers unguarded.

### 2.3 Verified Zero-MMIO Clean Paths
- `smu_idle_dpm` `hwBusy` probe (`amdgpu_init.cpp:19-21` -> `n1c_rings_busy()` at `garnet_ring_submit.cpp:827-834`) reads only sysmem writeback slots.
- `cp_kick_doorbell()` (`cp_v12_0.cpp:2230-2254`), `cp_compute_submit_ib()` (`cp_v12_0.cpp:2863-2929`), and native GFX/KCQ ring kicks use sysmem `wptr` + NBIO HDP flush + BAR2 `WDOORBELL64`.
- `rlc_v12_0.cpp` and `gfx_v12_0.cpp` execute only during `bringup_to()` (before `gfx_off_boot_init`) and `bringup_teardown()` (after `gfx_off_teardown()`).

---

## 3. Area 2 — HDMI & Multi-Display Hardware DPMS Audit

### 3.1 Primary Framebuffer HDMI Blockers
- `src/garnet/src/GarnetPrimaryFramebuffer.cpp:775,882-885` (`applyHardwareModeLocked`, `applyDpmsLocked`), `src/garnet/src/dcn/dcn41_dpms_pure.h:42-45,105` (`dpmsOff`, `dpmsWakePrepare`), and `src/garnet/src/dcn/dcn41_modeset.h:989` all hard-reject `!out.isDp || !out.auxValid`.
- `dcn41_dpms_pure.h:51-91` and `dcn41_modeset.h` lack HDMI TMDS (`DIG_BE`/`HDMI_CONTROL`/`TMDS_CTL`/stream blanking) and HDMI 2.1 FRL (`HPO_HDMI_*`/`FRL` link) blank/re-enable sequences; only `disp2.h:189,236,270-281` carries DIG2/DIG3 TMDS offsets for secondary bringup.
- `GarnetPrimaryFramebuffer.cpp:551-589` (`modeset_phy_control`) hardcodes `digMode=0` (DP) and `connObjId=0x13` (DP) on disable, whereas HDMI TMDS requires `connObjId=0x0C` (HDMI-A), `mode=2` (TMDS-DVI), `lanenum=4`, pixel-clock `symclk`, plus `SET_PIXEL_CLOCK` and `DIGX_ENCODER_CONTROL` on wake (only hardcoded fixed-slot examples exist in `src/garnet/src/dcn/dmubcmd.h:206-221`, none parameterized or FRL-aware).

### 3.2 Secondary Display DPMS Blockers
- `src/garnet-accel/src/garnet_accel_display_impl.inc:188-195` (`GarnetFramebuffer::setAttribute` for `kIOPowerAttribute`) invokes `dops->power(provider, value)`, but `setAttributeForConnection` (`:208-213`) for `kConnectionPower` only saves `connPower = value` without calling `dops->power`.
- `src/garnet/src/GarnetDisplayNub.cpp:78-82` (`op_power`) is a logging-only stub returning `0`.
- `garnet_accel_display_impl.inc:85-100` (`GarnetFramebuffer::initForPM`) sets `kIOPMPreventSystemSleep` across all 3 power states because secondary pipes lack S3 resume restoration.
- `src/garnet/src/dcn/dmubcmd.h:224-250` refuses manual `dmubsend` templates for monitor A (`3..7`) and monitor B (`8..15`) while a secondary plane is `HELD`, requiring kernel-internal DPMS sequencing coordinated with `scanX` / `HELD` / VBL state.

---

## 4. Area 3 — DCN 4.01 CRB/DET, Watermarks & Multi-Display Audit

- **Hardware CRB size (`torvalds/linux` primary source)**: `drivers/gpu/drm/amd/display/dc/hubbub/dcn401/dcn401_hubbub.h` defines `DCN4_01_CRB_SIZE_KB = 1344` (`21 x 64 KiB` segments) and `DCN4_01_DEFAULT_DET_SIZE = 320` (`5` segments), initialized into `hubbub401_construct()` in `dcn401_resource.c:933-939`. Garnet's `src/garnet/src/dcn/dcn41_det_pure.h:32-36` conservatively caps at UEFI GOP's `kUefiCrbBudgetSegs = 13u` (`1 compbuf + 4*3 DET`) and `kTargetDet0Segs = 9u` (`576 KiB`).
- **Mathematical bound at 4K@160Hz DSC**: Even expanding single-pipe `DET0` to 16–20 segments (`1024–1280 KiB`) yields `183–229 µs` DET drain time while VBLANK remains `108 µs`, proving CRB expansion alone improves stutter margin and multi-pipe balancing (`dcn401_hubbub.c:1120-1196`) but cannot replace FAMS2/SubVP for 4K@160Hz `UCLK` P-state switching.
- **Missing watermark programming**: `DCHUBBUB_ARB_*` urgent/stutter/P-state A & B watermarks (`dcn401_hubbub.c:68-350`) and `HUBPREQ_DCN_TTU_*` (`dcn41_modeset.h:32-34` constants declared, never written) are currently unprogrammed.
- **Multi-display `disp2` compatibility hazard**: `disp2.h:25,1033,1166-1167`, `disp2_flow.h:527,671`, and `dcn_disp2_scanx.cpp:233` hard-assert `DET0_CTRL == 0x00000303u` and `DET1/2 == 3` (`RDNA4D2_DP_DISTURBED` / `RDNA4D2_GATE`). Dynamic CRB/DET partitioning must preserve legacy `disp2` invariants whenever `garnet-disp2` is active or until `disp2` checks are updated safely.

---

## 5. Area 4 — DAL DMUB (`dcn_4_0_1_dmcub.bin`) & PSP Loading Audit

- **Current gap**: `tools/fetch-firmware.sh:33-54`, `src/garnet/src/fw/fw_table.h:25-40`, `src/garnet/src/amd/fw_table.cpp:18-28`, `amdgpu_asic.cpp:21-36`, `amdgpu_ucode_psp.h`, `amdgpu_psp.h:309-375`, `amdgpu_ucode_extract.{h,cpp}`, `fw_loader.cpp`, and `psp_v14_0.cpp:1603-1700` contain zero `DMCUB` firmware definitions or PSP load wiring.
- **Upstream Linux primary source**:
  - `drivers/gpu/drm/amd/display/amdgpu_dm/amdgpu_dm_dmub.h`: `FIRMWARE_DCN_401_DMUB` = `"amdgpu/dcn_4_0_1_dmcub.bin"` (shared by Navi 44 `GC 12.0.0` and Navi 48 `GC 12.0.1`).
  - `drivers/gpu/drm/amd/amdgpu/psp_gfx_if.h` & `amdgpu_psp.c`: `AMDGPU_UCODE_ID_DMCUB` maps to `GFX_FW_TYPE_DMUB = 51` via `GFX_CMD_ID_LOAD_IP_FW`.
  - `drivers/gpu/drm/amd/display/dmub/src/dmub_dcn401.c:64-220`: clean VBIOS -> DAL handoff stops the running VBIOS FW via `DMUB_GPINT__STOP_FW`, waits for `DMCUB_SCRATCH7 == 0xDEADDEAD` and `DMCUB_PWAIT_MODE_STATUS` bit 0, asserts `DMCUB_SOFT_RESET = 1` + `DMCUB_ENABLE = 0`, clears `CW2..CW7`, resets `INBOX1`/`OUTBOX1`/`OUTBOX0` pointers and `SCRATCH0`, programs `CW0` (code)/`CW1` (stack/data) or PSP-authenticated load, sets up `CW3..CW6` + `REGION6`, sets `DMCUB_SCRATCH15`, and releases `DMCUB_ENABLE = 1` + `DMCUB_SOFT_RESET = 0`.
  - `QUERY_FEATURE_CAPS` (`dmub_cmd.h` / `dcn41_dmub.h:110-116`) reports `fw_assisted_mclk_switch_ver` (`2.1`) and `subvp_psr_support`.
- **Allowlist readiness**: `src/dcn41/dcn41_allow_ranges.h:64,67` **already permits** all `DMCUB_*` registers (`abs 0x3656..0x36c0` inside `0x35e6..0x3771`), `DCHUBBUB_*` (`abs 0x392f..0x39ef`), and `HUBP0..3_DCHUBP_MALL_CONFIG` (`abs 0x3ab7..0x3d4b`) inside `0x392f..0x3de1`. Only missing offset definitions (e.g. `DCHUBBUB_ARB_*`, `DCHUBBUB_ARB_MALL_CNTL`) need adding to `src/dcn41/dcn41_regs.h`.

---

## 6. Area 5 — Full MIT DML2.1, FAMS2 v2.1, Display MALL & DCC-on-Scanout

### 6.1 Full MIT DML2.1 Port & macOS x86_64 Kernel FPU Isolation
- **Upstream MIT C source subset (`torvalds/linux/drivers/gpu/drm/amd/display/dc/dml2_0/Makefile`)**:
  - `dml21/src/dml2_top/{dml2_top_interfaces,dml2_top_soc15,dml2_top_utm}.c`
  - `dml21/src/dml2_core/{dml2_core_dcn4,dml2_core_utils,dml2_core_factory,dml2_core_dcn4_calcs}.c`
  - `dml21/src/dml2_dpmm/{dml2_dpmm_dcn4,dml2_dpmm_factory}.c`
  - `dml21/src/dml2_mcg/{dml2_mcg_dcn4,dml2_mcg_factory}.c`
  - `dml21/src/dml2_pmo/{dml2_pmo_dcn3,dml2_pmo_factory,dml2_pmo_dcn4_fams2}.c`
  - `dml21/src/dml2_standalone_libraries/{lib_float_math,lib_frl_cap_check}.c`
  (DCN5/DCN6 files are excluded.)
- **macOS x86_64 kext constraints (`src/garnet/Makefile:101-108`)**:
  - Normal kext objects compile with `-mkernel` (no FPU/SSE instructions). DML2.1 `.c` files must be isolated in their own build rule with `-msse -msse2 -mno-avx -Wframe-larger-than=3072` (never touching non-DML translation units).
  - Every DML2.1 call site must be wrapped in serialized `DC_FP_START()` / `DC_FP_END()`: acquire dedicated `IOLock`, disable preemption/interrupts around `fxsave64` (64-byte-aligned buffer) + `fninit` + `ldmxcsr(0x1F80)` on entry and `fxrstor64` on exit, strictly forbidden from primary/VBL IRQ context.
  - Because `dml2_core_dcn4_calcs.c` carries large structs and macOS kernel thread stacks are 16 KiB, `dml2_context` / `display_configuation_with_meta` / `dml2_display_cfg_programming` MUST be heap-allocated (`IOMallocZero` / `IOFree`), never stack-allocated.

### 6.2 FAMS2 v2.1 & SubVP
- Upstream `dcn401_resource.c:781,796-806`, `dc_dmub_srv.c`, `dcn401_optc.c`, and `dmub_cmd.h` implement FAMS2 v2.1 (`major=2, minor=1`) using:
  - `DMUB_CMD__FW_ASSISTED_MCLK_SWITCH` sub-types `DMUB_CMD__FAMS2_CONFIG = 4`, `DMUB_CMD__FAMS2_DRR_UPDATE = 5`, `DMUB_CMD__FAMS2_FLIP = 6`, `DMUB_CMD__FAMS2_IB_CONFIG = 7`, backed by an indirect buffer of size `sizeof(struct dmub_fams2_config_v2)`.
  - SubVP phantom-pipe programming (`SUBVP_PHANTOM` in `dcn401_hwseq.c:847-850`, `dcn32_subvp_in_use` in `dcn401_resource.c:1778`) and `DCHUBBUB_ARB_MALL_CNTL` / `HUBP*_DCHUBP_MALL_CONFIG` / `DCHUBP_MALL_SUB_VP0..2`.

### 6.3 Static Screen CAB, Navi 44 vs Navi 48 Capacity, Cursor & DCC-on-Scanout
- **Static Screen CAB (`dcn401_hwseq.c:1279-1398`)**: `dcn401_apply_idle_power_optimizations()` sends `DMUB_CMD__CAB_FOR_SS` with `NO_DCN_REQ`, `DCN_SS_FIT_IN_CAB` (`cab_alloc_ways`), `DCN_SS_NOT_FIT_IN_CAB`, or `NO_IDLE_OPTIMIZATION`.
- **Navi 44 vs Navi 48 MALL sizing (`dcn401_resource.c:1450-1471,1785-1791`)**:
  - `dcn401_calc_num_avail_chans_for_mall()` clamps `GC 12.0.0` (Navi 44) to `16` channels -> **32 MiB MALL**, and `GC 12.0.1` (Navi 48) -> **64 MiB MALL** (power-of-2 channel calculation).
  - A linear 4K `3840x2160` ARGB8888 surface consumes `3840 * 2160 * 4 = 33,177,600` bytes (`31.64 MiB`), leaving only `0.36 MiB` of 32 MiB on Navi 44 — marginal once cursor bytes (`dcn32_helper_calculate_mall_bytes_for_cursor`) and way-granularity alignment are added. SubVP (caching only a sub-viewport slice) works on linear 4K on Navi 44, whereas full-frame 4K Static Screen CAB on Navi 44 requires either Navi 48 (64 MiB) or **GFX12 DCC-on-scanout** (`DCSURF_TILING_CONFIG` + `DCSURF_SURFACE_CONTROL.PRIMARY_SURFACE_DCC_EN`).
- **Current DCC-on-scanout blockers**: `scanout_pure.h:48-57,70,79` (`geom_check`, `reg_bounds`), `dcn_scanout_mt.cpp:34,214,357,414-417`, and `dcn41_hubp.c:27-95` enforce linear-only (`SW_MODE=0, DCC_EN=0`) visible-BAR0 scanout.
- **Hardware cursor + MALL (`dcn41_cursor_pure.h:222-224`, `dcn401_resource.c:1838-1850`, `dcn401_hwseq.c:1305-1312`)**: keep `DCHUBP_MALL_CONFIG.USE_MALL_FOR_CURSOR = 0` whenever MALL is inactive; set `USE_MALL_FOR_CURSOR = 1` and count cursor MALL bytes in CAB allocation when SubVP/Static Screen CAB is active, enforcing upstream cursor SubVP compatibility checks.
