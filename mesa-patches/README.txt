Mesa patches (RADV Darwin / macOS port for the Garnet native kext)
===================================================================

These patches add a macOS backend to Mesa's RADV Vulkan driver so that it
talks to Garnet.kext (GarnetDriver / GarnetAccelerator, user-client ABI "GRNT")
instead of a DRM node. Only our changes are included, not the Mesa tree.

Upstream base (apply on top of exactly this commit):
    mesa  f5cb8ee032adabef599ae892ec4e42d796b84da9   (VERSION 26.3.0-devel)
    https://gitlab.freedesktop.org/mesa/mesa.git

Licence: Mesa is MIT licensed. These patches are changes to Mesa and are
released under the same licence (see Mesa's docs/license.rst and
third-party/THIRD-PARTY-LICENSES.txt).

Apply (or use tools/rdna4/build-radv.sh):
    git clone https://gitlab.freedesktop.org/mesa/mesa.git
    cd mesa
    git checkout f5cb8ee032adabef599ae892ec4e42d796b84da9
    for file in /path/to/Garnet/mesa-patches/*.patch; do
        patch -p1 < "$file" || break
    done

Order matters: 0001 .. 0033. The kext ABI header that patches 0002-0004,
0028, 0030, 0031, and 0033 mirror is src/garnet/src/GarnetNativeABI.h in this repository;
Mesa's copy and the kext's copy must stay ABI-compatible (currently 1.14; older
ABI <= 1.13 kernels remain compatible with automatic fallback to legacy selectors 3/21/27).

0010: Navi 44 (RX 9060 XT, gfx1200) canned device entry; the chip is chosen at runtime from the kext QueryInfo gc_version (0x0c0001 gfx1201 / 0x0c0000 gfx1200, 0 = old kext -> gfx1201). Env: RADV_DARWIN_CHIP=gfx1200|gfx1201 forces the chip (also the HOST_BO stand-in choice); RADV_DARWIN_MOCK_GC=gfx1200 makes the mock report GC 12.0.0. The gfx1200 device fields match the RX 9060 XT Linux amdgpu DRM_INFO topology.

0011: hide asynchronous COMPUTE rings on native/mock connections. The kernel Submit parser accepts only GFX instance 0, ring 0; RADV's general GFX queue still supports compute dispatch. HOST_BO retains its synthetic ring masks. Also replaces null-pointer member arithmetic in the vendored VA-manager list helper with offsetof (UBSan failed during device teardown).

0012: preserve the last accepted sequence and stop the connection after a partial split-submit failure or a post-submit sync-object attachment failure. Return ECANCELED instead of a retryable ENOMEM after work was accepted, so RADV cannot replay accepted work. Fence queries remain available for cleanup.

0013: propagate backend errors from timeline queries. On Darwin, sync queries and waits latch DEVICE_LOST for ECANCELED or ENODEV, while ordinary wait expiration remains VK_TIMEOUT. The existing mock hang mode reproduces false query success and UNKNOWN wait errors on the previous dylib; both now report DEVICE_LOST and reject resubmission.

0014: report native host-import failures without treating an unsuccessful import as a valid BO.

0015: export radv_darwin_buffer_copy/fill/wait for VkDeviceMemory plus allocation-relative offsets on RADV's original RDNA4N connection. Copy/fill require QueryInfo.flags RDNA4N_INFO_BUFFER_OPS (bit 3); an older kernel returns -ENOSYS without submitting. ABI minor is unchanged. The caller must keep device/memory alive and order these immediate operations with Vulkan submissions; this patch does not connect Metal recording. Size is dword-aligned, 4 bytes through 1 MiB; imported/virtual memory and overlapping copies are rejected.

0016: implement the VRAM/GTT memory query and distribute native shared headroom once across the disjoint local heaps. Preserve real usage counters and physical heap capacities.

0017: back native Private allocations above 0xfffffffc bytes with physical children in one contiguous GPU VA. Children are at most 256 MiB, reduced only on physical ENOMEM. The fully backed parent uses no sparse operations; CS references expand to child handles. CPU mapping and external export are rejected.

0018: import native host-pointer ranges above 64 MiB as 64 MiB children in one GPU VA. Borrow the caller's CPU pointer, retain every child for submission, and unwind failed imports in reverse order. The parent never unmaps caller-owned CPU memory.

0019: report an 8 GiB logical allocation/buffer ceiling on the native backend (bounded by VRAM capacity), retaining existing descriptor range limits. Reject oversized creates, saturate hardware vertex/index windows, and exclude composite BOs from the single-handle native transfer bridge. Complete Metal support also requires the matching 64-bit translator and storage/transfer paths; these limits alone are not GPU execution evidence.

0020: split Darwin copy/fill ranges before entering the 32-bit compute kernels. Keep addresses and range advancement in 64 bits, bound the byte-copy dispatch calculation, preserve the existing backend choice for tails, and stop after recording errors.

0021: hide sparse resources on the native backend, which has no PRT mappings or sparse binds.

0022: export versioned read-only physical and update-after-bind limits from libvulkan_radeon without creating a Vulkan object or opening the native connection. The pure export shares the physical-property descriptor-size and VRAM-ceiling helpers. It accepts optional native PCI/GC/VRAM facts, marks chip-dependent memory limits unavailable when those facts are absent, and never fabricates pipelineCacheUUID. Hardware acceptance still requires a full-open cross-check.

0023: propagate failed graphics shader allocation instead of caching or dereferencing an incomplete pipeline.

0024: on Darwin, retry an ordinary CPU-mapped shader arena in GTT after a visible-VRAM device-memory error. Preserve its flags and alignment, and do not retry capture/replay or non-mappable arenas.

0025: disable GFX12 color compression on Darwin. Garnet accepts the allocation flag without setting the GPU page table compression bit. Keep image format queries, image creation, and allocation on the uncompressed path. The gfx1200 pipeline probe checks the reported color attachment compression mode.

0026: enable VK_EXT_metal_surface on RADV for Darwin builds (src/amd/vulkan/radv_instance.c) and route WSI Metal swapchains lacking bind_drawable_to_vkimage through the CPU present blit path with explicit fence waits before wsi_metal_layer_blit_and_present (src/vulkan/wsi/wsi_common_metal.c). Absorbed and adapted from RX4Darwin/AMDRadeonRDNA4.kext (commit 6811730c, BSD 3-Clause).

0027: define RADV_USE_WSI_PLATFORM in src/amd/vulkan/radv_wsi.h when VK_USE_PLATFORM_METAL_EXT is enabled, so RADV exposes VK_KHR_surface, VK_KHR_swapchain, and related WSI extensions alongside VK_EXT_metal_surface, and skip the private SDMA blit queue in src/amd/vulkan/radv_wsi.c when ip[AMD_IP_SDMA].num_queues == 0 so WSI CPU-blit swapchains execute on the graphics queue.

0028: sync include/darwin/rdna4_native_abi.h with GarnetNativeABI.h ABI 1.13 (RDNA4N_CAP_VM_PRT = 1u << 4, RDNA4N_VA_OP_CLEAR/REPLACE, RDNA4N_VM_PAGE_PRT), enable CLEAR/REPLACE/PRT VA operations in the Darwin winsys and mock when QueryInfo.flags advertises RDNA4N_CAP_VM_PRT (continuing to reject them with -EOPNOTSUPP on older ABI <= 1.12 kernels and HOST_BO), replace 0021's unconditional false in radv_sparse_enabled() with a runtime RDNA4N_CAP_VM_PRT capability check that preserves Mesa's sparse disable flags, and reject composite child-backed BOs as sparse backing targets.

0029: split radv_mesh_enabled() from radv_taskmesh_enabled() in src/amd/vulkan/radv_physical_device.c so GFX10.3+ / GFX12 hardware exposes VK_EXT_mesh_shader (meshShader=true, multiviewMeshShader=true, primitiveFragmentShadingRateMeshShader=true, meshShaderQueries=false) on GFX ring 0 when ip[AMD_IP_COMPUTE].num_queues == 0, while keeping taskShader, meshAndTaskShaderDerivatives, and VK_SHADER_STAGE_TASK_BIT_EXT gated on radv_compute_queue_enabled(). Zero kext/ABI changes; mesh-only draws run on GFX ring 0 via PKT3_DISPATCH_MESH_DIRECT.

0030: sync RDNA4N_CAP_GFX12_DCC (1u << 5) into include/darwin/rdna4_native_abi.h and ac_darwin_kext.c; report drm_minor = 60 in acd_device_initialize() when QueryInfo.flags advertises RDNA4N_CAP_GFX12_DCC (unlocking gfx12_supports_dcc_write_compress_disable), and gate radv_is_dcc_disabled() on Darwin on !pdev->info.gfx12_supports_dcc_write_compress_disable (superseding 0025's unconditional GFX12 disable while preserving RADV_DEBUG=nodcc, garnet-dcc=0, and HOST_BO fallback).

0031: sync RDNA4N_HW_IP_COMPUTE (1u), RDNA4N_CAP_ASYNC_COMPUTE (1u << 6), and RDNA4N_SEQ_RING_COMPUTE (1ull << 62) into include/darwin/rdna4_native_abi.h and ac_darwin_kext.c; expose available_rings = 1 for AMDGPU_HW_IP_COMPUTE when QueryInfo.flags advertises RDNA4N_CAP_ASYNC_COMPUTE (unlocking a dedicated Vulkan compute queue family and Task+Mesh in 0029 while preserving available_rings = 0 on older kernels, garnet-async-compute=0, and mock); upgrade ac_darwin_sync.c to per-ring retirement (known_retired[2]), per-ring pending pairs (seq[2]) for gang submissions, multi-point cross-ring timeline wait polling, and cross-ring synchronization in acd_sync_cs_wait_ready(); partition mixed COMPUTE+GFX gang submissions in acd_kext_submit_ex() for concurrent Task+Mesh launch while propagating both leader and follower seqnos into last_compute_seq and syncobjs; drain both rings in acd_cmd_gem_wait_idle(). Exercised by `mesa-patches/tests/dual_ring_sync_test.c` (invoked from `mesa-patches/tests/test-gfx1200-runtime.sh`).

0032: use IDSet for interference tracking in aco_spill.cpp (`0032-aco-spill-use-idset-for-interferences.patch`), cutting peak host memory by ~7x to 14x when compiling large MetalRT / Blender Cycles megakernels without OOM.

0033: sync include/darwin/rdna4_native_abi.h with GarnetNativeABI.h ABI 1.14 (reserving selector 29, flags bit 7, and seqno bit 61 for VCN 5.0 ABI 1.15; adding WebKit-silent APP aliases 30/31/39 for BOCREATE/BO_IMPORT_HOST/BUFFER_COPY and info flags bits 8..10); route RADV's GarnetAccelerator connection through selectors 30/31/39 on ABI >= 1.14 kernels with automatic fallback to 3/21/27 on ABI <= 1.13 kernels and on the GarnetDriver route. Verified by `python3 mesa-patches/test-webkit-safe-selectors.py "$TMPDIR/webkit-sel-test"`.

Active RADV Darwin tests live under `mesa-patches/tests/`:
    - Host runtime / submit / dual-ring sync / kernel Submit contract:
        bash mesa-patches/tests/test-gfx1200-runtime.sh /path/to/patched-mesa [output-dir]
    - Native macOS mock pipeline / queue marshalling / hang propagation:
        bash mesa-patches/tests/test-gfx1200-pipeline.sh /path/to/patched-mesa /path/to/libvulkan_radeon.dylib /path/to/output
    - Standalone Vulkan sparse / GFX12 PRT residency test:
        cc -std=c11 -O2 -Wall -Wextra -Werror mesa-patches/tests/vulkan_sparse_prt_test.c -lvulkan -o "$TMPDIR/vulkan_sparse_prt_test"
    - Argument-limit export test: `mesa-patches/tests/radv_darwin_argument_limits_test.c`

Memory and topology audit (offline, 2026-11-01):
    The gfx1200 .mem values are synthetic HOST_BO/drm-shim inputs, copied from
    gfx1201; they do not determine the native RX 9060 XT memory size. Native
    AMDGPU_INFO_MEMORY and usage queries fetch QueryInfo each time. VRAM is
    vram_vis_total + vram_hi_total (allocator pools, not physical chip capacity);
    the CPU-visible heap uses vram_vis_total, GTT uses gtt_cap/gtt_max_bo, and
    patch0006 applies the ABI 1.10 session budget. No 16 GiB assumption is used
    for native heap sizing. Synthetic 8/16 GiB and all-visible profiles are tested.

    QueryInfo's GC encoding is (major << 16) | (minor << 8) | revision. It
    selects the canned device ID, firmware versions, marketing name, and
    topology together. Unknown nonzero versions fail unless explicitly
    overridden. GC=0 is a legacy assumption, not positive chip identification.
    Native GB_ADDR_CONFIG must match the selected entry unless the existing
    debug override is set. The mock uses the same assumed GB_ADDR_CONFIG.

    QueryInfo does not carry PCI identity, CU/RB harvest masks, TCC/cache sizes,
    or clocks. The target dump confirms the selected CU/RB topology, GB_ADDR_CONFIG,
    cache sizes, clocks, PCIe 5 x16 and VA limit. The kext's own physical-device
    report and native GPU execution still require a physical acceptance run.

Host regression test (no Mesa configure, IOKit, Vulkan loader, or GPU needed):
    bash mesa-patches/tests/test-gfx1200-runtime.sh /path/to/patched-mesa [output-dir]
    Run from this repository after applying the full series to the pinned
    Mesa base. Requires clang/clang++ (or CC/CXX), pthreads, and sanitizer
    runtimes. Builds the actual backend, marshaller, mock, sync provider,
    VA manager and canned tables, plus the kernel's pure Submit validator.
    Tests cover GC selection/overrides/rejection, shared cores, GB checks,
    live memory pools/budgets, HOST_BO/mock, native queue capability, partial
    acceptance, a timeline allocation failure after kernel acceptance, timeline
    query errors, successful retirement, and ordinary timeout preservation.
    The new tests use -Wall -Wextra -Werror; existing Mesa C files use -Wall.
    All binaries run with AddressSanitizer and UndefinedBehaviorSanitizer.
    LeakSanitizer is enabled on Linux; Apple's ASan does not support leak
    detection, so the script disables that option on Darwin.

Native macOS build (ACO only, no LLVM/Rust/system installations):
    Apply the series to the pinned source, then use Python >= 3.10 with
    meson, ninja, mako, packaging and PyYAML in a user venv. CLT's Python
    3.9 is too old. glslangValidator >= 12.2 is required for RADV's BVH
    shader generation even with ACO; glslang 16.1.0 can be built with
    CMake/Ninja and -DENABLE_OPT=OFF -DGLSLANG_TESTS=OFF into a user prefix,
    without installing SPIRV-Tools or LLVM. Put both prefixes on PATH.

    meson setup radv-build /path/to/patched-mesa --buildtype=release \
      --prefix="/path/to/mesa-install" \
      -Dgallium-drivers= -Dvulkan-drivers=amd -Dplatforms=macos \
      -Dllvm=disabled -Damd-use-llvm=false -Dglx=disabled \
      -Degl=disabled -Dgbm=disabled -Dopengl=false \
      -Dgles1=disabled -Dgles2=disabled -Dspirv-tools=disabled \
      -Dzstd=disabled -Dvalgrind=disabled -Dlibunwind=disabled \
      -Dbuild-tests=false -Dbuild-aco-tests=false
    meson compile -C radv-build -j3 vulkan_radeon
    meson compile -C radv-build -j3 radeon_icd radeon_devenv_icd

    On macOS 26.x x86_64 with Apple clang, `tools/rdna4/build-radv.sh` automates
    patching and compiling `libvulkan_radeon.dylib` with ACO only (no LLVM or
    Rust required) and generates both the development and install ICD manifests.

Native Vulkan mock pipeline test (macOS, no queue submission):
    Put glslangValidator on PATH, then run:
    bash mesa-patches/tests/test-gfx1200-pipeline.sh /path/to/patched-mesa \
      /path/to/radv-build/src/amd/vulkan/libvulkan_radeon.dylib /path/to/output
    This loads the ICD directly through vk_icdGetInstanceProcAddr; no Vulkan
    loader installation is needed. The harness forces RADV_DARWIN_MOCK=1 and
    RADV_DARWIN_MOCK_GC=gfx1200, clears chip/HOST_BO overrides, disables shader
    caches, and compiles an owned compute pipeline, verifying physical-device
    enumeration, queue families, device creation, and nonempty ACO IR through
    VK_KHR_pipeline_executable_properties.

    Add --submit to exercise real RADV queue marshalling through mock RDNA4N:
    one compute command buffer, 65 simultaneous-use command buffers (split into
    64+2 IBs including the preamble), timeline signal/wait, and an empty submit.
    The test verifies fence completion and timeline values while confirming that
    mock acceptance never executes the shader.

    Use --query-hang or --wait-hang for a single compute submission followed by
    an injected mock hang. Both modes require DEVICE_LOST on the detecting call
    and later calls, unchanged query output, and refusal of later submissions.

Offline host and mock checks validate patch application, marshalling, sync
state machines, and ACO lowering; physical GPU execution requires running on
target RDNA 4 hardware.
