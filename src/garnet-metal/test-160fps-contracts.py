#!/usr/bin/env python3
"""Source-contract verification for the 4K@160Hz full-pipeline optimization (Phase A1-A4 + Phase B).

Validates wiring invariants across C++, Objective-C, and pure-C headers offline on Linux:
  A1: Default SMUPowerState::Auto (idleClamp=false unless garnet-idle-clamp=1)
  A2: Adaptive + condvar-wakeable Present Hold (holdCond, holdAdapt, 0 ms single-pass decay, post-hoc re-arm)
  A3: Submit-time superseded dispCopyVk elision + bounded 4-deep pipelining + strict FIFO completion
  A4: Dual-BAR auto policy (IORegistry port 0, fail-closed Small-BAR) + linear VkImage reuse cache
  B:  WindowServer OPTIMAL hi-pool VRAM surface cache (GFX12 DCC VkImageFormatListCreateInfo paired with
      MUTABLE_FORMAT_BIT, capture-stack exclusion, write-observed Class 2 admission, 50 ms idle flush,
      lock order, &unresolved tracking, and preservation of all pinned safety contracts).
"""
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
GM = ROOT / "src" / "garnet-metal"
GK = ROOT / "src" / "garnet" / "src"

checks = 0
fails = 0


def check(name: str, cond: bool, detail: str = "") -> None:
    global checks, fails
    checks += 1
    if not cond:
        fails += 1
        print(f"FAIL: {name}" + (f" ({detail})" if detail else ""))


def read(p: pathlib.Path) -> str:
    return p.read_text(encoding="utf-8")


def main() -> int:
    # --- Phase A1: Default SMUPowerState::Auto ---
    pwr_h = read(GK / "amd" / "garnet_power_policy_pure.h")
    stages = read(GK / "GarnetDriverStages.cpp")
    init_h = read(GK / "amd" / "amdgpu_init.h")
    init_cpp = read(GK / "amd" / "amdgpu_init.cpp")
    check("A1: idle_clamp_enabled helper defaults OFF unless garnet-idle-clamp=1",
          "idle_clamp_enabled(bool present, uint32_t value)" in pwr_h
          and "return present && value != 0u;" in pwr_h)
    check("A1: GarnetDriverStages uses rdna4pwr::idle_clamp_enabled",
          "rdna4pwr::idle_clamp_enabled(" in stages)
    check("A1: amdgpu::InitOptions::idleClamp defaults to false",
          "bool idleClamp { false };" in init_h
          and "SMUPowerState::Auto" in init_cpp)

    # --- Phase A2: Adaptive + condvar-wakeable Present Hold ---
    df_h = read(GM / "garnet_dispflip.h")
    surf_m = read(GM / "garnet_surface.m")
    cmd_m = read(GM / "garnet_commands.m")
    int_h = read(GM / "garnet_metal_internal.h")
    check("A2: adaptive hold decay=8 and 3.0 ms post-hoc window in garnet_dispflip.h",
          "RDNA4DF_HOLD_ADAPT_DECAY_FRAMES 8u" in df_h
          and "RDNA4DF_HOLD_ADAPT_WINDOW_NS    (3000000ULL)" in df_h
          and "rdna4df_hold_adapt_effective(" in df_h
          and "rdna4df_hold_adapt_note_comp(" in df_h)
    check("A2: rdna4h_wait uses pthread_cond_timedwait_relative_np and never nanosleep",
          "pthread_cond_timedwait_relative_np(&RDNA4S.holdCond, &RDNA4S.mu" in surf_m
          and "rdna4df_is_superseded(ctx->sm, ctx->slot)" in surf_m)
    check("A2: rdna4DisplayCommit notes COMP commits and wakes holdCond",
          "rdna4df_hold_adapt_note_comp(&RDNA4S.holdAdapt" in cmd_m
          and "pthread_cond_broadcast(&RDNA4S.holdCond)" in cmd_m)
    check("A2: RDNA4ScanoutState carries holdCond and holdAdapt",
          "pthread_cond_t holdCond;" in int_h and "rdna4df_hold_adapt_t holdAdapt;" in int_h)

    # --- Phase A3: Superseded D-copy elision + 4-deep multi-CB pipelining ---
    qp_h = read(GM / "garnet_queue_pipe.h")
    check("A3: garnet_queue_pipe.h defines 4-deep cap, serial kill-switch, and D-copy elision",
          "GARNET_QP_DEFAULT_MAX_INFLIGHT 4u" in qp_h
          and "/private/tmp/garnet-serial-queue" in qp_h
          and "garnet_qp_should_elide_dcopy(" in qp_h)
    check("A3: RDNA4CommandSegment splits dispCopyVk and recycles it",
          "dispCopyVk;" in int_h
          and "segment->dispCopyVk" in cmd_m
          and "garnet_qp_should_elide_dcopy(" in cmd_m
          and "superseded-elided" in cmd_m)
    check("A3: rdna4AdvanceHead uses garnet_qp_can_launch + strict FIFO ordered completion",
          "garnet_qp_can_launch(&_qp" in cmd_m
          and "rdna4EnqueueOrderedCompletion:" in cmd_m
          and "if (atomic_load(&rdna4_submit_fault) != VK_SUCCESS) return NO;" in cmd_m)

    # --- Phase A4: Dual-BAR policy + linear VkImage reuse ---
    bar_h = read(GM / "garnet_bar_policy.h")
    imp_h = read(GM / "garnet_impcache.h")
    be_m = read(GM / "garnet_backend.m")
    res_m = read(GM / "garnet_resources.m")
    check("A4: dual-BAR policy caps (512 MiB Small-BAR / 1536 MiB Full-BAR)",
          "GARNET_VRAM_SURF_CAP_SMALL_BAR  (512ULL << 20)" in bar_h
          and "GARNET_VRAM_SURF_CAP_FULL_BAR   (1536ULL << 20)" in bar_h)
    check("A4: garnet_backend.m probes GarnetDriver via port 0 (no deprecated kIOMasterPortDefault call)",
          'IOServiceGetMatchingService(0, IOServiceMatching("GarnetDriver"))' in be_m
          and "IOServiceGetMatchingService(kIOMasterPortDefault" not in be_m
          and "garnet_bar_choose_policy(" in be_m)
    check("A4: linear VkImage cache drains BEFORE import memory free and retires orphans",
          "rdna4li_retire_orphans(" in imp_h
          and res_m.index("rdna4li_pop(&RDNA4IC.li") < res_m.index("rdna4ic_pop(&RDNA4IC.c")
          and "rdna4_linimg_mark_inited(_img)" in res_m)

    # --- Phase B: WindowServer 320 GB/s OPTIMAL VRAM surface cache ---
    vs_h = read(GM / "garnet_vram_surf.h")
    check("B: kill switches and 50 ms idle flush constants defined",
          "/private/tmp/garnet-no-vram-surf" in vs_h
          and "/private/tmp/garnet-sync-writeback" in vs_h
          and "GARNET_VS_IDLE_FLUSH_NS        50000000ULL" in vs_h)
    check("B: GFX12 DCC preservation — MUTABLE_FORMAT_BIT paired with VkImageFormatListCreateInfo",
          "VkImageFormatListCreateInfo flci" in res_m
          and ".pNext = fmtListCnt == 2 ? &flci : NULL" in res_m
          and ".flags = fmtListCnt == 2 ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0" in res_m)
    check("B: hi-pool allocator prefers non-HOST_VISIBLE DEVICE_LOCAL VRAM",
          "rdna4_find_vram_hipool_mem(" in res_m
          and "!(f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)" in res_m)
    check("B: Class 2 admission only on observed WS GPU writes with capture-stack exclusion",
          "rdna4_vs_observe_ws_write(t)" in cmd_m
          and "garnet_vs_symbol_is_capture(" in res_m
          and "CaptureSurfaceMetal" in vs_h
          and "_XHWCaptureDesktop" in vs_h
          and "CreateCompositeDestination" in vs_h
          and "ScreenCapture" in vs_h)
    check("B: shared VkImageLayout tracked in RDNA4VS across wrappers",
          "_vramSurfCached ? rdna4_vs_get_layout(_vramSurfId) : _layout" in res_m
          and "else if (_vramSurfCached) rdna4_vs_set_layout(_vramSurfId, l)" in res_m)
    check("B: rdna4_vs_run_flush tracks &unresolved and restores savedLayout",
          "rdna4_oneshot(&err, ^(VkCommandBuffer cmd)" in res_m
          and ", &unresolved);" in res_m
          and ".oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, .newLayout = savedLayout" in res_m)
    check("B: rdna4_vs_latch / report / tick wired in garnet_backend.m",
          "rdna4_vs_latch();" in be_m
          and "rdna4_vs_report(tick); rdna4_vs_tick();" in be_m)
    check("B: DisplaySurface in VRAM uses layout-preserving rdna4s_record_copy_img_preserve in dispCopyVk while preserving test-m6x pin",
          "const BOOL vramImgSrc = imgsrc || [dt rdna4VramSurfCached];" in cmd_m
          and "rdna4s_record_copy_img_preserve(dcvk, dcTex, &segment->dispCopyPlan, segment->dispCopySavedLayout);" in cmd_m
          and "if (take && !((imgsrc ? rdna4df_img_ok(gw_, gh_, gp_, 4) : (BOOL)YES)" in cmd_m)
    check("B: rdna4s_late_run copies VRAM-cached display textures directly from optImg (guarded for GARNET_DISPLAY_LATE_ONLY)",
          "rdna4s_record_copy_img_preserve(cmd, frame->texture, &plan, vramLateLayout);" in surf_m
          and "#ifndef GARNET_DISPLAY_LATE_ONLY" in surf_m)
    check("B: no unconditional VK_IMAGE_USAGE_STORAGE_BIT on sRGB-mutable VRAM images",
          "if (wantStorage && fmtListCnt == 2) return NO;" in res_m)
    check("B: RDNA4CommandBuffer queue-accessed ivars are @public",
          "@public\n    BOOL _launchedAsBarrier, _fifoDoneFailed;" in int_h)
    check("B: synchronizeResource:, replaceRegion:, getBytes: preserve CPU coherency",
          "rdna4_vs_invalidate_cpu([self rdna4VramSurfId])" in res_m
          and "rdna4_vs_flush_entry_sync([self rdna4VramSurfId])" in res_m
          and "rdna4_ios_writeback_ex([_cb vk], t, YES)" in cmd_m)
    check("B: no screensharingd process/FD polling introduced",
          "screensharingd" not in res_m and "screensharingd" not in cmd_m and "screensharingd" not in vs_h)
    check("B: no duplicate RDNA4VS definitions in garnet_resources.m",
          res_m.count("GARNET_INTERNAL void rdna4_vs_latch(void)") == 1)
    check("B: newTexture(iosurface:) uses kIOSurfaceLockReadOnly to avoid seed bumps",
          "IOSurfaceLock(s, kIOSurfaceLockReadOnly | kIOSurfaceLockAvoidSync, NULL)" in res_m
          and "IOSurfaceUnlock(s, kIOSurfaceLockReadOnly | kIOSurfaceLockAvoidSync, NULL)" in res_m
          and "IOSurfaceLock(s, kIOSurfaceLockAvoidSync, NULL)" not in res_m)

    # --- Wave 7: Sync commit, lazy dispCopyVk, VK_KHR_push_descriptor, CB render scratch ---
    shd_m = read(GM / "garnet_shaders.m")
    pipe_m = read(GM / "garnet_pipelines.m")
    check("W7A: synchronous queue commit enabled with /private/tmp/garnet-async-commit kill switch",
          'setValue:@YES forKey:@"commitSynchronously"' in cmd_m
          and '"/private/tmp/garnet-async-commit"' in cmd_m)
    check("W7A: dispCopyVk recorded lazily after superseded-elision check via rdna4TakeCmdBufLocked:",
          "segment->dispCopyPending" in cmd_m
          and "rdna4TakeCmdBufLocked:" in cmd_m
          and "elidedDispCopy && segment->dispCopyPending" in cmd_m)
    check("W7B: VK_KHR_push_descriptor enabled and guarded against audit/hook/non-standard bindings",
          "VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME" in be_m
          and "VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR" in shd_m
          and "rdna4_stage_dsl_ex(" in pipe_m
          and "RDNA4R.cmdPushDescSet(" in cmd_m)
    check("W7C: RDNA4RenderEncoder borrows CB-owned RDNA4RenderScratch and RDNA4ResourceBindings",
          "RDNA4RenderScratch *_sc;" in int_h
          and "rdna4_stage_reset(" in int_h
          and "rdna4AcquireRenderScratch:" in cmd_m
          and "rdna4AcquireResourceBindings:" in cmd_m)

    # --- Wave 8: Active-scanout WindowServer pipelining + texture-dirty gating ---
    dc_h = read(GM / "garnet_drawcache.h")
    check("W8A: WindowServer commitAndWaitUntilSubmitted pipelines when RDNA4DF_ACTIVE owns scanout (keeps wait on fallback/dlate/override)",
          "garnet_qp_ws_needs_completion_wait(" in qp_h
          and "garnet_qp_ws_needs_completion_wait(" in cmd_m
          and "scanoutActive = (RDNA4S.sm.state == RDNA4DF_ACTIVE);" in cmd_m)
    check("W8B: texture dirty bits gate rdna4EachTex: on buffer/bytes-only draws",
          "GARNET_DRAW_DIRTY_FTEX" in dc_h
          and "rdna4dc_stage_needs_tex(" in dc_h
          and "rdna4dc_stage_needs_tex(" in cmd_m)

    print(f"test-160fps-contracts: {checks} checks, {fails} failed")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
