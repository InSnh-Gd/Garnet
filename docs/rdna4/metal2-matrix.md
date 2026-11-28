# Metal 2 capability and acceptance matrix

This matrix defines the `Mac2` (`MTLFeatureSet_macOS_GPUFamily2_v1`, MSL
2.0–2.1) and 8 GiB large-memory baseline for `GarnetMetal.bundle`. Metal 3
extensions (`MTLGPUFamilyMetal3`, ray tracing, mesh shaders, placement heaps,
sparse/PRT, binary archives, dynamic libraries) are tracked separately in
[metal3-matrix.md](metal3-matrix.md).

An entry is only considered verified on hardware when its execution oracle and
boundary checks pass on the target GPU; offline SPIR-V validation, host unit
tests, and VM software runs verify intermediate layers only.

## Reference target and specifications

- **OS / SDK baseline**: x86_64 macOS 26.x (`25G332` verified baseline), SDK 26.5.
- **Feature target**: `MTLFeatureSet_macOS_GPUFamily2_v1` (`10005`) /
  `MTLGPUFamilyMac2` (`2002`), with MSL 2.0 and 2.1 (language tests set
  `MTLCompileOptions.languageVersion` explicitly).
- **Reference anchors**: Apple *Metal Feature Set Tables* (Mac2 column, filtered
  to the macOS 10.14 / MSL 2.1 boundary), Apple *Metal Shading Language
  Specification* v2.1, and macOS SDK `MTLDevice.h` / `MTLLibrary.h` headers.

## Execution matrix

Every oracle uses defined inputs, checks the full relevant output, and preserves
untouched guard regions. Floating-point comparisons follow MSL accuracy rules or
explicit reference tolerances.

### 1. Compilation and MSL (`C01..C07`)

| ID | Requirement | Implementation / coverage | Oracle criteria |
| --- | --- | --- | --- |
| C01 | Source vertex, fragment, kernel; MSL 2.0 and 2.1 (spec 4.1, 6.4) | AIR compiler, `library-source`, execution suite | Compile each explicit version and render/dispatch nonconstant inputs; invalid source returns `NSError` |
| C02 | Scalar/vector/packed/matrix/aggregate layout, conversions, operators (spec 2.1–2.11, 2.14–3.2) | Translator fixtures, reflection | Layout/stride/bit-pattern round trips and matrix output vs CPU reference |
| C03 | Device, constant, threadgroup, thread pointers/references/arrays (spec 4.2) | `gpu-pointer`, `garnetxlate` pointer lowering | Runtime indexing through helpers and nested fields, aliasing, legal readonly reads; invalid permissions rejected |
| C04 | Function constants, optional arguments, stage inputs (spec 4.10) | `pipeline-reflection`, `test-cold-pipelines` | Distinct specializations produce distinct outputs in cold and warm cache; missing required constants fail |
| C05 | Uniform type, stage attributes, interpolation, vertex/fragment linkage (spec 2.13, 4.3, 4.5, 4.9) | AIR metadata, execution suite | Flat/perspective/centroid/sample inputs and built-ins match reference; invalid linkage fails |
| C06 | Common, integer, relational, math, matrix, geometric library functions (spec 5.2–5.7) | Translator corpus, `test-metal2-msl-probe.m` | Overloads checked across finite, sign, NaN/Inf, and conversion edges |
| C07 | Translation failure, bounded wait, cache identity | `garnet_shaders.m`, `test-cold-pipelines` | Refusal and deadline bounds preserved; incompatible address modes/constants/stages never share cache keys |

### 2. Compute and memory model (`K01..K07`)

| ID | Requirement | Implementation / coverage | Oracle criteria |
| --- | --- | --- | --- |
| K01 | Direct/indirect dispatch, grid/group IDs, nonuniform final group | `indirect-compute`, execution suite | Per-thread output including partial groups, nonzero indirect offsets, invalid dimensions |
| K02 | Static/dynamic threadgroup memory; threadgroup and SIMD barriers (spec 5.8.1) | AIR barrier lowering | Producer/consumer patterns across `mem_device`, `mem_threadgroup`, and `mem_texture` |
| K03 | Atomic `int`/`uint` store, load, exchange, CAS, fetch RMW (spec 2.5, 5.13) | Atomic lowering | Device and threadgroup contention, old values, CAS success/failure, relaxed ordering |
| K04 | MSL 2.0 SIMD broadcast and shuffle variants (spec 5.14) | Translator subgroup path | Lane-unique values, active-lane and subgroup boundaries in compute and fragment stages |
| K05 | MSL 2.1 SIMD vote/mask/first/helper, bitwise, min/max/product/sum, prefix ops (spec table 34) | Subgroup lowering | Active-lane results for table-34 operations, including divergent participation |
| K06 | MSL 2.1 quad broadcasts and shuffle variants (spec 5.15) | Subgroup lowering | Table-35 operations vs lane/quad reference, including fragment helper semantics |
| K07 | Global resource visibility across compute/render/blit | `command-events.md`, `display-write-order` | Write-then-consume across stage transitions with proper completion and error propagation |

Atomic float and 64-bit atomic extensions are covered in
[metal3-matrix.md](metal3-matrix.md) (`X08`), independent of 64-bit buffer
addressing (`M02`/`M03`).

### 3. Textures and formats (`T01..T07`)

| ID | Requirement | Implementation / coverage | Oracle criteria |
| --- | --- | --- | --- |
| T01 | 1D, 1D array, 2D, 2D array, 3D, cube, cube array (spec 5.10.1–5.10.7) | `garnet_texdesc.h`, `test-vkimage.c`, `texture-view` | Unique values per layer/face/depth/mip, legal views, rejected incompatible subresources |
| T02 | `sample`, `sample_compare`, `gather`, explicit gradients/LOD/bias/offsets, queries | `argument-image-operations`, `argument-gather-cube`, `argument-fragment-flow` | Per-operation reference values, mip/array selection, border handling, dimension queries |
| T03 | Filters, anisotropy, LOD clamps, border colors, comparison functions | `sampler-patterns`, `sampler-binding` | Distinguishable patterns for every state including border and depth comparisons |
| T04 | Linear/buffer textures, texture/buffer copies, mip generation, MSAA blits | `test-texture-transfer-bounds`, `test-blit-buffer-layout`, `buffer-blit` | Copied texels, padding guards, row/image pitches, nonzero origins |
| T05 | Shader texture writes / read-writes and synchronization (spec 5.10.15, RW Tier 2) | Storage-image lowering | Read-modify-write followed by dependent sampling for every reported RW format |
| T06 | All format/capability pairs in the format table below | `test-intfmt.c`, `test-depth.c`, `test-metal2-formats-render.m` | Creation, sample, write/render/blend/MSAA/resolve per advertised bit; unsupported bits reject |
| T07 | BC1–BC7 families (signed, float, sRGB) and compressed volumes | BC format mapping, `test-metal2-formats-render.m` | Decoded reference blocks across mips, layers, and volumes; compressed writes reject |

### 4. Rendering (`R01..R09`)

| ID | Requirement | Implementation / coverage | Oracle criteria |
| --- | --- | --- | --- |
| R01 | Direct/indexed/indirect primitives, base vertex/instance, instancing, stepping | `renderbox`, `indirect`, execution suite | Geometry/IDs, nonzero offsets/base instances, index types, overflow rejection |
| R02 | Depth/stencil tests, masks, front/back ops, bias/clamp, depth/stencil views | `garnet_depth.h`, `test-vkdepth.c` | Overlapping primitives match exact depth/stencil/color reference |
| R03 | Blending (including dual-source), write masks, sRGB, wide-color | Render pipeline blend mapping, `test-independent-blend.py` | Per-channel reference for every blend op/factor, dual-source, and disabled channels |
| R04 | MSAA color/depth/stencil resolve, store+resolve, programmable sample positions, deferred store | `garnet_depth.h`, `blit-subresources`, `VK_EXT_sample_locations` | Coverage/sample values and preserved/resolved attachments across supported sample counts |
| R05 | Layered rendering (including MSAA layers), multiple viewports/scissors | Image arrays, viewport/scissor state | Distinct layer/viewport patterns and clipping with zero cross-layer leakage |
| R06 | Counting occlusion and visibility query offsets | `garnet_occ.h`, `test-occ.c`, `occlusion-semantics` | Visible/occluded sample counts, accumulation order, upper-offset (`256 KiB`) rejection |
| R07 | Derivatives, sample built-ins, discard/helper behavior, stencil feedback | AIR fragment lowering | Reference derivatives/coverage/stencil with killed/helper lanes |
| R08 | Raster order groups (8; spec 4.3.1.2) | `VK_EXT_fragment_shader_interlock` (`fragmentShaderPixelInterlock`) | Ordered conflicting buffer/image accesses and independent groups match specified output |
| R09 | Texture/memory barriers and encoder dependencies | Conservative full barriers, `command-events.md` | Dependent stages observe prior writes; in-flight resources survive early release |

### 5. Resources and argument buffers (`A01..A05`)

| ID | Requirement | Implementation / coverage | Oracle criteria |
| --- | --- | --- | --- |
| A01 | Direct buffer/texture/sampler/inline binding and stage limits | `garnet_arguments.m` / `garnet_argument_encoder.m`, `arguments-layout` | Last valid slot, invalid slot rejection, offsets, inline sizes across all three stages |
| A02 | Tier 2 layout/reflection, arrays, nested arguments, pointer indexing (spec 2.12.1) | `argument-arrays`, `argument-nested`, `argument-resources` | Dynamic/nonuniform indexing picks distinct resources across vertex/fragment/compute |
| A03 | Tier 2 GPU-produced resource and sampler references | `argument-gpu`, `argument-copy`, `test-gpu-resource-tables.c` | GPU copies device argument members, synchronized consumers observe updates; illegal thread-to-device sampler copy rejected |
| A04 | Indirect resources, usage declarations, and lifetime | `argument-scan-runtime`, `summary-*`, `writer-epochs-*` | Dropping application references before submit preserves indirect reads/writes through completion and heap reuse |
| A05 | Setter validation and atomic invalid-batch refusal | `argument-encoder`, `argument-contract` | Valid writes preserved and invalid setter batches rejected atomically |

### 6. Tessellation (`P01..P04`)

| ID | Requirement | Implementation / coverage | Oracle criteria |
| --- | --- | --- | --- |
| P01 | Triangle/quad post-tessellation vertex functions, control-point and patch inputs (spec 4.1.1.1) | `garnet_tessellation.h`, synthesized VS/TCS + translated TES | Validates and executes patch/control-point/instance values |
| P02 | Packed half factors, per-patch/per-instance strides, partition modes, winding, max factor 64 | Patch pipeline adapter | Subdivision geometry across partition modes, factor limits, culling, malformed layouts |
| P03 | Direct/indexed/indirect patch draws, base instance, control-point stepping | `garnet_commands.m` patch draw paths | Compute-generated factors match exact geometry/pixel output across index and argument offsets |
| P04 | Patch draws inside CPU/GPU ICBs | `test-icb`, `test-icb-gpu` | Equivalent direct and ICB patch output, inherited/explicit state, bounded ranges |

### 7. Indirect command buffers (`I01..I04`)

| ID | Requirement | Implementation / coverage | Oracle criteria |
| --- | --- | --- | --- |
| I01 | CPU ICB render commands, descriptor constraints, ranges, reset and reuse | `garnet_icb.m`, `test-icb` | Equivalent direct draw/indexed/patch commands, empty/reset slots, range boundaries |
| I02 | MSL 2.1 GPU encoding/copying ICB commands (spec 5.16) | `metal2vulkan-rdna4` (`0068`), `garnetx_lower_icb_stage`, `test-icb-spv`, `test-icb-gpu` | GPU producer changes rendered output/parameters with `cpu_readback=0` and preserved slot order |
| I03 | Pipeline/buffer inheritance, explicit state, barriers, command resource lifetime | `test-icb-lifecycle` (`-O0`/`-O2`), `test-icb-transactions.sh` | Dependent draw order, distinct states, early release, event and heap interactions |
| I04 | Partial-submit failure, error status, and retirement | Segment quarantine and `rdna4RetainEncoded:` | Later failure never replays accepted commands and retains backing until safe retirement |

MSL 2.1 section 5.16 covers ICB **render** commands; compute ICBs are tracked
in [metal3-matrix.md](metal3-matrix.md) (`X03`).

### 8. Heaps and synchronization (`H01..H05`)

| ID | Requirement | Implementation / coverage | Oracle criteria |
| --- | --- | --- | --- |
| H01 | Automatic heaps, actual resource size/alignment, available/used size | `automatic-heaps.md`, `test-host-memory.c`, `test-heap-execution` | Real `VkDeviceMemory` backing, memory-type/dedicated constraints, capacity limits |
| H02 | Aliasable resources, backing reuse, purgeable state, retirement | Deferred physical lease retirement, `test-heap-gpu` | Repeated alias cycles within fixed heap capacity, intact in-flight contents |
| H03 | Encoder/resource fences and barriers | `command-events.md`, `test-fences`, `test-fence-execution` | Compute/render/blit writes visible at encoded dependency; resources survive retirement |
| H04 | Events and shared events at encoded positions across logical queues | Ready-head segment scheduler, `test-command-events`, `test-command-event-execution` | Same-CB signal-then-wait, two-queue handoff, CPU notifications, bounded progress |
| H05 | Failure and concurrent lifetime | Submit poison/quarantine | Zero UAF, alias reuse, or replay after unproven GPU retirement |

## Numeric limits (`L01..L23`)

Values come from Apple's `Mac2` feature table (checked by `test-metal2-limits.m`).
Device-dependent queries always reflect actual backend-probed limits.

| Limit ID | Property | Value / rule |
| --- | --- | --- |
| L01 | Vertex attributes; direct buffer arguments per function | 31 each |
| L02 | Direct texture arguments per function | 128 |
| L03 | Direct samplers including inline constant samplers | 16 |
| L04 | Threadgroup-memory argument entries | 31 |
| L05 | Inline constant buffers and length | 14 buffers, 4 KiB each |
| L06 | Threads per threadgroup | 1024 (pipelines may report a lower shader-specific max) |
| L07 | Total threadgroup memory; length alignment | 32 KiB; 16 bytes |
| L08 | Constant-address-space function allocation | 64 KiB |
| L09 | Fragment stage-in inputs / components | 32 inputs / 124 components |
| L10 | Function constants | 65536 identifiers |
| L11 | Tessellation factor | 64 |
| L12 | Viewports / scissors | 16 |
| L13 | Raster order groups | 8 |
| L14 | Buffer binding / texture-to-buffer copy alignment | 256 bytes (`minimumTextureBufferAlignmentForPixelFormat:` / `minimumLinearTextureAlignmentForPixelFormat:`) |
| L15 | 1D / 2D / cube dimensions | 16384 |
| L16 | 3D dimensions | 2048 |
| L17 | Array layers | 2048 |
| L18 | Color attachments | 8 |
| L19 | Maximum point primitive size | 511 |
| L20 | Visibility query offset | 256 KiB |
| L21 | Fence objects | 32768 |
| L22 | Single buffer length / recommended working set | Runtime device properties (8 GiB logical buffer ceiling; up to 12 GiB recommended working set) |
| L23 | RW texture tier, sample counts, Depth24 support | Runtime-probed (`Tier2`, `1/2/4/8`, `Depth24Stencil8 == NO`) |

Tier 2 indirectly referenced resource counts are governed by actual encoder
length, array capacity, and backend descriptor limits (`A02`), not capped at
31/128/16.

## Pixel-format capability matrix (`F01..F33`)

`F/W/C/B/M/R` denote filter, shader write, color target, blend, MSAA, and
resolve (`All` = all six). All listed formats support applicable shader
read/sample operations subject to type, stage, and read-write-tier rules.

| Format ID | Formats | Mac2 capability bits |
| --- | --- | --- |
| F01 | `A8Unorm` | F |
| F02 | `R8Unorm`, `R8Snorm` | All |
| F03 | `R8Uint`, `R8Sint` | W C M |
| F04 | `R16Unorm`, `R16Snorm`, `R16Float` | All |
| F05 | `R16Uint`, `R16Sint` | W C M |
| F06 | `RG8Unorm`, `RG8Snorm` | All |
| F07 | `RG8Uint`, `RG8Sint` | W C M |
| F08 | `R32Uint`, `R32Sint` | W C M |
| F09 | `R32Float` | All |
| F10 | `RG16Unorm`, `RG16Snorm`, `RG16Float` | All |
| F11 | `RG16Uint`, `RG16Sint` | W C M |
| F12 | `RGBA8Unorm`, `RGBA8Snorm`, `BGRA8Unorm` | All |
| F13 | `RGBA8Unorm_sRGB`, `BGRA8Unorm_sRGB` | F C M R B |
| F14 | `RGBA8Uint`, `RGBA8Sint` | W C M |
| F15 | `RGB10A2Unorm`, `RG11B10Float` | All |
| F16 | `RGB10A2Uint` | W C M |
| F17 | `RGB9E5Float` | F |
| F18 | `RG32Uint`, `RG32Sint` | W C M |
| F19 | `RG32Float` | All |
| F20 | `RGBA16Unorm`, `RGBA16Snorm`, `RGBA16Float` | All |
| F21 | `RGBA16Uint`, `RGBA16Sint` | W C M |
| F22 | `RGBA32Uint`, `RGBA32Sint` | W C M |
| F23 | `RGBA32Float` | All |
| F24 | `BC1_RGBA`, `BC2_RGBA`, `BC3_RGBA` (+ sRGB) | F |
| F25 | `BC4_RUnorm`, `BC4_RSnorm`, `BC5_RGUnorm`, `BC5_RGSnorm` | F |
| F26 | `BC6H_RGBFloat`, `BC6H_RGBUfloat`, `BC7_RGBAUnorm`, `BC7_RGBAUnorm_sRGB` | F |
| F27 | `GBGR422`, `BGRG422` | F |
| F28 | `Depth16Unorm`, `Depth32Float` | F M R |
| F29 | `Stencil8`, `X24_Stencil8`, `X32_Stencil8` | M (aspect rules apply) |
| F30 | `Depth24Unorm_Stencil8` | Refused (`isDepth24Stencil8PixelFormatSupported == NO`) |
| F31 | `Depth32Float_Stencil8` | F M R (aspect/view rules apply) |
| F32 | `BGR10A2Unorm` | F W C M R B |
| F33 | `R8Unorm_sRGB`, `RG8Unorm_sRGB`, packed 16-bit (`B5G6R5`/`A1BGR5`/`ABGR4`/`BGR5A1`), PVRTC/EAC/ETC/ASTC, `BGRA10_XR`/`BGR10_XR` | Not part of Mac2; rejected |

## Large-memory requirements (`M01..M08`)

These requirements apply independently of whether the system boots in 256 MiB
Small-BAR or Full-BAR mode (see [large-buffer-backing.md](large-buffer-backing.md)):

| ID | Requirement |
| --- | --- |
| M01 | Multiple Private resources totaling >= 8 GiB on target hardware; shared-budget OOM, release, and isolation |
| M02 | Single 8 GiB logical Private buffer verified at start, both sides of 4 GiB, far end, and guards |
| M03 | Single shader invocation using runtime 64-bit GEP/helper/nested-pointer accesses across the 4 GiB boundary |
| M04 | Shared/Managed continuous CPU VA above 4 GiB with page accounting and bidirectional visibility |
| M05 | Multiple NoCopy imports totaling > 4 GiB preserving deallocator ownership, readonly enforcement, and 64 MiB per-import cap |
| M06 | Segmented copy/fill/upload/readback with tails, guards, and no replay after a later chunk fails |
| M07 | Page-table/handle/map quotas, transactional rollback, and close/hang quarantine |
| M08 | Pre-open read-only discovery reporting distinct physical, pool, visible, budget, available, and recommended working-set quantities |

## Mac2 device capability queries

- **Advertised `YES` (when backend-supported)**: `supportsFamily:`
  (`MTLGPUFamilyMac2`, `Common1..3`), `supportsFeatureSet:` /
  `deviceSupportsFeatureSet:` (`macOS_GPUFamily1_v1..v4`,
  `macOS_ReadWriteTextureTier2`, `macOS_GPUFamily2_v1`), `argumentBuffersSupport`
  (`Tier2` for apps), `readWriteTextureSupport` (`Tier2`),
  `supportsBCTextureCompression` (`textureCompressionBC`),
  `supports32BitFloatFiltering`, `supports32BitMSAA`,
  `areRasterOrderGroupsSupported` (`fragmentShaderPixelInterlock`),
  `areProgrammableSamplePositionsSupported` (`VK_EXT_sample_locations`),
  `supportsTextureSampleCount:` / `supportsSampleCount:` (`1,2,4,8`),
  `supportsQueryTextureLOD`, `supportsUnalignedVertexFetch`,
  `minimumTextureBufferAlignmentForPixelFormat:` /
  `minimumLinearTextureAlignmentForPixelFormat:` (`256`), and
  `supportsVertexAmplificationCount:` (`1` baseline).
- **Metal 3 selectors** (`supportsFamily:(Metal3)`, ray tracing, function
  pointers, pull-model interpolation, barycentrics, dynamic libraries, binary
  archives, sparse/PRT, mesh shaders) are governed by the runtime-probed rules
  and explicit Apple Silicon exclusions in [metal3-matrix.md](metal3-matrix.md).
