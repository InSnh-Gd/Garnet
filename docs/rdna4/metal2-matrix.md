# Fixed Metal 2 acceptance matrix

This matrix defines `complete-rdna4-metal2-large-memory` task 1.1. An entry is
complete only with its execution oracle and supported-boundary checks. Existing
selectors, reflected metadata, encoded commands and SPIR-V validation establish
individual layers; they do not establish GPU execution. No new feature is
marked complete by this document.

## Pinned sources and interpretation

The execution target is macOS 26.7.2, build 25G332, using SDK 26.5. The feature
target is `MTLFeatureSet_macOS_GPUFamily2_v1` (10005, macOS 10.14), with MSL 2.0
and 2.1. `MTLGPUFamilyMac2` (2002) is the corresponding family query, introduced
with `supportsFamily:` in macOS 10.15. Its current name does not import every
later feature into this target. Set `MTLCompileOptions.languageVersion`
explicitly in language tests; the SDK's current default is not the baseline.

Source files are preserved under
`logs/rdna4-metal2/20261105-110230/apple-sources/`:

| Source | Fixed identity and use |
| --- | --- |
| Apple Metal Feature Set Tables | PDF created 2019-09-10, 11 pages, Mac2 column; SHA256 `4964d1a5f5392809ebbe0d2ce450657f7672146d9f4cfdd4e41af6a8c1c7c7a6` |
| Apple Metal Shading Language Specification | Version 2.1, 2018-11-09, 184 pages; SHA256 `419dbbfcae1dc213b70d2da097db1beee036e191a6c05ed83231a0804efccd66` |
| SDK MTLDevice.h | SDK 26.5; SHA256 `d1ef1bb5bc85faeeec9bc85f49124a453a8cd33b1182b0a71a41ba0dc4ebbb89`; enum values, availability and device queries |
| SDK MTLLibrary.h | SHA256 `be5d8b86b781cf6a52f527b4d8b05ca037189082c0f45bdf2a23db92fae935d8`; MSL 2.0 = macOS 10.13, 2.1 = 10.14, 2.2 = 10.15 |
| SDK pixel format, heap, ICB, render encoder and render pass headers | Exact files and hashes in `sdk-headers.jsonl` and `sdk-feature-headers.jsonl`; distinguish availability from implementation |

The PDFs are archived copies of Apple's original URLs. Exact archive URLs and
hashes are in `sources.json`. The feature table's footer says 2018-08-01 even
though its PDF metadata and contents are from 2019; the hash identifies the
actual document. Filter its Mac2 entries through the 10.14/MSL 2.1 boundary:
placement heaps and texture swizzles, for example, are later extensions.
The MSL spec's section 6.4 accidentally calls `osx-metal2.1` “revision 2.0”; its
cover, version convention, macOS 10.14 target and SDK availability distinguish
2.1. This typo does not change the language target.

The current compiler deployment floor (12.0) and bundle OS requirement (26.6)
are separate from the feature target. This work runs on the fixed 25G332 target;
it does not claim that this private driver works on macOS 10.14 or every OS
above the compiler floor. Build provenance must record the actual SDK and OS.

## Required execution entries

IDs name the individual acceptance assertions, not a single group-level PASS.
Test names without an existing source are new cases to add under the specified
task. Every oracle must use defined inputs, check the full relevant result and
retain untouched guards. Floating-point comparisons use the pinned MSL accuracy
rules or a recorded reference tolerance, not relaxed tolerances chosen after a
failure. Every row remains open until evidence is linked during implementation.

### 1. Compilation and MSL

| ID | Requirement and fixed source | Existing implementation / case | Required oracle | Tasks |
| --- | --- | --- | --- | --- |
| C01 | Source vertex, fragment, kernel; MSL 2.0 and 2.1; spec 4.1, 6.4 | Inherited AIR compiler; library-source, execution | Compile each explicit version, then render/dispatch nonconstant inputs; invalid source returns an NSError | 8.2 |
| C02 | Scalar/vector/packed/matrix/aggregate layout, conversions, operators; spec 2.1..2.11, 2.14..3.2 | Translator fixtures, reflection, B1 | Layout/stride/bit-pattern round trips, matrix output against CPU, per-overload generated cases | 3.4, 8.2 |
| C03 | Device, constant, threadgroup, thread pointers/references/arrays; spec 4.2 | gpu-pointer, wrapper gpu_pointer.rs | Runtime indexing through helper and nested fields, correct aliases and legal readonly reads; invalid permissions rejected | 3.4, 3.5, 8.2 |
| C04 | Function constants, optional arguments and stage inputs; spec 4.10 | pipeline-reflection, cold-pipelines | Two distinct specializations produce distinct correct outputs in cold and warm cache; missing required constants reject | 8.2 |
| C05 | Uniform type, stage attributes, interpolation and vertex/fragment linkage; spec 2.13, 4.3, 4.5, 4.9 | AIR metadata, execution | Flat/perspective/centroid/sample inputs and built-in IDs compared to reference; invalid linkage rejects | 6.1, 8.1, 8.2 |
| C06 | Common, integer, relational, math, matrix and geometric library functions; spec tables in 5.2..5.7 | Translator corpus, execution | Enumerate the applicable overloads in these fixed tables; finite, sign, NaN/Inf and conversion edges against the specified oracle | 8.2 |
| C07 | Translation failure, finite wait, cache identity | xlate corpus, cold-translator, cold-pipelines | Original rejection/time bounds remain; address modes/constants/stages cannot reuse incompatible cached code; placeholders never count as execution | 3.4, 8.2, 9.1 |

### 2. Compute and memory model

| ID | Requirement and fixed source | Existing implementation / case | Required oracle | Tasks |
| --- | --- | --- | --- | --- |
| K01 | Direct/indirect dispatch, grid and group IDs, nonuniform final group | indirect-compute, execution, devcaps | Per-thread output including partial groups, nonzero indirect offsets and invalid dimensions | 8.2, 8.3 |
| K02 | Static/dynamic threadgroup memory; threadgroup and SIMD barriers; spec 5.8.1 | AIR barrier lowering | Producer/consumer patterns with `mem_device`, `mem_threadgroup`, `mem_texture` as applicable; check each memory domain | 4.2, 8.2 |
| K03 | Atomic int/uint store, load, exchange, compare-exchange, fetch add/sub/min/max/and/or/xor; spec 2.5, 5.13 | Existing atomic lowering | Device and threadgroup contention, returned old values, success/failure paths, relaxed ordering as defined by this spec | 8.2 |
| K04 | MSL 2.0 SIMD broadcast and shuffle variants; spec 5.14 | Translator subgroup path | Lane-unique values, active-lane and subgroup boundary results in compute and permitted fragment cases | 8.2 |
| K05 | MSL 2.1 SIMD vote/mask/first/helper, and/or/xor, min/max/product/sum and inclusive/exclusive prefix operations; spec table 34 | Existing partial subgroup path | Every table-34 operation/overload has a known active-lane result, including divergent participation where defined | 8.2 |
| K06 | MSL 2.1 quad broadcasts and shuffle variants; spec 5.15 | Translator subgroup path | All table-35 operations against lane/quad reference, including fragment helper semantics | 8.2 |
| K07 | Global resource visibility between compute/render/blit | command-completion, display-write-order | Write then consume through each stage transition with correct completion and error propagation | 4.1, 4.2 |

Atomic float, atomic 64-bit extensions and stronger atomic orders absent from
the pinned spec are not implied by K03. Large 64-bit *addresses* remain required
by the separate memory requirements; they are not a claim of 64-bit atomics.

### 3. Textures and formats

| ID | Requirement and fixed source | Existing implementation / case | Required oracle | Tasks |
| --- | --- | --- | --- | --- |
| T01 | 1D, 1D array, 2D, 2D array, 3D, cube and cube array; spec 5.10.1..5.10.7 | texdesc, vkimage, texture-view, execution | Unique values at each layer/face/depth/mip, legal views and rejected incompatible subresources | 8.1 |
| T02 | Sample, sample_compare, gather, explicit gradients/LOD/bias/offsets and queries for permitted texture types | Translator image paths, sampler test | Per-operation reference values, mip and array selection, defined borders and dimension queries | 8.1, 8.2 |
| T03 | Filters, anisotropy, LOD clamps, border colors, comparison functions | sampler mapping, argbuf-sampler | Known patterns distinguish each state, including border and depth comparisons | 8.1 |
| T04 | Linear/buffer textures, texture/buffer copies, mip generation and MSAA blits | pitch, buffer-blit, blit-subresources | All copied texels and padding guards, legal row/image pitches and nonzero origins | 3.6, 8.1 |
| T05 | Shader texture writes/read-writes and synchronization; spec 5.10.15, device RW tier | Existing storage-image lowering | Read-modify-write then dependent sampling for every reported RW format and stage | 4.2, 8.1 |
| T06 | All applicable format/capability pairs in the format table below | intfmt, depth, texture-view, execution | Creation, upload/sample, write/render/blend/MSAA/resolve per advertised bit; unsupported bits reject | 8.1, 8.3 |
| T07 | BC1..BC7 families including signed, float and sRGB variants; compressed volumes | Format mapping currently incomplete | Fixed decoded reference blocks across mips, layers and supported volume paths; compressed writes reject | 8.1 |

### 4. Rendering

| ID | Requirement and fixed source | Existing implementation / case | Required oracle | Tasks |
| --- | --- | --- | --- | --- |
| R01 | Direct/indexed/indirect primitives, base vertex/instance, instancing and stepping | renderbox, indirect, execution | Geometry/IDs, nonzero offsets/base instances, valid index types, overflow rejection | 8.1 |
| R02 | Depth/stencil tests, masks, front/back operations, bias/clamp, depth/stencil views | depth, vkdepth, execution | Overlapping primitives give exact depth/stencil/color results for each state | 8.1 |
| R03 | Blending incl. dual-source, write masks, sRGB and wide-color paths | Render pipeline blend mapping | Per-channel numeric reference for every blend operation/factor, second source and disabled channels | 8.1 |
| R04 | MSAA color/depth/stencil resolve, store+resolve, programmable sample positions, deferred store | depth, RenderBox, blit-subresources | Coverage/sample values and preserved/resolved attachments for each supported sample count and resolve filter | 8.1 |
| R05 | Layered rendering including multisample layers; multiple viewports/scissors | Existing image arrays, fixed viewport path | Distinct layer/viewport patterns and clipping with no cross-layer writes | 8.1, 8.2 |
| R06 | Counting occlusion and visibility query offsets | occ, occlusion-semantics | Exact visible/occluded sample counts, accumulation/order and upper-offset rejection | 8.1 |
| R07 | Derivatives, sample built-ins, discard/helper behavior and stencil feedback | AIR fragment lowering | Reference derivatives/coverage/stencil with killed/helper lanes, within defined behavior | 8.1, 8.2 |
| R08 | Raster order groups (8); spec 4.3.1.2 | Ordered-color Vulkan path covers only a subset | Ordered conflicting accesses to buffers/images and independent groups produce specified output; framebuffer fetch alone is insufficient | 4.2, 8.1, 8.2 |
| R09 | Texture and memory barriers, encoder dependencies | Conservative full barriers | Legal feedback and stage dependencies observe prior writes; in-flight resources survive application release | 4.2, 8.1 |

### 5. Resources and argument buffers

| ID | Requirement and fixed source | Existing implementation / case | Required oracle | Tasks |
| --- | --- | --- | --- | --- |
| A01 | Direct buffer/texture/sampler/inline binding, table and stage limits | Existing encoders, arguments-layout, sampler | Last valid slot and invalid slot, offsets, inline sizes and all three shader stages | 5.1, 8.3 |
| A02 | Tier 2 layout/reflection, arrays, nested arguments, pointer indexing; spec 2.12.1 | Proven Tier 1 compute pointer/texture/sampler path | Dynamic/nonuniform index including nonzero entries chooses distinct resources in vertex/fragment/compute | 5.1, 5.2 |
| A03 | Tier 2 GPU-produced resource and sampler references | Existing resource token registry and BDA table | GPU copies legal device argument members, consumers see changes after synchronization; illegal thread-to-device sampler copy rejects | 5.2 |
| A04 | Indirect resources, usage declarations and lifetime | Argument retention path | Drop application references before submit; indirect reads/writes remain correct through completion and heap reuse | 4.2, 5.2 |
| A05 | Existing setter validation and atomic invalid-batch refusal | argument-encoder, argument-resources, compute-argbuf-sampler | All original valid results and invalid setter guards remain; Tier 2 adds valid forms without partial invalid writes | 5.1, 5.2 |

Partial A02/A03/A04 software evidence is in
`logs/rdna4-metal2/20261106-gpu-arguments/RESULT.md`: system Metal and the
isolated VM candidate pass dynamic resource arrays in all three stages and
fenced GPU-produced buffer pointers with early application release.
`logs/rdna4-metal2/20261106-gpu-resource-driver/RESULT.md` adds legal GPU
texture/sampler copies consumed by all three stages, Private argument storage,
heap declarations and early release; complete nested resource graphs remain open.
The illegal sampler stores are checked offline; their reference
pipeline is unsupported by the VM runtime. These results do not close Tier 2
or establish target-RDNA4 execution.

### 6. Tessellation

| ID | Requirement and fixed source | Existing implementation / case | Required oracle | Tasks |
| --- | --- | --- | --- | --- |
| P01 | Triangle/quad post-tessellation vertex functions, control-point and patch inputs; spec 4.1.1.1 | Existing TES translation fixtures only | Generated VS/TCS + TES validate and execute; identify patch/control-point/instance values | 6.1 |
| P02 | Packed half factors, per-patch/per-instance strides, partition modes, winding, max factor 64 | Patch adapter missing | Known subdivision geometry for each partition, factor boundaries, culling and malformed layouts | 6.1, 6.2 |
| P03 | Direct/indexed/indirect patch draws, base instance and control-point stepping | Patch draw path missing | Compute-generated factors then exact geometry/pixel comparison; both index and argument offsets | 6.2 |
| P04 | Patch draws inside CPU/GPU ICBs | ICB path missing | Equivalent direct and ICB patch output, inherited/explicit state and bounded ranges | 6.2, 7.1..7.3 |

### 7. Indirect command buffers

| ID | Requirement and fixed source | Existing implementation / case | Required oracle | Tasks |
| --- | --- | --- | --- | --- |
| I01 | CPU ICB render commands, descriptor constraints, ranges, reset and reuse | Pipeline support query currently NO | Equivalent direct draw/indexed/patch commands, empty/reset slots and range boundaries | 7.1 |
| I02 | MSL 2.1 GPU encoding/copying ICB commands; spec 5.16 | GPU ICB lowering missing | GPU producer changes rendered output and parameters; no CPU command-decision readback; slot order preserved | 7.2 |
| I03 | Pipeline/buffer inheritance, explicit state, barriers and command resource lifetime | Existing direct/indirect encoders are reusable | Mixed dependent draw order, distinct registered states, early release, event and heap interactions | 7.1..7.3 |
| I04 | Partial-submit failure, error status and retirement | Existing no-replay rule | Inject a later failure; accepted commands are not replayed and their backing remains retained until safe | 7.3 |

The fixed MSL 2.1 section 5.16 supports ICB **render** commands. Later compute
ICB command types are an extension and must remain separately gated; ordinary
indirect compute dispatch is already required by K01. This distinction preserves
the change's rule that supported ICB command types match their capability report.

### 8. Heaps and synchronization

| ID | Requirement and fixed source | Existing implementation / case | Required oracle | Tasks |
| --- | --- | --- | --- | --- |
| H01 | Automatic heaps, actual resource size/alignment, available/used size | Real Vulkan backing with requirements2 sizing; software evidence below | Real buffer/image backing, memory type/dedicated constraints and allocation boundaries | 4.3 |
| H02 | Aliasable resources, backing reuse, purgeable state and retirement | Separate logical release and GPU-retired physical leases; software evidence below | Repeated alias cycles within fixed heap capacity, correct in-flight contents, bounded physical allocations | 4.2, 4.3 |
| H03 | Encoder/resource fences and barriers | Checkpoint-backed fence segments and full barriers; software evidence below | Compute/render/blit writes visible at the encoded dependency; resources stay alive through retirement | 4.2 |
| H04 | Events/shared events at encoded positions, cross logical queues | Ready-head segment scheduler; software evidence below | Same-CB signal-then-wait, two-queue producer/consumer and CPU notification, bounded progress and final completion | 4.1 |
| H05 | Failure and concurrent lifetime | Existing submit poison/isolation | No use-after-free, alias reuse or replay after unproven GPU retirement | 3.7, 4.1..4.3, 7.3 |

H03/H04 software evidence is recorded in
`logs/rdna4-metal2/20261105-110230/fences/RESULT-final-4.2.md` and
`events/RESULT-final-4.1.md` under the same run. These cover production
scheduling/failure paths and actual GPU data on both isolated VM IOSurface
paths. Target-RDNA4 execution and the unfinished ICB interactions remain
open; these entries do not establish complete Metal 2 support.

H01/H02 software evidence is under the same run's `heaps/`, especially
`source-heap-ordered-final-v5.json`, `vm-heap-ordered-final-v5-build.log`,
`vm-final-gpu-v5-reference.log`, `vm-final-gpu-v5-candidate.log` and
`vm-final-mtlprobe-v7.log`. Seventeen simulated production-path cases cover
requirements, dedicated/type constraints, imports and rollback. Four GPU cases
pass on system Metal and the isolated candidate, including mixed formats,
byte-tail data, in-flight refusal and repeated reuse of one physical block.
The existing heap command passes 51 checks with explicit sizing/availability
upgrades and an added A8 sampling oracle. See `automatic-heaps.md` for usage
and the distinction between reserved capacity, charged requirements and backing.

## Numeric limits

Each boundary is a separate assertion in `metal2-limits`, owned by task 8.3;
texture/format boundaries also belong to 8.1, binding boundaries to 5.1 and
tessellation boundaries to 6.1/6.2. The values come from the pinned table,
pages 4/5, Mac2 column. They are feature limits, not proof that current code
implements them. Device-dependent queries must reflect actual implementation.

| Limit ID | Property | Fixed value / rule |
| --- | --- | --- |
| L01 | Vertex attributes; direct buffer arguments per function | 31 each |
| L02 | Direct texture arguments per function | 128 |
| L03 | Direct samplers including inline constant samplers | 16 |
| L04 | Threadgroup-memory argument entries | 31 |
| L05 | Inline constant buffers and length | 14 buffers, 4 KiB each |
| L06 | Threads per threadgroup | 1024; pipeline may report a lower actual shader-specific maximum |
| L07 | Total threadgroup memory; length alignment | 32 KiB; 16 bytes |
| L08 | Constant-address-space function allocation | 64 KiB |
| L09 | Fragment stage-in inputs / components | 32 inputs / 124 components, respecting both limits |
| L10 | Function constants | 65536 identifiers |
| L11 | Tessellation factor | 64 |
| L12 | Viewports / scissors | 16 |
| L13 | Raster order groups | 8 |
| L14 | Buffer binding / texture-to-buffer copy alignment | 256 bytes in the family table; preserve documented legal less-restrictive existing paths and verify actual Vulkan windows |
| L15 | 1D / 2D / cube dimensions | 16384 |
| L16 | 3D dimensions | 2048 |
| L17 | Array layers | 2048 |
| L18 | Color attachments | 8; no additional family-wide aggregate pixel-bit limit |
| L19 | Maximum point primitive size | 511 |
| L20 | Visibility query offset | 256 KiB; validate exact aligned valid range |
| L21 | Fence objects | 32768 |
| L22 | Single buffer length / recommended working set | Runtime device properties; the table does not establish a per-app 4 GiB limit |
| L23 | RW texture tier, sample counts, Depth24 support | Runtime support and per-format execution; no invented universal value |

Tier 2 indirectly referenced resource counts are not the direct argument table
counts. Test actual encoder length, array capacity and backend descriptor
limits separately (A02). They must not be truncated to 31/128/16 arbitrarily.

## Pixel-format capability matrix

Task 8.1 owns a `metal2-formats` case for each listed format and capability bit;
8.3 owns corresponding queries. `F/W/C/B/M/R` mean filter, shader write, color
target, blend, MSAA and resolve. All listed formats also have the table's
applicable shader read/sample operation. “All” means all six bits. The spec's
type, stage and read-write-tier restrictions still apply. Use pixel/bit-pattern
or decoded-block references for operations, not just successful creation.

| Format ID | Formats (each named member is a case) | Mac2 capability bits |
| --- | --- | --- |
| F01 | A8Unorm | F |
| F02 | R8Unorm, R8Snorm | All |
| F03 | R8Uint, R8Sint | W C M |
| F04 | R16Unorm, R16Snorm, R16Float | All |
| F05 | R16Uint, R16Sint | W C M |
| F06 | RG8Unorm, RG8Snorm | All |
| F07 | RG8Uint, RG8Sint | W C M |
| F08 | R32Uint, R32Sint | W C M |
| F09 | R32Float | All |
| F10 | RG16Unorm, RG16Snorm, RG16Float | All |
| F11 | RG16Uint, RG16Sint | W C M |
| F12 | RGBA8Unorm, RGBA8Snorm, BGRA8Unorm | All |
| F13 | RGBA8Unorm_sRGB, BGRA8Unorm_sRGB | F C M R B |
| F14 | RGBA8Uint, RGBA8Sint | W C M |
| F15 | RGB10A2Unorm, RG11B10Float | All |
| F16 | RGB10A2Uint | W C M |
| F17 | RGB9E5Float | F |
| F18 | RG32Uint, RG32Sint | W C M |
| F19 | RG32Float | All |
| F20 | RGBA16Unorm, RGBA16Snorm, RGBA16Float | All |
| F21 | RGBA16Uint, RGBA16Sint | W C M |
| F22 | RGBA32Uint, RGBA32Sint | W C M |
| F23 | RGBA32Float | All |
| F24 | BC1_RGBA, BC2_RGBA, BC3_RGBA and their sRGB variants | F |
| F25 | BC4_RUnorm, BC4_RSnorm, BC5_RGUnorm, BC5_RGSnorm | F |
| F26 | BC6H_RGBFloat, BC6H_RGBUfloat, BC7_RGBAUnorm and BC7_RGBAUnorm_sRGB | F |
| F27 | GBGR422, BGRG422 | F |
| F28 | Depth16Unorm, Depth32Float | F M R |
| F29 | Stencil8, X24_Stencil8, X32_Stencil8 | M; stencil views retain aspect rules |
| F30 | Depth24Unorm_Stencil8 | F M R if `depth24Stencil8PixelFormatSupported`; otherwise explicit refusal |
| F31 | Depth32Float_Stencil8 | F M R; aspect/view restrictions apply |
| F32 | BGR10A2Unorm | F W C M R B |
| F33 | R8Unorm_sRGB, RG8Unorm_sRGB; packed B5G6R5/A1BGR5/ABGR4/BGR5A1; PVRTC/EAC/ETC/ASTC/HDR-ASTC; BGRA10_XR/BGR10_XR variants | Not part of Mac2's required formats; reject or report a separately verified extension |

The table notes device variation for some formats. Preserve the actual queried
support and gate it on execution; do not infer every optional operation from an
enum's existence. Depth24 may be absent; format aliases must not expose a
writable stencil or depth aspect accidentally. Integer filtering and sRGB
shader writes are not enabled by a generic “all formats supported” answer.

## Additional large-memory acceptance

These are project requirements beyond a family-name query. They remain
independent of the separate 16 GiB CPU BAR change.

| ID | Oracle | Tasks |
| --- | --- | --- |
| M01 | Multiple actual Private resources total at least 8 GiB on the target; shared-budget OOM/release/isolation | 2.1, 2.2, 3.7, 10.2 |
| M02 | One 8 GiB logical Private buffer; beginning, both sides of 4 GiB, far end and guards | 3.1..3.3, 3.6, 10.2 |
| M03 | Same invocation uses runtime 64-bit GEP/helper/nested-pointer accesses across 4 GiB; no rebinding/subdivision substitute | 3.4, 10.2 |
| M04 | Shared/Managed continuous CPU VA above 4 GiB; actual page accounting and bidirectional visibility | 3.5, 10.2 |
| M05 | Multiple NoCopy imports total above 4 GiB; original owner/deallocator/readonly and per-import bound | 3.5, 10.2 |
| M06 | Segmented copy/fill/upload/readback, tails/guards and later-chunk failure without replay | 3.6, 3.7, 10.2 |
| M07 | PT/handle/map caps, transactional rollback and close/hang isolation; bounded repeat cycles | 3.1, 3.2, 3.7 |
| M08 | Pre-open read-only discovery; physical/pool/visible/budget/available/recommendation describe distinct quantities | 2.2, 3.3 |

## Later or separate capabilities

Placement heaps and texture swizzles (macOS 10.15), later compute ICB types,
MSL 2.2+, sparse resources, dynamic libraries, binary archives, ray tracing,
mesh/object shaders, Apple-only tile/imageblock/memoryless/programmatic-blend
features, Metal3/Metal4 families and MPS operator coverage do not become
requirements through a current Mac2 query. Existing supported extensions remain
regression obligations. In particular, preserving the WS compatibility family
answer does not certify Metal3 for applications.

Deferred work recorded 2026-11-06: implement sparse resources / PRT in a
separate change. RDNA4N currently rejects PRT reservations and sparse binds;
RADV's inherited sparse advertisement makes `vulkaninfo` fail with PRT `-22`
and `ERROR_OUT_OF_DEVICE_MEMORY` on the physical target. A capability-hiding
patch is prepared but has not been deployed. Restore advertisement only after
implementing kernel PRT mappings, Vulkan sparse bind/unbind, nonresident-access
semantics, synchronization and resource lifetime, with physical validation;
assess Metal sparse API exposure separately. This remains outside fixed Mac2 /
MSL 2.1 acceptance. Evidence:
`logs/rdna4-hardware-test/desktop-failure-20261106/20261105T163547Z-desktop/vulkan.output.log`.

## Evidence and closure

Each future evidence record must name the ID, applicable format/operation or
language overload, source hash, backend and oracle result. A compiler or host
pass cannot close its execution assertion. A VM backend missing a required
feature records that feature as unverified, with the executable live case ready;
it does not become PASS. Synthetic 8 GiB layouts cannot close M01..M05 physical
capacity checks. Tasks 9.1..9.3 verify software integration and provenance;
10.1..10.3 run the physical matrix last, after explicit release. Old regression
contracts and permitted query upgrades are in `metal2-baseline.md`.
