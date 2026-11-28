# Metal 3 capability and acceptance matrix (`AMD 6000-series` parity)

This matrix extends the Metal 2 (`Mac2` / MSL 2.0 to 2.1 + 8 GiB large-memory)
baseline in [metal2-matrix.md](metal2-matrix.md) to match Apple's discrete
Intel Mac `AMD 6000-series` (`Mac2` + `Metal3`, MSL 2.2 to 3.2) feature profile on
RDNA 4 hardware.

An entry is only complete when its execution oracle and negative/boundary checks
pass on target hardware; host unit tests, `spirv-val`, and LavaPipe validate
intermediate layers only.

## Target hardware, stack, and scope

| Item | Definition |
| --- | --- |
| Physical target | AMD Radeon RX 9060 XT (Navi 44, `gfx1200`, GC 12.0.0, PCI `1002:7590`), x86_64 macOS 26.x (`25G332` verified baseline), SDK 26.5 |
| Standalone stack | `Garnet.kext` (`'GRNT'` native ABI `1.14`), `GarnetAccel.kext`, `GarnetPCIBarCompat.kext`, `GarnetMetal.bundle` over Mesa RADV (`mesa@f5cb8ee` + `mesa-patches/`) and `metal2vulkan@43c46ac8` + `third-party/metal2vulkan-rdna4/` (`garnetxlate`) |
| Feature anchor | Apple *Metal Feature Set Tables* (`AMD 6000-series` = `Mac2` + `Metal3`), MSL 2.2 to 3.2 |
| Preserved Metal 2 gates | All rows of [metal2-matrix.md](metal2-matrix.md) (`C01..C07`, `K01..K07`, `T01..T07`, `R01..R09`, `A01..A05`, `P01..P04`, `I01..I04`, `H01..H05`, `L01..L23`, `F01..F33`, `M01..M08`) remain non-regression gates |
| Clean-room boundary | Zero Apple `AMDRadeonX6000` binary reuse or substitution; zero `Lilu`, `AMDRadeonRDNA4.kext`, or `AppleParavirtGPU`; no closed-source private `MetalFX` protocols |
| Public family gate | Application and WindowServer `supportsFamily:(MTLGPUFamilyMetal3)` (`5001`) both return `YES` once backend probes pass on RX 9060 XT; `EX01..EX08` (`Apple1..10` incl. `Apple6`, `Mac1`, `MacCatalyst`, `Metal4`, TBDR/`simdgroup_matrix`) remain strictly `NO` |

## Capability matrix

Every ID is unique across `metal2-matrix.md` and `metal3-matrix.md`. Every oracle
uses defined inputs, checks the full relevant output, preserves guard regions,
and validates error paths.

### 1. Runtime capability probe (`B01..B02`)

| ID | Feature / requirement | Capability query / selector | Vulkan / stack dependency | Oracle & negative checks |
| --- | --- | --- | --- | --- |
| B01 | `AMD 6000-series` (`Mac2` + `Metal3`) capability matrix, limits, and Apple Silicon exclusions | `docs/rdna4/metal3-matrix.md`, `test-metal2-limits.m` | Apple *Metal Feature Set Tables* (`AMD 6000-series`) | Every capability query, limit, and exclusion maps to an explicit test oracle |
| B02 | Conservative pre-open fallback and runtime Vulkan Metal 3 extension/feature/limit probe | `rdna4dc_metal3_caps`, `rdna4dc_metal3_from_probe()`, `rdna4_metal3_caps()` (`garnet_devcaps.h`, `garnet_backend.m`) | `VK_KHR_fragment_shader_barycentric`, `VK_EXT_shader_atomic_float` (+ `float2`), `VK_KHR_shader_atomic_int64` (Vulkan 1.2 core), `VK_EXT_mesh_shader`, `VK_KHR_deferred_host_operations`, `VK_KHR_acceleration_structure`, `VK_KHR_ray_query`, timestamp limits, sparse features | Pre-open `rdna4dc_fallback()` / `rdna4dc_from_native_memory()` and missing-dependency probes keep all Metal 3 bits `0`/`false`; probed-on flags gate accurately and clamp oversized mesh limits (`src/garnet-metal/tests/test-host-runtime.c`) |

### 2. Metal 2.2 to 2.4 and MSL 2.2 to 3.2 core features (`X01..X08`)

| ID | Feature / requirement | Capability query / selector | Vulkan / stack dependency | Oracle & negative checks |
| --- | --- | --- | --- | --- |
| X01 | Texture Swizzle (`MTLTextureDescriptor.swizzle`, `newTextureViewWithPixelFormat:textureType:levels:slices:swizzle:`) | `Mac2` / `Metal3` baseline (macOS 10.15+) | `VkComponentMapping` composition in `rdna4_view_map` over base format swizzles (e.g. `A8Unorm`) | Custom channel remaps (`R/G/B/A`, `Zero`, `One`) match reference without modifying base/unswizzled views; invalid swizzle enum or non-identity swizzle on storage-write/render-target views returns `nil` (`test-metal3-swizzle.m`, `test-swizzle-placement.c`) |
| X02 | Placement Heaps (`MTLHeapTypePlacement`, `newBufferWithLength:options:offset:`, `newTextureWithDescriptor:offset:`, `heapOffset`) | `MTLHeapDescriptor.type = MTLHeapTypePlacement`; `heapBufferSizeAndAlignWithLength:options:`; `heapTextureSizeAndAlignWithDescriptor:` | `VkDeviceMemory` explicit-offset bind + `vkGetBufferMemoryRequirements` / `vkGetImageMemoryRequirements` + `bufferImageGranularity` | Returned `heapOffset` matches request, non-overlapping regions stay isolated, post-sync explicit-offset aliasing is visible to subsequent reads, accounting queries match; misaligned/out-of-bounds/over-capacity placement returns `nil` (`test-metal3-placement-heap.m`, `test-swizzle-placement.c`) |
| X03 | Compute Indirect Command Buffers (`MTLIndirectCommandTypeConcurrentDispatch`, `ConcurrentDispatchThreads`, CPU & GPU encoding) | `MTLIndirectCommandBufferDescriptor.commandTypes` | `vkCmdDispatchIndirect` + dynamic binding expansion + `garnetx_lower_icb_stage` | CPU and GPU encoded multi-pipeline compute ICB subranges match direct dispatch, unset/reset slots no-op, GPU path has `cpu_readback=0`, early-release and partial-submit failure preserve safe retirement (`test-metal3-compute-icb.m`) |
| X04 | Shader Barycentric Coordinates (`[[barycentric_coord]]`, perspective & `noperspective`) | `supportsShaderBarycentricCoordinates` (`rdna4dc_metal3_barycentric_ok`) | `VK_KHR_fragment_shader_barycentric` -> SPIR-V `FragmentBarycentricKHR` (`BaryCoordKHR` / `BaryCoordNoPerspKHR`) | Fragment weights sum to `1.0` and reconstruct triangle vertex attributes accurately; unsupported backend reports `NO` (`test-metal3-bary-pull.m`) |
| X05 | Pull-Model Interpolation (`interpolant<T>::interpolate_at_center`, `centroid`, `sample`, `offset`) | `supportsPullModelInterpolation` (`rdna4dc_metal3_pull_model_ok`) | Vulkan 1.0 core `sampleRateShading` -> SPIR-V `SampleRateShading` + `GLSL.std.450` `InterpolateAt*` | MSAA per-sample and explicit subpixel-offset interpolants match analytic reference; unsupported backend reports `NO` (`test-metal3-bary-pull.m`) |
| X06 | GPU Timestamp Counter Sampling (`MTLCounterSampleBuffer`, `MTLCommonCounterSetTimestamp`) | `counterSets`, `supportsCounterSampling:` (`AtStageBoundary`, `AtDrawBoundary`, `AtDispatchBoundary`, `AtBlitBoundary`), `sampleTimestamps:gpuTimestamp:`, `resolveCounterRange:` (`rdna4dc_metal3_timestamps_supported`) | `VkQueryPool(VK_QUERY_TYPE_TIMESTAMP)`, `vkCmdWriteTimestamp2` / `vkCmdWriteTimestamp`, `timestampPeriod`, `timestampValidBits` | Real compute/render/blit work resolves valid monotonic nanosecond timestamps (`end >= start`, non-zero); out-of-range index, unsupported sampling point, or invalid descriptor returns `NSError` (`test-metal3-counters-amp.m`, `test-counters-amp.c`) |
| X07 | Vertex Amplification (`setVertexAmplificationCount:viewMappings:`, `[[amplification_id]]`, `[[amplification_count]]`) | `supportsVertexAmplificationCount:` (`1` baseline; `> 1` only when target-verified) | `multiViewport` / layered view mapping / Vulkan 1.1 multiview | Amplified draw writes distinct viewport/layer outputs matching CPU reference; unsupported counts or invalid view mappings fail (`test-metal3-counters-amp.m`) |
| X08 | MSL 2.2 to 3.2 float/64-bit/texture atomics, `bfloat`, and SIMD shift/fill (`atomic<float>`, `atomic<int64_t>`/`uint64_t`, `bfloat`, texture atomics, `simd_shuffle_and_fill_*`) | `rdna4dc_metal3_atomic_float_ok`, `atomic_float_minmax`, `atomic_float_image`, `rdna4dc_metal3_atomic_int64_ok` | `VK_EXT_shader_atomic_float`, `VK_EXT_shader_atomic_float2`, Vulkan 1.2 `shaderBufferInt64Atomics`/`shaderSharedInt64Atomics` + `shaderInt64`, core storage-image atomics, subgroup shuffles | Contended buffer/threadgroup/texture atomic sums/min/max/CAS, `bfloat` conversions/math, and SIMD fill results match race-free references (`test-metal3-msl3-atomics.m`) |

### 3. Pipeline modularity and dynamic function dispatch (`D01..D03`)

| ID | Feature / requirement | Capability query / selector | Vulkan / stack dependency | Oracle & negative checks |
| --- | --- | --- | --- | --- |
| D01 | `MTLBinaryArchive` (`newBinaryArchiveWithDescriptor:error:`, `addComputePipelineFunctionsWithDescriptor:error:`, `addRenderPipelineFunctionsWithDescriptor:error:`, `serializeToURL:error:`, pipeline `binaryArchives`) | `supportsBinaryArchives` (`YES` for admitted apps, `NO` for WindowServer) | Versioned Garnet container (magic, ABI/RADV `pipelineCacheUUID`, SHA-256, normalized descriptor key -> SPIR-V + reflection JSON + `VkPipelineCache`) | Serialize + reload in fresh instance hits archive and produces bit-identical GPU output; tampered header/hash, UUID mismatch, foreign `.metallib`, or unsupported tile pipeline returns `MTLBinaryArchiveError` (`test-metal3-binary-archive.m`, `test-host-runtime.c`) |
| D02 | `MTLDynamicLibrary` (`newDynamicLibrary:error:`, `newDynamicLibraryWithURL:error:`, `serializeToURL:error:`, `preloadedLibraries`, `binaryFunctions`) | `supportsDynamicLibraries` (compute), `supportsRenderDynamicLibraries` (vertex/fragment) | Garnet dynamic library container (sanitized AIR + export table) + `garnetx_translate_linked` symbol linking in `garnet_shaders.m` / `garnet_dynlib_vft.m` | Compute, vertex, and fragment pipelines invoke exported dynamic-library functions with exact GPU output; missing symbol or invalid file returns `NSError` (`test-metal3-dynamic-lib.m`) |
| D03 | `MTLVisibleFunctionTable` and function pointers (`newVisibleFunctionTableWithDescriptor:`, `functionHandleWithFunction:`, `setFunction:atIndex:`, `setFunctions:withRange:`, `visible_function_table<...>`) | `supportsFunctionPointers` (compute), `supportsFunctionPointersFromRender` (render) | Compile-time `MTLLinkedFunctions` closure + bounded `OpSwitch` dispatch lowering + device slot-ID buffer; Footnote 10 mesh-pipeline exclusion | Runtime table index invokes distinct linked functions (including nested resource access); out-of-bounds/empty slots are isolated; Mesh pipeline requesting render function pointers fails with `NSError` (`test-metal3-function-tables.m`) |

### 4. Fast resource loading and residency sets (`Q01..Q02`)

| ID | Feature / requirement | Capability query / selector | Vulkan / stack dependency | Oracle & negative checks |
| --- | --- | --- | --- | --- |
| Q01 | Fast Resource Loading (`MTLIOFileHandle`, `MTLIOCommandQueue`, `MTLIOCommandBuffer`: `loadBuffer:`, `loadTexture:`, `addBarrier`, `signalEvent:value:`, `waitForEvent:value:`, `tryCancel`) | `Mac2` + `Metal3` baseline (macOS 13+ API; limit `RDNA4DC_IO_MAX_COMMANDS = 8192`) | Bounded async `pread` worker + recyclable Shared staging ring + `vkCmdCopyBuffer`/`vkCmdCopyBufferToImage` + serial scheduler + `MTLSharedEvent` | Async load into Private buffer and mipmapped texture + event handoff reads back bit-exact bytes with untouched guards and early handle release; missing file, EOF overrun, `> 8192` commands, unsupported codec, or `tryCancel` yields `MTLIOError` (`test-metal3-io-queue.m`, `test-io-residency.c`) |
| Q02 | Residency Sets (`MTLResidencySet`: `newResidencySetWithDescriptor:error:`, `addAllocation:`, `removeAllocation:`, `commit`, `MTLCommandQueue.addResidencySet:`, `MTLCommandBuffer.useResidencySet:`) | Metal 3.2 (macOS 15+ API) | Immutable committed allocation snapshot merged into command-buffer retirement retention | Indirect BDA / Tier 2 / RT resources survive early ARC release (`-O0`/`-O2`) until GPU retirement and reclaim without VRAM leaks after `removeAllocation:` + `commit` (`test-metal3-residency-set.m`, `test-io-residency.c`) |

### 5. Full-stack GFX12 Sparse Resources / PRT (`S01..S03`, independent of `Apple6`)

In Apple's *Metal Feature Set Tables*, Metal sparse textures belong to
`Apple6+`, not `AMD 6000-series` (`Mac2` + `Metal3`). Garnet implements
full-stack GFX12 PRT and sparse resources as a hardware capability while
**keeping `MTLGPUFamilyApple6 == NO`** and all Apple Silicon TBDR queries `NO`.

| ID | Feature / requirement | Capability query / selector | Vulkan / stack dependency | Oracle & negative checks |
| --- | --- | --- | --- | --- |
| S01 | Kernel GFX12 PRT VM & atomic `CLEAR`/`REPLACE` (`Garnet.kext`) | `'GRNT'` ABI `1.13`, capability bit `RDNA4N_CAP_VM_PRT` | `RDNA4N_VA_OP_CLEAR`, `RDNA4N_VA_OP_REPLACE`, `RDNA4N_VM_PAGE_PRT` (`handle == 0`), GFX12 PTE `AMDGPU_PTE_PRT_GFX12 (1ULL << 56) \| SNOOPED \| SYSTEM \| IS_PTE` with `AMDGPU_PTE_VALID` cleared; interval splitting, mandatory TLB flush, PT quota & rollback | `native_prt_sparse_test.cpp` (ASan+UBSan) covers PTE encoding, splitting, TLB invalidation, quota exhaustion, invalid-arg rejection, and session cleanup; physical non-resident read-zero / write-discard with `DEAD/HUNG/gcvm = 0` |
| S02 | Mesa RADV Darwin Vulkan sparse binding & residency | Vulkan `sparseBinding`, `sparseResidencyBuffer`, `sparseResidencyImage2D`, `sparseResidencyImage3D`, `sparseResidency2/4/8/16Samples`, `sparseResidencyAliased`, `VK_QUEUE_SPARSE_BINDING_BIT` | `mesa-patches/0028` negotiating `RDNA4N_CAP_VM_PRT` (disabled on ABI `<= 1.12` kernels), `vkQueueBindSparse` | Physical `vulkaninfo` has zero `-22` / `ERROR_OUT_OF_DEVICE_MEMORY`; `mesa-patches/tests/vulkan_sparse_prt_test.c` passes resident read/write + unbind read-zero/write-discard |
| S03 | Metal sparse textures & shader residency predicates | `sparseTileSizeWithTextureType:pixelFormat:sampleCount:`, `sparseTileSizeInBytes` (`65536`), `convertSparsePixelRegions:toTileRegions:withTileSize:alignmentMode:numRegions:`, `MTLResourceStateCommandEncoder` (`rdna4dc_metal3_sparse_texture_ok`; **`MTLGPUFamilyApple6` stays `NO`**) | `vkQueueBindSparse` via command scheduler + `metal2vulkan-rdna4` `OpImageSparseSample*` / `OpImageSparseFetch` + `OpImageSparseTexelsResident` (`sparse_color<T>::resident()`) | Mapped 64 KiB tiles return `resident() == true` + exact texels, unmapped tiles return `resident() == false` + zero without GPU faults, and `supportsFamily:(MTLGPUFamilyApple6) == NO` (`test-metal3-sparse-prt.m`, `test-sparse-metal.c`) |

### 6. Mesh and Object Shaders (`G01..G03`)

| ID | Feature / requirement | Capability query / selector | Vulkan / stack dependency | Oracle & negative checks |
| --- | --- | --- | --- | --- |
| G01 | AIR Object & Mesh shader translation | `rdna4dc_metal3_mesh_supported` (`mesh_shader && object_shader`, payload `>= 16384`, mesh grid `>= 1024`) | `VK_EXT_mesh_shader` (`taskShader`, `meshShader`), SPIR-V 1.4 (`TaskEXT`, `TaskPayloadWorkgroupEXT`, `OpEmitMeshTasksEXT`, `MeshEXT`, `OpSetMeshOutputsEXT`, `PerPrimitiveEXT`), `garnetxlate` reflection JSON | Rust unit tests and `spirv-val` cover 16 KiB payload, point/line/triangle topologies, and per-vertex/per-primitive outputs |
| G02 | `MTLMeshRenderPipelineDescriptor` creation, limits, and Footnote 10 mutual exclusion | `newRenderPipelineStateWithMeshDescriptor:options:reflection:error:` (+ async overloads) | `VK_EXT_mesh_shader` graphics pipeline + stage linkage + Apple Footnote 10 validation (Mesh pipeline vs render function pointers / render ray tracing) | Mesh-only and object+mesh+fragment pipelines match CPU reference geometry/per-primitive attributes/culling; `> 16384` B payload, illegal topology/linkage, or Footnote 10 combination returns `NSError` (`test-metal3-mesh-shaders.m`, `test-mesh-runtime.c`) |
| G03 | Direct and indirect mesh draws (`setObject*`, `setMesh*`, `drawMeshThreadgroups:`, `drawMeshThreads:`, `drawMeshThreadgroupsWithIndirectBuffer:`) | `MTLRenderCommandEncoder` mesh/object binding and draw selectors | `vkCmdDrawMeshTasksEXT`, `vkCmdDrawMeshTasksIndirectEXT` | Compute producer writes `(gridX, gridY, gridZ)` to indirect buffer (including 0-grid no-op, non-zero offsets, multi-draw ordering, variable object-emitted mesh grid `>= 1024`) matching CPU reference; out-of-bounds indirect buffer or illegal workgroup dimensions return controlled error (`test-metal3-mesh-shaders.m`) |

### 7. Hardware Ray Tracing (`RT01..RT04`)

| ID | Feature / requirement | Capability query / selector | Vulkan / stack dependency | Oracle & negative checks |
| --- | --- | --- | --- | --- |
| RT01 | `MTLAccelerationStructure` & `MTLAccelerationStructureCommandEncoder` (triangle BLAS, bounding-box BLAS, instance TLAS, build, refit, writeCompactedSize, copyAndCompact, copy, `MTLHeap` AS, async compute AS builds) | `supportsRaytracing` (`rdna4dc_metal3_raytracing_supported`); `accelerationStructureSizesWithDescriptor:` | `VK_KHR_deferred_host_operations`, `VK_KHR_acceleration_structure` (`accelerationStructure`, `bufferDeviceAddress`, `minAccelerationStructureScratchOffsetAlignment`), Private `VkBuffer` + BDA + retirement list | Size queries match Vulkan requirements; build -> compact -> refit -> copy succeed; undersized scratch/AS buffer or invalid descriptor returns controlled error with zero UAF/hang (`test-metal3-rt-accel.m`, `test-rt-accel.c`) |
| RT02 | Inline Ray Query lowering & compute ray tracing (`raytracing::intersector`, `intersection_query`, `@air.intersect.*`, `primitiveDataBuffer`) | `supportsRaytracing` | `VK_KHR_ray_query` (`rayQuery`) -> SPIR-V `RayQueryKHR` (`OpRayQueryInitializeKHR` / `ProceedKHR` / `ConfirmIntersectionKHR` / `GenerateIntersectionKHR`), `metal2vulkan-rdna4` | Triangle hit/miss, distance, primitive ID, instance ID/transform, visibility mask, hit barycentrics, `primitiveDataBuffer`, and post-refit hits match analytic CPU oracle (`test-metal3-raytracing.m`, `test-rt-runtime.c`) |
| RT03 | Render-stage ray tracing & `MTLIntersectionFunctionTable` (vertex/fragment TLAS binding incl. Tier 2 argument buffers; custom bounding-box intersection & triangle any-hit functions) | `supportsRaytracingFromRender` (subject to Footnote 10 mutual exclusion with Mesh pipelines); `newIntersectionFunctionTableWithDescriptor:` | `VK_KHR_ray_query` in vertex/fragment + `descriptorBindingAccelerationStructureUpdateAfterBind` + bounded switch-table intersection function dispatch (IFT ABI v2) | Fragment shadow/reflection rays and custom sphere bounding-box intersection distance/normal + any-hit filtering match analytic reference; early ARC release (`-O0`/`-O2`) has zero UAF (`test-metal3-raytracing.m`) |
| RT04 | Curve geometry (`MTLAccelerationStructureCurveGeometryDescriptor`) and primitive/instance motion blur (`MTLAccelerationStructureMotion*Descriptor`, `intersector::intersect(..., time)`) | Opt-in gates `GARNET_RT_CURVES=1` and `GARNET_RT_MOTION=1` (with `GARNET_ALLOW=1`); `supportsPrimitiveMotionBlur` defaults to `NO` until full release certification | Compute AABB/union-AABB BLAS & motion TLAS builders plus `metal2vulkan-rdna4` patches `0088`, `0089`, and `0090` | Opt-in RX 9060 XT exact-oracle suites pass (`test-metal3-rt-curves.m` 10,000-ray oracle, `test-metal3-rt-motion.m`, `test-rt-curves.c`, `test-rt-motion.c`); default path keeps both off and returns `NO` |

## Metal 3 numeric limits and rules (`ML01..ML14`)

These limits extend `L01..L23` and `F01..F33` in
[metal2-matrix.md](metal2-matrix.md) per Apple's *Metal Feature Set Tables*
(`AMD 6000-series` = `Mac2` + `Metal3`). Device queries never inflate probed
backend limits.

| Limit ID | Property / rule | Fixed value / contract |
| --- | --- | --- |
| ML01 | Max object-to-mesh threadgroup payload memory length | **16,384 bytes (16 KiB)** (`RDNA4DC_MESH_MAX_PAYLOAD_BYTES`; `maxTaskPayloadSize` clamped to 16,384; Metal 3 requires `>= 16,384`) |
| ML02 | Max mesh threadgroups per object threadgroup (mesh grid) | **`>= 1,024` threadgroups** (`RDNA4DC_MESH_MIN_GRID`; `maxMeshWorkGroupTotalCount >= 1024` and `maxMeshWorkGroupCount[0] >= 1024`) |
| ML03 | Max threads per object / mesh threadgroup | **1,024** (`RDNA4DC_MESH_MAX_INVOCATIONS`; clamped to `<= 1024`, matching `L06`) |
| ML04 | Max total threadgroup memory for object / mesh stage | **32,768 bytes (32 KiB)**, 16-byte aligned (matching `L07`) |
| ML05 | Max output vertices / primitives per mesh threadgroup | **256 vertices / 512 primitives** (`RDNA4DC_MESH_MAX_VERTICES` / `RDNA4DC_MESH_MAX_PRIMITIVES`, preserving lower backend actuals if `< 256`/`< 512`) |
| ML06 | Max I/O commands per `MTLIOCommandBuffer` | **8,192** (`RDNA4DC_IO_MAX_COMMANDS`; `> 8192` fails with `MTLIOError`) |
| ML07 | Footnote 10 mutual exclusion (`AMD 6000-series` / `Metal3`) | A render pipeline using Object/Mesh shaders **cannot** combine render-stage function pointers (`MTLVisibleFunctionTable` / render `MTLLinkedFunctions`) or render-stage ray tracing; pipeline creation returns a deterministic `NSError` |
| ML08 | Sparse texture tile size (GFX12 PRT, independent of `Apple6`) | **64 KiB (65,536 bytes)** (`sparseTileSizeInBytes`); kernel PRT VA intervals 64 KiB aligned |
| ML09 | Timestamp query counter precision and period | `timestampValidBits` in `[1, 64]` (`0` = unsupported) and finite `timestampPeriod` in `(0, 1e9)` ns/tick (`rdna4dc_metal3_timestamps_supported`) |
| ML10 | Acceleration structure scratch alignment & geometry bounds | Runtime-probed `VkPhysicalDeviceAccelerationStructurePropertiesKHR` (`minAccelerationStructureScratchOffsetAlignment`, `maxGeometryCount`, `maxInstanceCount`, `maxPrimitiveCount`) |
| ML11 | Vertex amplification count | `1` baseline; `> 1` only where verified and matching `supportsVertexAmplificationCount:` |
| ML12 | Placement heap size, alignment, and granularity | Exact `vkGetBufferMemoryRequirements` / `vkGetImageMemoryRequirements` + `bufferImageGranularity`; `currentAllocatedSize` never double-counts aliased placements |
| ML13 | Compute ICB buffer slots and execution ranges | Direct buffer slots (`31`) + Tier 2 indirect bindings; out-of-range `executeCommandsInBuffer:withRange:` / `resetCommandsInBuffer:withRange:` rejected cleanly |
| ML14 | Inherited Metal 2 limits, formats, and large-memory rules | `L01..L23`, `F01..F33`, and `M01..M08` in [metal2-matrix.md](metal2-matrix.md) remain unchanged |

## Vulkan extension and feature probe dependencies

All Metal 3 fields in `rdna4dc_metal3_caps` default to `0`/`false` before RADV
opens (`rdna4dc_fallback()`, `rdna4dc_from_native_memory()`). When RADV opens,
`garnet_backend.m` enables each extension only when all dependencies hold and
normalizes via `rdna4dc_metal3_from_probe()`:

| Vulkan extension / core feature | Required dependencies | Probed fields (`garnet_devcaps.h`) | Matrix rows |
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
| Sparse binding / residency (Vulkan 1.0 core) | `pf.sparseBinding` + `VK_QUEUE_SPARSE_BINDING_BIT` + `vkQueueBindSparse` (gated by kernel `RDNA4N_CAP_VM_PRT`) | `sparse_binding`, `sparse_residency_buffer`, `sparse_residency_image_2d/3d`, `sparse_residency_2/4/8/16_samples`, `sparse_residency_aliased` | B02, S02, S03, ML08 |

## Explicit exclusions (Apple Silicon and out-of-scope features)

Every entry below remains `NO` / rejected on Garnet RX 9060 XT:

| Exclusion ID | Excluded feature / family | Required device behavior | Rationale |
| --- | --- | --- | --- |
| EX01 | `MTLGPUFamilyApple1..10` (`1001..1010`), explicitly including **`MTLGPUFamilyApple6` (`1006`)** | `supportsFamily:` returns `NO` | Discrete AMD RDNA 4 IMR GPU; GFX12 Sparse/PRT (`S01..S03`) does not advertise `Apple6` |
| EX02 | `MTLGPUFamilyMetal4` (`5002`) and Metal 4 archives/APIs | `supportsFamily:(Metal4)` returns `NO`; `newArchiveWithURL:error:` declines cleanly | Outside Metal 3 parity scope (Metal 4 foundation is planned separately) |
| EX03 | `MTLGPUFamilyMac1` (`2001`), `MTLGPUFamilyMacCatalyst1..2` (`4001..4002`), unknown family enums | `supportsFamily:` returns `NO` | Preserves `garnet_gate.h` family contract |
| EX04 | Tile shaders (`MTLTileRenderPipelineDescriptor`, `dispatchThreadsPerTile:`, tile render pipeline binary archive entries) | Refused / `NSError` | Apple Silicon TBDR-only |
| EX05 | Imageblocks and explicit imageblock layouts (`imageblock_data`, `imageblock_slice`, `threadgroup_imageblock`) | Unsupported / rejected | Apple Silicon TBDR-only |
| EX06 | Memoryless render targets (`MTLStorageModeMemoryless`) and TBDR programmable blending (`[[color(n)]]` tile-memory fetch) | Unsupported / rejected (distinct from Mac2 `areRasterOrderGroupsSupported` via `VK_EXT_fragment_shader_interlock`) | Apple Silicon TBDR-only |
| EX07 | `simdgroup_matrix` (`simdgroup_float8x8`, `simdgroup_half8x8`, `simdgroup_multiply_accumulate`) | Unsupported / rejected | Apple Silicon `Apple7+`-only |
| EX08 | Apple-only pixel formats (`F33`), `Depth24Stencil8`, `hasUnifiedMemory`, `isLowPower`, `isRemovable`, closed-source `MetalFX` private protocols | `isDepth24Stencil8PixelFormatSupported == NO`, `hasUnifiedMemory == NO`, `isLowPower == NO`, `isRemovable == NO`; unsupported formats/protocols rejected | Discrete AMD RDNA 4 hardware and clean-room boundary |

## Runtime capability summary

- Pre-open `rdna4_metal3_caps()` returns all `0`/`false` (fail-closed before RADV opens).
- Once RADV and `garnetxlate` are active on RX 9060 XT, application `MTLDevice`
  selectors `supportsFamily:(MTLGPUFamilyMetal3)`, `supportsRaytracing`,
  `supportsRaytracingFromRender`, `supportsFunctionPointers`,
  `supportsFunctionPointersFromRender`, `supportsPullModelInterpolation`,
  `supportsShaderBarycentricCoordinates`, `supportsDynamicLibraries`,
  `supportsRenderDynamicLibraries`, and `supportsBinaryArchives` report `YES`;
  `supportsPrimitiveMotionBlur` remains `NO` by default (opt-in curve and motion
  blur paths require `GARNET_ALLOW=1` with `GARNET_RT_CURVES=1` /
  `GARNET_RT_MOTION=1`).
- `EX01..EX08` remain strictly `NO`.
- WindowServer keeps `supportsFamily:(MTLGPUFamilyMetal3) == YES` and
  `supportsBinaryArchives == NO`.
