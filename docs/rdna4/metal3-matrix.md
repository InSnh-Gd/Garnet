# Fixed Metal 3 acceptance matrix (`AMD 6000-series` parity)

This matrix defines `implement-metal3-amd6000x-parity` task 1.1. It extends the
frozen Metal 2 (`Mac2` / `macOS_GPUFamily2_v1` / MSL 2.0–2.1 + 8 GiB
large-memory) baseline in [metal2-matrix.md](metal2-matrix.md) (commit
`1736ee8`). An entry is complete only when its execution oracle and
negative/boundary checks pass on the target hardware. Host C/Rust unit tests,
SPIR-V validation, and Mesa LavaPipe runs establish intermediate layers; they do
not close RX 9060 XT execution. No feature is marked complete by this document.

## Pinned baseline, target hardware, and scope

| Item | Fixed definition |
| --- | --- |
| Physical target | AMD Radeon RX 9060 XT 16 GB (Navi 44, `gfx1200`, GC 12.0.0, PCI `1002:7590`), macOS 26.7.2 (`25G332`), SDK 26.5 |
| Standalone stack | `Garnet.kext` (`'GRNT'` native ABI `1.12 -> 1.13`), `GarnetAccel.kext`, `GarnetPCIBarCompat.kext`, `GarnetMetal.bundle` over Mesa RADV (`mesa@f5cb8ee` + `mesa-patches/`) and `metal2vulkan@43c46ac8` + `third-party/metal2vulkan-rdna4/` (`garnetxlate`) |
| Official feature anchor | Apple `Metal-Feature-Set-Tables` (2025-10 / 2026-05 editions), Intel Mac `AMD 6000-series` (`Mac2` + `Metal3`), MSL 2.2–3.2 |
| Preserved Metal 2 gates | All rows of [metal2-matrix.md](metal2-matrix.md) (`C01..C07`, `K01..K07`, `T01..T07`, `R01..R09`, `A01..A05`, `P01..P04`, `I01..I04`, `H01..H05`, `L01..L23`, `F01..F33`, `M01..M08`) and compatibility/execution suites (`47/95/18/8`, B1, ICB, Tessellation, Tier 2, 8 GiB) remain non-regression gates |
| Independent boundary | Zero Apple `AMDRadeonX6000` binary reuse or substitution; zero `Lilu`, `AMDRadeonRDNA4.kext`, or `AppleParavirtGPU`; no closed-source `MetalFX` internal protocols |
| Public family gate | Task `8.2` certified: application and WindowServer `supportsFamily:(MTLGPUFamilyMetal3)` (`5001`) both return `YES` after live RX 9060 XT verification of `X01..X08`, `D01..D03`, `Q01..Q02`, `S01..S03`, `G01..G03`, and `RT01..RT03`; `EX01..EX08` (`Apple1..10` incl. `Apple6`, `Mac1`, `MacCatalyst`, `Metal4`, TBDR/`simdgroup_matrix`) remain strictly `NO`. |

## Required acceptance entries

Every ID is unique across `metal2-matrix.md` and `metal3-matrix.md`. Every oracle
must use defined inputs, check the full relevant result, retain untouched guard
regions, and validate rejected error paths.

### 1. Matrix and runtime backend capability probe (`B01..B02`)

| ID | Feature / requirement | Capability query / selector | Vulkan / stack dependency | Required oracle & negative checks | Tasks |
| --- | --- | --- | --- | --- | --- |
| B01 | Fixed `AMD 6000-series` (`Mac2` + `Metal3`) acceptance matrix, limits, and Apple Silicon exclusion ledger | `docs/rdna4/metal3-matrix.md` + Task `8.2` capability audit | Apple `Metal-Feature-Set-Tables` (`AMD 6000-series`), OpenSpec capability specs | Every capability query, limit, exclusion, and task (`1.1..9.2`) maps to an explicit oracle with no ambiguous rows | 1.1, 8.2 |
| B02 | Conservative pre-open fallback and runtime Vulkan Metal 3 extension/feature/limit probe | `rdna4dc_metal3_caps`, `rdna4dc_metal3_from_probe()`, `rdna4_metal3_caps()` (`rdna4_devcaps.h`, `rdna4_backend_impl.inc`) | `VK_KHR_fragment_shader_barycentric`, `VK_EXT_shader_atomic_float` (+ `float2`), `VK_KHR_shader_atomic_int64` (Vulkan 1.2 core), `VK_EXT_mesh_shader`, `VK_KHR_deferred_host_operations`, `VK_KHR_acceleration_structure`, `VK_KHR_ray_query`, timestamp limits, sparse features | Pre-open `rdna4dc_fallback()` / `rdna4dc_from_native_memory()` and missing-dependency probes keep all Metal 3 bits `0`/`false`; probed-on flags gate accurately and clamp oversized mesh limits (`src/garnet-metal/tests/test-host-runtime.c` `-Wall -Wextra -Werror`) | 1.2, 8.1 |

### 2. Metal 2.2–2.4 and MSL 2.2–3.2 core features (`X01..X08`)

| ID | Feature / requirement | Capability query / selector | Vulkan / stack dependency | Required oracle & negative checks | Tasks |
| --- | --- | --- | --- | --- | --- |
| X01 | Texture Swizzle (`MTLTextureDescriptor.swizzle`, `newTextureViewWithPixelFormat:textureType:levels:slices:swizzle:`) | `Mac2` / `Metal3` baseline (macOS 10.15+) | `VkComponentMapping` composition in `rdna4_view_map` over base format swizzles (e.g., `A8Unorm`) | Custom channel remaps (`R/G/B/A`, `Zero`, `One`) match per-channel reference without modifying base texture or unswizzled views; invalid swizzle enum or illegal non-identity swizzle on storage-write/render-target views returns `nil`/error (`test-metal3-swizzle.m`, host C tests) | 2.1, 8.1, 9.1 |
| X02 | Placement Heaps (`MTLHeapTypePlacement`, `newBufferWithLength:options:offset:`, `newTextureWithDescriptor:offset:`, `heapOffset`) | `MTLHeapDescriptor.type = MTLHeapTypePlacement`; `heapBufferSizeAndAlignWithLength:options:`; `heapTextureSizeAndAlignWithDescriptor:` | `VkDeviceMemory` explicit-offset bind + `vkGetBufferMemoryRequirements` / `vkGetImageMemoryRequirements` + `bufferImageGranularity` | Returned `heapOffset` matches request, non-overlapping regions stay isolated, post-sync explicit-offset aliasing is visible to subsequent reads, `currentAllocatedSize`/`usedSize`/`maxAvailableSizeWithAlignment:` match accounting; misaligned, out-of-bounds, or over-capacity placement returns `nil` (`test-metal3-placement-heap.m`, host C tests) | 2.2, 8.1, 9.1 |
| X03 | Compute Indirect Command Buffers (`MTLIndirectCommandTypeConcurrentDispatch`, `ConcurrentDispatchThreads`, CPU & GPU encoding) | `MTLIndirectCommandBufferDescriptor.commandTypes` (compute bits accepted only once validated) | `vkCmdDispatchIndirect` + dynamic binding expansion + `garnetx_lower_icb_stage` ABI upgrade (backward-compatible with render ICB) | CPU and GPU encoded multi-pipeline compute ICB subranges match direct dispatch, unset/reset slots no-op, GPU path has `cpu_readback=0`, early-release and partial-submit failure preserve safe retirement (`test-metal3-compute-icb.m`, SPIR-V & LavaPipe tests) | 2.3, 8.1, 9.1 |
| X04 | Shader Barycentric Coordinates (`[[barycentric_coord]]`, perspective & `noperspective`) | `supportsShaderBarycentricCoordinates` (gated by `rdna4dc_metal3_barycentric_ok`) | `VK_KHR_fragment_shader_barycentric` (`fragmentShaderBarycentric`) -> SPIR-V `FragmentBarycentricKHR` (`BaryCoordKHR` / `BaryCoordNoPerspKHR`) | Fragment weights sum to `1.0` and reconstruct triangle vertex attributes accurately; unsupported backend reports `NO` (`test-metal3-bary-pull.m`, Rust + LavaPipe Vulkan 1.4) | 1.2, 2.4, 8.1, 9.1 |
| X05 | Pull-Model Interpolation (`interpolant<T>::interpolate_at_center`, `interpolate_at_centroid`, `interpolate_at_sample`, `interpolate_at_offset`) | `supportsPullModelInterpolation` (gated by `rdna4dc_metal3_pull_model_ok`) | Vulkan 1.0 core `sampleRateShading` -> SPIR-V `SampleRateShading` + `GLSL.std.450` `InterpolateAtCentroid` / `Sample` / `Offset` | MSAA per-sample and explicit subpixel-offset interpolants match analytic reference; unsupported backend reports `NO` (`test-metal3-bary-pull.m`, Rust + LavaPipe Vulkan 1.4) | 1.2, 2.4, 8.1, 9.1 |
| X06 | GPU Timestamp Counter Sampling (`MTLCounterSampleBuffer`, `MTLCommonCounterSetTimestamp`) | `counterSets`, `supportsCounterSampling:` (`AtStageBoundary`, `AtDrawBoundary`, `AtDispatchBoundary`, `AtBlitBoundary`), `sampleTimestamps:gpuTimestamp:`, `resolveCounterRange:` (gated by `rdna4dc_metal3_timestamps_supported`) | `VkQueryPool(VK_QUERY_TYPE_TIMESTAMP)`, `vkCmdWriteTimestamp2` / `vkCmdWriteTimestamp`, `timestampPeriod`, `timestampValidBits` | Real compute/render/blit work resolves valid monotonic nanosecond timestamps (`end >= start`, non-zero); out-of-range sample index, unsupported sampling point, or invalid descriptor returns controlled `NSError` (`test-metal3-counters-amp.m`, host C tests) | 1.2, 2.5, 8.1, 9.1 |
| X07 | Vertex Amplification (`setVertexAmplificationCount:viewMappings:`, `[[amplification_id]]`, `[[amplification_count]]`) | `supportsVertexAmplificationCount:` (`1` baseline; `> 1` only when target-verified; unsupported counts return `NO`) | `multiViewport` / layered view mapping / Vulkan 1.1 core multiview | Amplified draw writes distinct viewport/layer outputs matching CPU reference; counts exceeding reported support or invalid view mappings are refused (`test-metal3-counters-amp.m`) | 2.5, 8.1, 9.1 |
| X08 | MSL 2.2–3.2 float/64-bit/texture atomics, `bfloat`, and SIMD shift/fill (`atomic<float>`, `atomic<int64_t>`/`uint64_t`, `bfloat`, texture atomics, `simd_shuffle_and_fill_*`) | Gated by `rdna4dc_metal3_atomic_float_ok`, `atomic_float_minmax`, `atomic_float_image`, `rdna4dc_metal3_atomic_int64_ok` | `VK_EXT_shader_atomic_float`, `VK_EXT_shader_atomic_float2`, Vulkan 1.2 core `shaderBufferInt64Atomics`/`shaderSharedInt64Atomics` (`VK_KHR_shader_atomic_int64`) + `shaderInt64`, core storage-image atomics, subgroup shuffles | Contended multi-thread buffer/threadgroup/texture atomic sums/min/max/CAS, `bfloat` conversions/math, and SIMD fill results match race-free references (`test-metal3-msl3-atomics.m`, Rust + LavaPipe Vulkan 1.4) | 1.2, 2.6, 8.1, 9.1 |

### 3. Pipeline modularity and dynamic function dispatch (`D01..D03`)

| ID | Feature / requirement | Capability query / selector | Vulkan / stack dependency | Required oracle & negative checks | Tasks |
| --- | --- | --- | --- | --- | --- |
| D01 | `MTLBinaryArchive` (`newBinaryArchiveWithDescriptor:error:`, `addComputePipelineFunctionsWithDescriptor:error:`, `addRenderPipelineFunctionsWithDescriptor:error:`, `serializeToURL:error:`, pipeline `binaryArchives`) | `supportsBinaryArchives` (`rdna4x_active()`: `YES` for admitted Metal 3 applications/inproc tests, `NO` for WindowServer) | Versioned Garnet container (magic, ABI/RADV `pipelineCacheUUID`, SHA-256, normalized descriptor key -> SPIR-V + reflection JSON + `VkPipelineCache`) | Serialize + reload in fresh instance hits archive and produces bit-identical GPU output; tampered header/hash, UUID mismatch, foreign Apple `.metallib` slice, or unsupported tile pipeline returns `MTLBinaryArchiveError` without crash (`test-metal3-binary-archive.m`, host C codec tests) | 3.1, 8.1, 8.2, 9.1 |
| D02 | `MTLDynamicLibrary` (`newDynamicLibrary:error:`, `newDynamicLibraryWithURL:error:`, `serializeToURL:error:`, `preloadedLibraries`, `binaryFunctions`) | `supportsDynamicLibraries` (compute), `supportsRenderDynamicLibraries` (vertex/fragment) | Garnet dynamic library container (sanitized AIR + export table) + `garnetx_translate_linked` symbol linking in `rdna4_shader_runtime_impl.inc` | Compute, vertex, and fragment pipelines invoke exported dynamic-library functions with exact GPU output; missing symbol or invalid file returns `NSError` at creation (`test-metal3-dynamic-lib.m`, Rust link tests) | 3.2, 8.1, 9.1 |
| D03 | `MTLVisibleFunctionTable` and function pointers (`newVisibleFunctionTableWithDescriptor:`, `newVisibleFunctionTableWithDescriptor:stage:`, `functionHandleWithFunction:`, `setFunction:atIndex:`, `setFunctions:withRange:`, `visible_function_table<...>`) | `supportsFunctionPointers` (compute), `supportsFunctionPointersFromRender` (render) | Compile-time `MTLLinkedFunctions` closure + bounded `OpSwitch` dispatch lowering in `metal2vulkan-rdna4` + device slot-ID buffer; Footnote 10 mesh-pipeline exclusion check | Per-thread/per-pixel runtime table index invokes distinct linked functions (including nested resource access); out-of-bounds/empty slot is isolated without GPU hang; Mesh pipeline requesting incompatible render function pointers fails creation with `NSError` (`test-metal3-function-tables.m`, Rust + LavaPipe) | 3.3, 6.2, 8.1, 9.1 |

### 4. Fast resource loading and residency sets (`Q01..Q02`)

| ID | Feature / requirement | Capability query / selector | Vulkan / stack dependency | Required oracle & negative checks | Tasks |
| --- | --- | --- | --- | --- | --- |
| Q01 | Fast Resource Loading (`MTLIOFileHandle`, `MTLIOCommandQueue`, `MTLIOCommandBuffer`: `loadBuffer:`, `loadTexture:`, `addBarrier`, `signalEvent:value:`, `waitForEvent:value:`, `tryCancel`) | `Mac2` + `Metal3` baseline (macOS 13+ API; limit `RDNA4DC_IO_MAX_COMMANDS = 8192`) | Bounded async `pread` worker + recyclable Shared staging ring + `vkCmdCopyBuffer`/`vkCmdCopyBufferToImage` + serial command scheduler + `MTLSharedEvent` | Async load into Private buffer & mipmapped texture + event handoff reads back bit-exact bytes with untouched guards and early handle release safety; missing file, EOF overrun, `> 8192` commands, unsupported compression codec, or `tryCancel` yields controlled `MTLIOError` (`test-metal3-io-queue.m`, host C state-machine tests) | 4.1, 8.1, 9.1 |
| Q02 | Residency Sets (`MTLResidencySet`: `newResidencySetWithDescriptor:error:`, `addAllocation:`, `removeAllocation:`, `commit`, `MTLCommandQueue.addResidencySet:`, `MTLCommandBuffer.useResidencySet:`) | Metal 3.2 (macOS 15+ API) | Immutable committed allocation snapshot merged into command-buffer retirement retention (buffers, textures, heaps, acceleration structures) | Indirect BDA / Tier 2 / RT resources survive early app ARC release (`-O0`/`-O2`) until GPU retirement, and reclaim cleanly without VRAM leak after `removeAllocation:` + `commit` (`test-metal3-residency-set.m`, host C/ObjC lifetime tests) | 4.2, 8.1, 9.1 |

### 5. Full-stack GFX12 Sparse Resources / PRT (`S01..S03`, independent of `Apple6`)

In Apple's `Metal-Feature-Set-Tables`, Metal sparse textures are classified
under `Apple6+`, not `AMD 6000-series` (`Mac2` + `Metal3`). Garnet implements
full-stack GFX12 PRT and sparse resources as a separate hardware capability to
retire `mesa-patches/0021` and support genuine sparse residency, while
**explicitly keeping `MTLGPUFamilyApple6 == NO`** and all Apple Silicon TBDR
queries `NO`.

| ID | Feature / requirement | Capability query / selector | Vulkan / stack dependency | Required oracle & negative checks | Tasks |
| --- | --- | --- | --- | --- | --- |
| S01 | Kernel GFX12 PRT VM & atomic `CLEAR`/`REPLACE` (`Garnet.kext`) | `'GRNT'` ABI `RDNA4N_ABI_MINOR = 13`, negotiated capability bit `RDNA4N_CAP_VM_PRT` | `RDNA4N_VA_OP_CLEAR`, `RDNA4N_VA_OP_REPLACE`, `RDNA4N_VM_PAGE_PRT` (`handle == 0`), GFX12 PTE `AMDGPU_PTE_PRT_GFX12 (1ULL << 56) | SNOOPED | SYSTEM | IS_PTE` with `AMDGPU_PTE_VALID` cleared; interval splitting, mandatory TLB flush, PT quota & rollback | `native_prt_sparse_test.cpp` (ASan+UBSan) covers PTE encoding, head/middle/tail splitting, TLB invalidation, quota exhaustion, invalid-arg atomic rejection, and session cleanup; physical non-resident read-zero / write-discard with `DEAD/HUNG/gcvm = 0` | 5.1, 8.1, 9.1 |
| S02 | Mesa RADV Darwin Vulkan sparse binding & residency | Vulkan `sparseBinding`, `sparseResidencyBuffer`, `sparseResidencyImage2D`, `sparseResidencyImage3D`, `sparseResidency2/4/8/16Samples`, `sparseResidencyAliased`, `VK_QUEUE_SPARSE_BINDING_BIT` (`rdna4dc_metal3_caps.sparse_*`, default `0`) | `mesa-patches/0028+` negotiating `RDNA4N_CAP_VM_PRT` (old `minor <= 12` kernels stay disabled; supersedes static `0021` after physical validation), `vkQueueBindSparse` | `mesa@f5cb8ee` patch replay clean; physical `vulkaninfo` has zero `-22` / `ERROR_OUT_OF_DEVICE_MEMORY`; Vulkan sparse buffer/2D/3D/MSAA resident read/write + unbind read-zero/write-discard pass | 1.2, 5.2, 8.1, 9.1, 9.2 |
| S03 | Metal sparse textures & shader residency predicates | `sparseTileSizeWithTextureType:pixelFormat:sampleCount:`, `sparseTileSizeInBytes` (`65536`), `convertSparsePixelRegions:toTileRegions:withTileSize:alignmentMode:numRegions:`, `MTLResourceStateCommandEncoder` (gated by `rdna4dc_metal3_sparse_texture_ok`; **`MTLGPUFamilyApple6` MUST stay `NO`**) | `vkQueueBindSparse` via command scheduler + `metal2vulkan-rdna4` `OpImageSparseSample*` / `OpImageSparseFetch` + `OpImageSparseTexelsResident` (`sparse_color<T>::resident()`) | Mapped 64 KiB tiles return `resident() == true` + exact texels, unmapped tiles return `resident() == false` + zero with no GPU fault, and `supportsFamily:(MTLGPUFamilyApple6) == NO` (`test-metal3-sparse-prt.m`, Rust + LavaPipe) | 1.2, 5.3, 8.1, 9.1 |

### 6. Mesh and Object Shaders (`G01..G03`)

| ID | Feature / requirement | Capability query / selector | Vulkan / stack dependency | Required oracle & negative checks | Tasks |
| --- | --- | --- | --- | --- | --- |
| G01 | AIR Object & Mesh shader translation | `rdna4dc_metal3_mesh_supported` (`mesh_shader && object_shader`, payload `>= 16384`, mesh grid `>= 1024`) | `VK_EXT_mesh_shader` (`taskShader`, `meshShader`), Vulkan 1.2 / SPIR-V 1.4 (`TaskEXT`, `TaskPayloadWorkgroupEXT`, `OpEmitMeshTasksEXT`, `MeshEXT`, `OpSetMeshOutputsEXT`, `PerPrimitiveEXT`), `garnetxlate` reflection JSON | Patch replay + Rust unit tests + LavaPipe `VK_EXT_mesh_shader` / `spirv-val` cover `16384` B payload, point/line/triangle topologies, and per-vertex/per-primitive outputs | 1.2, 6.1, 8.1 |
| G02 | `MTLMeshRenderPipelineDescriptor` creation, limits, and Footnote 10 mutual exclusion | `newRenderPipelineStateWithMeshDescriptor:options:reflection:error:` (+ async overloads); part of `MTLGPUFamilyMetal3` parity gate | `VK_EXT_mesh_shader` graphics pipeline + stage linkage + Apple Footnote 10 validation (Mesh pipeline vs render function pointers / render ray tracing) | Mesh-only and object+mesh+fragment pipelines match CPU reference geometry/per-primitive attributes/culling; `> 16384` B payload, illegal topology/linkage, or Footnote 10 Mesh + render function-pointer/RT combination returns explicit `NSError` (`test-metal3-mesh-shaders.m`) | 1.2, 3.3, 6.2, 8.1, 9.1 |
| G03 | Direct and indirect mesh draws (`setObject*`, `setMesh*`, `drawMeshThreadgroups:`, `drawMeshThreads:`, `drawMeshThreadgroupsWithIndirectBuffer:`) | `MTLRenderCommandEncoder` mesh/object binding and draw selectors | `vkCmdDrawMeshTasksEXT`, `vkCmdDrawMeshTasksIndirectEXT` | Compute producer writes `(gridX, gridY, gridZ)` to indirect buffer (including 0-grid no-op, non-zero offsets, multi-draw ordering, variable object-emitted mesh grid `>= 1024`) and pixels match CPU reference; out-of-bounds indirect buffer or illegal workgroup dimensions return controlled error (`test-metal3-mesh-shaders.m`) | 6.2, 8.1, 9.1 |

### 7. Hardware Ray Tracing (`RT01..RT04`)

| ID | Feature / requirement | Capability query / selector | Vulkan / stack dependency | Required oracle & negative checks | Tasks |
| --- | --- | --- | --- | --- | --- |
| RT01 | `MTLAccelerationStructure` & `MTLAccelerationStructureCommandEncoder` (triangle BLAS, bounding-box BLAS, instance TLAS, build, refit, writeCompactedSize, copyAndCompact, copy) | `supportsRaytracing` (gated by `rdna4dc_metal3_raytracing_supported` + verified execution); `accelerationStructureSizesWithDescriptor:` | `VK_KHR_deferred_host_operations`, `VK_KHR_acceleration_structure` (`accelerationStructure`, `bufferDeviceAddress`, `minAccelerationStructureScratchOffsetAlignment`), Private `VkBuffer` + BDA + retirement list | Size queries match Vulkan requirements; build -> compact -> refit -> copy succeed; undersized scratch/AS buffer or invalid descriptor returns controlled error with zero UAF/hang (`test-metal3-rt-accel.m`, host layout/bounds tests) | 1.2, 7.1, 8.1, 9.1 |
| RT02 | Inline Ray Query lowering & compute ray tracing (`raytracing::intersector`, `air.intersect.*`) | `supportsRaytracing` | `VK_KHR_ray_query` (`rayQuery`) -> SPIR-V `RayQueryKHR` (`OpRayQueryInitializeKHR` / `ProceedKHR` / `ConfirmIntersectionKHR` / `GenerateIntersectionKHR`), `metal2vulkan-rdna4` | Triangle hit/miss, distance, primitive ID, instance ID/transform, visibility mask, hit barycentrics, and post-refit hits match analytic CPU ray-triangle oracle (`test-metal3-raytracing.m`, Rust + `spirv-val`) | 1.2, 7.2, 7.3, 8.1, 9.1 |
| RT03 | Render-stage ray tracing & `MTLIntersectionFunctionTable` (vertex/fragment TLAS binding incl. Tier 2 argument buffers; custom bounding-box intersection & triangle any-hit functions) | `supportsRaytracingFromRender` (subject to Footnote 10 mutual exclusion with Mesh pipelines); `newIntersectionFunctionTableWithDescriptor:` | `VK_KHR_ray_query` in vertex/fragment + `descriptorBindingAccelerationStructureUpdateAfterBind` + bounded switch-table intersection function dispatch | Fragment shadow/reflection rays and custom sphere bounding-box intersection distance/normal + any-hit filtering match analytic reference; early ARC release (`-O0`/`-O2`) has zero UAF (`test-metal3-raytracing.m`) | 1.2, 7.2, 7.3, 8.1, 9.1 |
| RT04 | Primitive motion blur (`MTLAccelerationStructureMotion*Descriptor`, `intersector::intersect(..., time)`) | `supportsPrimitiveMotionBlur` (strictly honest: `YES` only if verified hardware/interpolated execution passes; otherwise remains `NO`) | RADV GFX12 motion-blur extension check or verified keyframe-interpolated build/query path | Keyframe-interpolated intersection at `t in [0, 1]` matches analytic moving-primitive oracle, OR query stays `NO` and motion descriptors return controlled refusal (`test-metal3-raytracing.m`) | 7.1, 7.3, 8.1, 9.1 |

### 8. Integration, certification gate, and hardware acceptance (`Z01..Z03`)

| ID | Milestone / gate | Verification scope | Required oracle | Tasks |
| --- | --- | --- | --- | --- |
| Z01 | Full-stack pinned replay & non-regression | `metal2vulkan@43c46ac8` + patches, `mesa@f5cb8ee` + patches, all three kexts, `GarnetMetal.bundle`, `build-all.sh`, strict signing, load-only, `run-compatibility.py` | Zero undeclared capabilities, zero scratch dependencies, zero regressions on Metal 2 `47/95/18/8`, B1, ICB, Tessellation, Tier 2, and 8 GiB suites | 8.1 |
| Z02 | `MTLGPUFamilyMetal3` (`5001`) application certification gate | `rdna4_gate.h`, `GarnetDevice.m`, `test-gate.c`, `test-metal2-limits.m`, `docs/rdna4/metal3-matrix.md` | Flip app `supportsFamily:(MTLGPUFamilyMetal3)` to `YES` only after `X01..X08`, `D01..D03`, `Q01..Q02`, `G01..G03`, `RT01..RT03`, `Z01` pass live execution; lock `EX01..EX08` to `NO` and keep WindowServer unchanged | 8.2 |
| Z03 | RX 9060 XT isolated live suite and controlled deployment | Isolated `~/rdna4-build/` `test-metal3-*` + Metal 2/8 GiB/desktop regressions, then (only after explicit user release) `tools/rdna4/remote-hardware-test.sh` deploy, reboot/cold-boot, `vulkaninfo`, 30-min desktop soak | `logs/rdna4-metal3/<run>/RESULT.md` with SHA-256 provenance, `accel sessstat` `DEAD/HUNG/gcvm = 0`, `vulkaninfo` zero sparse errors, zero panic/hang/WindowServer restart, `restore` baseline retained | 9.1, 9.2 |

## Metal 3 numeric limits and rules (`ML01..ML14`)

These limits extend `L01..L23` and pixel formats `F01..F33` in
[metal2-matrix.md](metal2-matrix.md). Values come from Apple's official
`Metal-Feature-Set-Tables` (`AMD 6000-series` = `Mac2` + `Metal3`). Device
queries must reflect actual backend-probed limits and never inflate hardware
capabilities.

| Limit ID | Property / rule | Fixed value / contract | Tasks |
| --- | --- | --- | --- |
| ML01 | Max object-to-mesh threadgroup payload memory length | **16,384 bytes (16 KiB)** (`RDNA4DC_MESH_MAX_PAYLOAD_BYTES`; `maxTaskPayloadSize` clamped to 16,384; Metal 3 requires `>= 16,384`) | 1.2, 6.1, 6.2 |
| ML02 | Max mesh threadgroups per object threadgroup (mesh grid) | **`>= 1,024` threadgroups** (`RDNA4DC_MESH_MIN_GRID`; `maxMeshWorkGroupTotalCount >= 1024` and `maxMeshWorkGroupCount[0] >= 1024`, per-axis bounded by total count) | 1.2, 6.2 |
| ML03 | Max threads per object / mesh threadgroup | **1,024** (`RDNA4DC_MESH_MAX_INVOCATIONS`; clamped to `<= 1024`, matching `L06`) | 1.2, 6.2 |
| ML04 | Max total threadgroup memory for object / mesh stage | **32,768 bytes (32 KiB)**, 16-byte aligned (matching `L07`) | 1.2, 6.2 |
| ML05 | Max output vertices / primitives per mesh threadgroup | **256 vertices / 512 primitives** (`RDNA4DC_MESH_MAX_VERTICES` / `RDNA4DC_MESH_MAX_PRIMITIVES`, preserving lower backend actuals if `< 256`/`< 512`) | 1.2, 6.1, 6.2 |
| ML06 | Max I/O commands per `MTLIOCommandBuffer` | **8,192** (`RDNA4DC_IO_MAX_COMMANDS`; requests exceeding 8,192 fail with controlled `MTLIOError`) | 4.1 |
| ML07 | Footnote 10 mutual exclusion (`AMD 6000-series` / `Metal3`) | A render pipeline using Object/Mesh shaders **cannot** combine render-stage function pointers (`MTLVisibleFunctionTable` / render `MTLLinkedFunctions`) or render-stage ray tracing; pipeline creation MUST return a deterministic `NSError` | 3.3, 6.2, 7.3 |
| ML08 | Sparse texture tile size (GFX12 PRT, independent of `Apple6`) | **64 KiB (65,536 bytes)** (`sparseTileSizeInBytes`); kernel PRT VA intervals 64 KiB aligned | 5.1, 5.2, 5.3 |
| ML09 | Timestamp query counter precision and period | `timestampValidBits` in `[1, 64]` (`0` = unsupported) and finite `timestampPeriod` in `(0, 1e9)` ns/tick (`rdna4dc_metal3_timestamps_supported`) | 1.2, 2.5 |
| ML10 | Acceleration structure scratch alignment & geometry bounds | Runtime-probed `VkPhysicalDeviceAccelerationStructurePropertiesKHR` (`minAccelerationStructureScratchOffsetAlignment`, `maxGeometryCount`, `maxInstanceCount`, `maxPrimitiveCount`) | 1.2, 7.1 |
| ML11 | Vertex amplification count | `1` baseline; `> 1` only where verified and matching `supportsVertexAmplificationCount:` | 2.5 |
| ML12 | Placement heap size, alignment, and granularity | Exact `vkGetBufferMemoryRequirements` / `vkGetImageMemoryRequirements` + `bufferImageGranularity`; `currentAllocatedSize` never double-counts aliased placements | 2.2 |
| ML13 | Compute ICB buffer slots and execution ranges | Direct buffer slots (`31`) + Tier 2 indirect bindings; out-of-range `executeCommandsInBuffer:withRange:` / `resetCommandsInBuffer:withRange:` rejected cleanly | 2.3 |
| ML14 | Inherited Metal 2 limits, formats, and large-memory rules | `L01..L23`, `F01..F33`, and `M01..M08` in [metal2-matrix.md](metal2-matrix.md) remain unchanged | 8.1, 8.2, 9.1 |

## Vulkan extension and feature probe dependencies

All Metal 3 fields in `rdna4dc_metal3_caps` default to `0`/`false` prior to RADV
open (`rdna4dc_fallback()`, `rdna4dc_from_native_memory()`). When RADV opens,
`rdna4_backend_impl.inc` enables each extension only when every dependency holds
and normalizes via `rdna4dc_metal3_from_probe()`:

| Vulkan extension / core feature | Required dependencies | Probed fields (`rdna4_devcaps.h`) | Matrix rows |
| --- | --- | --- | --- |
| `VK_KHR_fragment_shader_barycentric` | Extension + `fragmentShaderBarycentric` | `barycentric` | B02, X04 |
| Core 1.0 `sampleRateShading` | `VkPhysicalDeviceFeatures::sampleRateShading` | `pull_model_interpolation` | B02, X05 |
| `VK_EXT_shader_atomic_float` | Extension + Float32 `Atomics` & `AtomicAdd` (buffer / shared / image) | `atomic_float_buffer`, `atomic_float_shared`, `atomic_float_image` | B02, X08 |
| `VK_EXT_shader_atomic_float2` | `VK_EXT_shader_atomic_float` enabled + Float32 `AtomicMinMax` | `atomic_float_minmax` | B02, X08 |
| `VK_KHR_shader_atomic_int64` (Vulkan 1.2 core) | `shaderInt64` + Vulkan 1.2 `shaderBufferInt64Atomics` / `shaderSharedInt64Atomics` (in `VkPhysicalDeviceVulkan12Features`, never duplicated in `pNext`) | `atomic_int64_buffer`, `atomic_int64_shared` | B02, X08 |
| `VK_EXT_mesh_shader` | API `>= 1.2` + extension + `meshShader` (+ `taskShader` for object/payload) + `VkPhysicalDeviceMeshShaderPropertiesEXT` | `mesh_shader`, `object_shader`, `mesh_max_payload_bytes`, `object/mesh_max_workgroup_*`, `mesh_max_output_vertices/primitives` | B02, G01..G03, ML01..ML05 |
| `VK_KHR_deferred_host_operations` + `VK_KHR_acceleration_structure` | API `>= 1.2` + `bufferDeviceAddress` + both extensions + `accelerationStructure` + `VkPhysicalDeviceAccelerationStructurePropertiesKHR` | `deferred_host_ops`, `acceleration_structure`, `accel_indirect_build`, `accel_uab`, `rt_min_scratch_alignment`, `rt_max_*` | B02, RT01, RT03, ML10 |
| `VK_KHR_ray_query` | `VK_KHR_acceleration_structure` enabled + extension + `rayQuery` | `ray_query` | B02, RT02, RT03 |
| Timestamp queries (Vulkan 1.0 / 1.3) | Query pool funcs + `vkCmdWriteTimestamp2`/`vkCmdWriteTimestamp` + `timestampValidBits` in `[1, 64]` + finite `timestampPeriod > 0` | `timestamp_supported`, `timestamp_compute_and_graphics`, `timestamp_period_ns`, `timestamp_valid_bits` | B02, X06, ML09 |
| Sparse binding / residency (Vulkan 1.0 core) | `pf.sparseBinding` + `VK_QUEUE_SPARSE_BINDING_BIT` on selected queue + `vkQueueBindSparse` (gated by kernel `RDNA4N_CAP_VM_PRT` once Task `5.2` lands; `0` while `0021` hides sparse) | `sparse_binding`, `sparse_residency_buffer`, `sparse_residency_image_2d/3d`, `sparse_residency_2/4/8/16_samples`, `sparse_residency_aliased` | B02, S02, S03, ML08 |

## Explicit exclusions (Apple Silicon and out-of-scope features)

Every entry below MUST remain `NO` / rejected on Garnet RX 9060 XT, including
after `MTLGPUFamilyMetal3` is enabled in Task `8.2`:

| Exclusion ID | Excluded feature / family | Required device answer / behavior | Rationale | Tasks |
| --- | --- | --- | --- | --- |
| EX01 | `MTLGPUFamilyApple1..10` (`1001..1010`), explicitly including **`MTLGPUFamilyApple6` (`1006`)** | `supportsFamily:` returns `NO` | Discrete AMD RDNA 4 IMR GPU; full-stack GFX12 Sparse/PRT (`S01..S03`) MUST NOT advertise `Apple6` | 1.1, 5.3, 8.2 |
| EX02 | `MTLGPUFamilyMetal4` (`5002`) and Metal 4 archives/APIs | `supportsFamily:(Metal4)` returns `NO`; `newArchiveWithURL:error:` declines cleanly | Outside `AMD 6000-series` Metal 3 parity scope | 1.1, 8.2 |
| EX03 | `MTLGPUFamilyMac1` (`2001`), `MTLGPUFamilyMacCatalyst1..2` (`4001..4002`), unknown family enums | `supportsFamily:` returns `NO` | Preserves exact `rdna4_gate.h` family contract | 1.1, 8.2 |
| EX04 | Tile shaders (`MTLTileRenderPipelineDescriptor`, `dispatchThreadsPerTile:`, tile render pipeline binary archive entries) | Refused / `NSError` (`NO`) | Apple Silicon TBDR-only | 1.1, 3.1, 8.2 |
| EX05 | Imageblocks and explicit imageblock layouts (`imageblock_data`, `imageblock_slice`, `threadgroup_imageblock`) | Unsupported / rejected | Apple Silicon TBDR-only | 1.1, 8.2 |
| EX06 | Memoryless render targets (`MTLStorageModeMemoryless`) and TBDR programmable blending (`[[color(n)]]` tile-memory fetch) | Unsupported / rejected (distinct from Mac2 `areRasterOrderGroupsSupported` via `VK_EXT_fragment_shader_interlock`, which remains supported) | Apple Silicon TBDR-only | 1.1, 8.2 |
| EX07 | `simdgroup_matrix` (`simdgroup_float8x8`, `simdgroup_half8x8`, `simdgroup_multiply_accumulate`) | Unsupported / rejected | Apple Silicon `Apple7+`-only; absent from `AMD 6000-series` | 1.1, 8.2 |
| EX08 | Apple-only pixel formats (`F33`: PVRTC/EAC/ETC/ASTC/HDR-ASTC, packed 16-bit B5G6R5/A1BGR5/ABGR4/BGR5A1, BGRA10_XR/BGR10_XR, R8/RG8 sRGB), `Depth24Stencil8`, `hasUnifiedMemory`, `isLowPower`, `isRemovable`, closed-source `MetalFX` private protocols | `isDepth24Stencil8PixelFormatSupported == NO`, `hasUnifiedMemory == NO`, `isLowPower == NO`, `isRemovable == NO`; unsupported formats/protocols rejected | Discrete AMD RDNA 4 hardware and clean-room boundary | 1.1, 8.2 |

## Current verification boundary and closure discipline

- **Certified Metal 3 state (Task `8.2`)**:
  - Pre-open `rdna4_metal3_caps()` still returns all `0`/`false` (fail-closed before RADV opens).
  - Application `MTLDevice` selectors `supportsFamily:(MTLGPUFamilyMetal3)`,
    `supportsRaytracing`, `supportsRaytracingFromRender`,
    `supportsFunctionPointers`, `supportsFunctionPointersFromRender`,
    `supportsPullModelInterpolation`, `supportsShaderBarycentricCoordinates`,
    `supportsDynamicLibraries`, `supportsRenderDynamicLibraries`, and
    `supportsBinaryArchives` now reflect verified runtime capabilities (`YES` on
    RX 9060 XT once RADV / `garnetxlate` are active);
    `supportsPrimitiveMotionBlur` remains honestly `NO` until verified.
  - `EX01..EX08` (`Apple1..10` including `Apple6`, `Mac1`, `MacCatalyst`,
    `Metal4`, TBDR/tile/imageblock/memoryless/`simdgroup_matrix`,
    `Depth24Stencil8`, `hasUnifiedMemory`, `isLowPower`, `isRemovable`) remain
    strictly `NO`.
  - WindowServer retains its `supportsFamily:(MTLGPUFamilyMetal3) == YES` and
    `supportsBinaryArchives == NO` compatibility behavior unchanged.
- **Evidence rules**: Every future closure record under
  `logs/rdna4-metal3/<run>/RESULT.md` must record row ID, build provenance and
  SHA-256, backend (`host-c`, `rust-unit`, `lavapipe-1.4`, or
  `rx9060xt-25G332`), and exact oracle outcome. Offline host C/Rust tests,
  `spirv-val`, and LavaPipe runs never close an RX 9060 XT hardware execution
  row.
