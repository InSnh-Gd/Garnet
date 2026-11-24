Build (macOS, Rust >= 1.87; automated by tools/rdna4/build-xlate.sh):
    git clone https://github.com/steelbrain/metal2vulkan
    cd metal2vulkan && git checkout 43c46ac8a24adf1a6e872b8a52c706ec9614fad0
    for patch in /path/to/Garnet/third-party/metal2vulkan-rdna4/patches/*.patch; do
        git apply "$patch" || break
    done
    cp -R /path/to/Garnet/third-party/metal2vulkan-rdna4/garnetxlate .
    cargo build --release -p garnetxlate     # target/release/libgarnetxlate.dylib
See NOTICE for the licence (LGPL-3.0-or-later; separate from the MIT code in this repository).
Note: tools/rdna4/build-xlate.sh overlays garnetxlate/ after applying patches/ (0001..0082).

Verification and Tier 2 Scope
=============================
Unless explicitly stated in a section below, wire contracts remain at the
current baseline (C ABI 3, reflection 66, argument-table ABI 9, source-proof
version 2). Patch-level SPIR-V, wrapper, and host/LavaPipe checks verify
translation, metadata invariants, and software Vulkan 1.4.363 execution only;
they do not establish physical RDNA4 GPU execution or complete Tier 2
acceptance. Runtime capability/execution gating stays controlled by the native
bundle and compatibility runner.

0082-metal3-renderbox-visible-ref-trailing-ptr-params.patch relaxes the direct
visible-function reference signature check in `src/linked_functions.rs`
(`rewrite_visible_reference_stubs` / `visible_ref_params_compatible`) to accept
Apple RenderBox / `[[stitchable]]` superset stubs that pass unused pointer slots
(`LlType::Ptr(_)`) beyond the linked `privateFunction`'s defined parameter list
(matching `native::emitter::body::calls::function_call_args_for_params`), while
preserving strict mismatch rejection on return types, non-pointer parameters,
extra callee parameters, and `MTLDynamicLibrary` links.

0081-metal3-air-sparse-residency-polarity.patch fixes Apple AIR sparse status
polarity in `src/passes/air_calls/images/resolve.rs` (`encode_residency_member`
and `lower_null_texture_result_with_sparse`). Apple's `<metal_texture>`
`sparse_color<T>::resident()` computes `((status & 1) ^ 1)` on member 1 of
`{ texel, i8 }` (TFE convention: `0` = resident, `1` = non-resident), so
`encode_residency_member` selects `0` when `OpImageSparseTexelsResident` is true
and `1` when false (and absent/null sparse textures materialize status `1`),
fixing live RX 9060 XT `test-metal3-sparse-prt` residency detection.

0080-metal3-void-atomic-air-calls.patch handles void-returning buffer atomic
RMW intrinsics emitted by Apple `air64_v28` (`atomic_{min,max}_explicit` and
dead-result `add`/`sub`/`min`/`max`/`and`/`or`/`xor`/`xchg` across `global`/
`local` and `i32`/`i64`/`f32`) in `src/native/emitter/ops/air_calls.rs` by
routing both `emit_void_air_call` and `emit_value_air_call` through the shared
`emit_atomic_rmw_air_call` helper (discarding the SPIR-V SSA result id on void
calls), fixing live RX 9060 XT translation of `k_u64_minmax`.

0079-metal3-apple-air-live-fixes.patch fixes four live macOS 26.7.2 Apple AIR
(`air64_v28`, `metalfe-32023.886.1`) translator gaps captured on RX 9060 XT:
1. Real Apple AIR Mesh & Object shaders + `.MTL_VISIBLE_FN_REF`
   (`air_intrinsics.rs`, `meta/mod.rs`, `reflect/mod.rs`, `passes/mesh_stage.rs`,
   `native/ir/pointer_pointees.rs`, `passes/workgroup/pointers.rs`,
   `linked_functions.rs`, `garnetxlate/src/{stage2_link,air_spirv}.rs`):
   supports Apple's `air.set_position_mesh`, `air.set_vertex_data_mesh.<ty>`,
   `air.set_primitive_data_mesh.<ty>`, `air.set_index_mesh` (`i8`/`i16`/`i32`),
   `air.set_primitive_count_mesh`, `air.set_point_size_mesh`,
   `air.set_primitive_culled_mesh`, `air.set_{render_target,viewport}_array_index_mesh`,
   `air.set_primitive_id_mesh`, and `air.set_threadgroups_per_grid_mesh_properties`;
   parses `!air.mesh_type_info` (`!verts`, `!prims`, `maxV`, `maxP`,
   `!"air.triangle"|"air.line"|"air.point"`), `!air.mesh_{vertex,primitive}_data`
   `attrIdx -> Location`/`user_semantic` mappings, and `!air.max_mesh_work_groups`;
   handles opaque-pointer index-0 folded payload loads/stores before GEPs on
   `addrspace(6)` payloads in both native `TaskEXT`+`MeshEXT` and
   `GARNETX_CAP_MESH_EMULATED_PAYLOAD` (`GLCompute`+`MeshEXT` `DrawIndex`
   673/674/675); and links `@<sym>.MTL_VISIBLE_FN_REF` declarations against
   `MTLLinkedFunctions.privateFunctions` (`deps`).
2. Pull-model & non-pull `half` VS/Mesh/FS varying width consistency
   (`passes/stage_output/mod.rs`, `passes/mesh_stage.rs`,
   `passes/stage_input/{air_layout,mod}.rs`, `passes/resources/collapse.rs`):
   widens `half[1..4]` inter-stage user varyings consistently to `f32` (`OpFConvert`
   at VS/Mesh stores and non-pull FS loads, matching `InterpolateAt*` `f32` pull
   inputs) so `half2` pull-model and regular varyings match without requiring
   `StorageInputOutput16`.
3. Opaque-pointer index-0 `Array`/`Struct` GEP folding in local atomics
   (`native/emitter/ops/atomics.rs`): peels leading zero indices via
   `OpInBoundsAccessChain` when an atomic pointer directly targets an
   `Array`/`Struct` aggregate (e.g. `[4 x %"struct.metal::_atomic.0"]` / `{ float }`).
4. Real Apple AIR `air.intersect.*` + `MTLIntersectionFunctionTable` dispatch
   (`native/ray_query_air.rs`, `linked_functions.rs`): enables IFT lowering on
   Apple's 20-arg `@air.intersect.*` calls whenever the table operand is non-null
   (without requiring `.intersection_function_buffer.`), fixes trailing
   `intersection_params` operand indices (`opacity_cull_mode = cur+3`,
   `force_opacity = cur+4`, `function_table_offset = cur+8`), strips LLVM
   parameter attributes (`align N`, `dereferenceable(N)`, quoted attributes),
   rewrites parameterized `!"intersection_function_table<...>"` metadata to
   `!"uint"`, scopes callback parameter role decoding to each callback's own
   `!air.intersection` metadata node, and wires runtime `OpSwitch` dispatch for
   packed `<{ i1, float }>` bounding-box (`OpRayQueryGenerateIntersectionKHR`)
   and `i1` triangle any-hit (`OpRayQueryConfirmIntersectionKHR`) callbacks.

0078-metal3-live-air-fixes-and-gfx-object-mesh.patch fixes two live macOS
26.7.2 Apple AIR translator bugs and adds GFX-ring emulated Object+Mesh payload
mode (for RADV `meshShader=1 && taskShader=0`):
1. Pull-model interpolation (`air_intrinsics.rs`,
   `passes/air_calls/integer_simd.rs`, `passes/stage_input/mod.rs`): recognizes
   all Apple AIR naming variants (`air.interpolate_{center,centroid,sample,offset}_{no_perspective,perspective}.*`,
   `air.interpolate_{center,centroid,sample,offset}.*`, and
   `air.interpolate_at_{center,centroid,sample,offset}*`) across scalar/v2/v3/v4
   `f32`/`f16`, ensures `NoPerspective` decoration on `_no_perspective` inputs,
   and preserves Metal `[0,1)` -> Vulkan `[-0.5,+0.5)` offset shift.
2. Runtime VFT indirect-call parsing (`native/{parse,mod}.rs`,
   `linked_functions.rs`): replaces naive `split_whitespace().last()`/`rfind()`
   argument/return type splitting with top-level LLVM type parsing so constant
   vector/aggregate literals (e.g. `<4 x float> <float 0.0, ...>`) and
   attributed/fast-math indirect calls emit valid `@metal2vulkan.vft.dispatch.*`
   signatures.
3. GFX-ring emulated Object+Mesh payload mode (`passes/{mod,finalize,
   mesh_stage,module_cleanup,stage_input/mod,resources/{bindings,collapse}}.rs`,
   `reflect/mod.rs`, `lib.rs`, `garnetxlate/{garnetxlate.h,src/*}`): opt-in via
   `TransformOptions::with_emulated_mesh_payload()` and C ABI flag
   `GARNETX_CAP_MESH_EMULATED_PAYLOAD` (`1u << 8`, keeping `GARNETX_ABI_VERSION
   == 3u`). Reserves set-0 `VK_DESCRIPTOR_TYPE_STORAGE_BUFFER` bindings
   `GARNETX_MESH_GRID_PARAMS_BINDING` (`673`, `{obj_grid_x, obj_grid_y,
   obj_grid_z, payload_stride_bytes}`), `GARNETX_MESH_PAYLOAD_BINDING` (`674`,
   `uint32_t[]` payload words at `group_id * (payload_stride_bytes / 4)`), and
   `GARNETX_MESH_INDIRECT_BINDING` (`675`, `VkDrawMeshTasksIndirectCommandEXT`
   triples). Lowers `Stage::Object` to `GLCompute` (no `MeshShadingEXT`), copies
   `object_data` payload to binding `674` and indirect dims to binding `675`;
   lowers `Stage::Mesh` with payload to `MeshEXT` + `OpCapability
   DrawParameters` (`BuiltIn DrawIndex`) reading bindings `673`/`674` (leaving
   mesh-only stages without payload as pure hardware `MeshEXT`, and leaving
   native `TaskEXT`/`TaskPayloadWorkgroupEXT` unchanged when bit 8 is unset).
   Reflection JSON exposes `"emulated_payload": true`, 16-byte-aligned
   `"payload_stride"`, and distinct `"MeshEmulatedGridParams"`,
   `"MeshEmulatedPayload"`, and `"MeshEmulatedIndirect"` binding kinds.

0077-metal3-ray-query-and-intersection-tables.patch adds hardware
`SPV_KHR_ray_query` lowering (`OpCapability RayQueryKHR`, `OpExtension
"SPV_KHR_ray_query"`) across `native/ray_query_air.rs`, `passes/ray_query.rs`,
`passes/stage_input/mod.rs`, `passes/resources/collapse.rs`,
`passes/module_cleanup.rs`, `meta/{embedded,mod}.rs`, `linked_functions.rs`,
`reflect/{mod,tests}.rs`, and `lib.rs` (opt-in via
`TransformOptions::with_hardware_ray_query()`, enabled by `garnetxlate` while
preserving legacy default fallback behavior). Top-level Metal
`primitive_acceleration_structure` / `instance_acceleration_structure` bindings
map to `OpTypeAccelerationStructureKHR` (`VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR`
at `BUFFER_BINDING_BASE + n`), and argument-buffer acceleration structures lower
64-bit GPU device addresses via `OpConvertUToAccelerationStructureKHR`
(`bda_abi: "u64_device_address"`). Lowers `air.intersect.*` families to
`OpRayQueryInitializeKHR`, `OpRayQueryProceedKHR`, `OpRayQueryTerminateKHR`,
`OpRayQueryConfirmIntersectionKHR`, `OpRayQueryGenerateIntersectionKHR`, and
committed/candidate getters (`Type`, `T`, `Barycentrics`, `FrontFace`,
`InstanceId`, `InstanceCustomIndex`, `PrimitiveIndex`, `GeometryIndex`,
`ObjectToWorld`, `WorldToObject`, `WorldRayOrigin`, `WorldRayDirection`,
`ObjectRayOrigin`, `ObjectRayDirection`) across compute, vertex, fragment,
object, and mesh stages. Adds runtime `MTLIntersectionFunctionTable` dispatch
(`specialize_linked_module_with_tables`, `RuntimeIntersectionFunctionTableInfo`)
inside `OpRayQueryProceedKHR` via bounded `OpSwitch` reading the shared
`uint32_t[]` table buffer ABI (`word[0] = slot_count`, `word[1 + slot] =
handle_id`, `slot = sbt_offset + geometry_index + max(0, function_table_offset)`)
for custom bounding-box (`IntersectionFunctionKind::BoundingBox`) and triangle
any-hit (`IntersectionFunctionKind::TriangleAnyHit`) functions with safe
handle-0/OOB fallback (confirm for triangles, miss for bounding boxes).
`garnetxlate` keeps `garnetx_abi_version() == 3` and adds `garnetx_detect_stage`
(`GARNETX_STAGE_*`) and `garnetx_translate_stage3` (`RDNA4xIntersectionFunction`),
exposing `"acceleration_structures"` and `"intersection_function_tables"` in
trimmed reflection JSON when present.

0076-metal3-mesh-and-object-shaders.patch adds Metal 3 Object (`Stage::Object`,
`OpEntryPoint TaskEXT`) and Mesh (`Stage::Mesh`, `OpEntryPoint MeshEXT`) shader
lowering (`OpCapability MeshShadingEXT`, `OpExtension "SPV_EXT_mesh_shader"`)
across `passes/mesh_stage.rs`, `air_intrinsics.rs`, `meta/{mod,types}.rs`,
`native/{emit_tiers,emitter/{functions,helpers},ir/metadata_pointees,owned_cfg,
tir/storage}.rs`, `passes/{mod,finalize,module_cleanup,stage_input/*,
stage_output/mod,resources/*,workgroup/*,access/raw_byte}.rs`,
`emitted_effects.rs`, `linked_functions.rs`, `reflect/{mod,tests}.rs`, and
`lib.rs`. Supports `TaskPayloadWorkgroupEXT` payloads `>= 16,384` bytes
(`addrspace(3|6|7)`), `air.set_mesh_properties` / `set_threadgroups_per_grid`
-> `OpEmitMeshTasksEXT`, `LocalSize`, `OutputVertices`, `OutputPrimitivesEXT`,
`OutputPoints`/`OutputLinesEXT`/`OutputTrianglesEXT`, `OpSetMeshOutputsEXT`
(including late primitive-count culling via `CullPrimitiveEXT`), per-vertex
output arrays, `PerPrimitiveEXT` primitive output arrays and fragment inputs,
`Primitive{Point,Line,Triangle}IndicesEXT`, `CullPrimitiveEXT`, `Layer`, and
`ViewportIndex`. Trimmed reflection JSON exposes `"object"`, `"mesh"`, and
`PerPrimitiveEXT` `"varyings"` when present while leaving non-mesh shaders
unchanged.

0075-metal3-sparse-prt-residency.patch adds Sparse / PRT residency predicate
lowering across `passes/air_calls/images/{resolve,sample,sample_depth,read,
cube_read,gather,query_offset,sampler_coordinates,mod}.rs`, `passes/mod.rs`,
`passes/image_facts.rs`, `passes/module_cleanup.rs`, and
`passes/resources/query_select.rs`. MSL `sparse_color<T>` texture
`sample`/`read`/`gather` (explicit `.sparse.` AIR names OR live member-1
residency uses on `{ texel, i8 }` structs, including propagation through
`select`/`phi`/`alloca`) lower to SPIR-V `OpImageSparseSampleImplicitLod`,
`OpImageSparseSampleExplicitLod`, `OpImageSparseSampleDref*`,
`OpImageSparseFetch`, `OpImageSparseRead`, `OpImageSparseGather`, and
`OpImageSparseDrefGather` paired with `OpImageSparseTexelsResident`, emitting
`OpCapability SparseResidency` only when sparse residency is actually used.

0074-metal3-visible-function-tables.patch adds runtime `MTLVisibleFunctionTable`
bounded switch-table dispatch in `linked_functions.rs` and `lib.rs` across
compute, vertex, and fragment stages (`specialize_linked_module_with_vft`,
`RuntimeVisibleFunctionTableInfo`). Table parameters (`!"air.visible_function_table"`)
lower to read-only `StorageBuffer` (`uint32_t[]`: `word[0] = slot_count`,
`word[1 + slot] = handle_id` with `0` = null and `1..=N` sorted deterministically
by symbol name), bounds-checking negative/OOB/empty indices safely to handle `0`
and lowering indirect calls to `@metal2vulkan.vft.dispatch.<k>` bounded
`OpSwitch` with safe zero/`ret void` default (never `OpUnreachable`).
`garnetxlate` exposes the resolved descriptor binding and deterministic handle
map in trimmed reflection JSON under `"visible_function_tables"` (`abi_version=1`,
`header_words=1`) only when runtime VFTs are present.

0073-metal3-dynamic-library-linking.patch adds `MTLDynamicLibrary` external
symbol enumeration (`exported_symbols`) and transitive multi-library linking
(`link_dynamic_libraries`, `DynamicLibraryModule`) across compute, vertex, and
fragment stages in `linked_functions.rs` and `lib.rs`, renaming colliding
`internal`/`private` helpers and globals, validating caller `declare` vs
provider `define` signatures, and refusing missing, mismatched, or conflicting
exports with descriptive errors naming the symbol. `garnetxlate` exports
`garnetx_list_exported_symbols` and `garnetx_translate_linked_libraries`
(declared in `garnetxlate/garnetxlate.h`, keeping `garnetx_abi_version() == 3`
and all ABI 2/3 entry points unchanged).

0072-metal3-compute-icb-gpu-lowering.patch extends GPU ICB lowering in
`ops/icb.rs` and `air_intrinsics.rs` to lower Compute ICB intrinsics
(`reset_compute_command`, `copy_compute_command`,
`set_pipeline_state_compute_command`, `set_kernel_buffer_compute_command.*`,
`set_barrier_compute_command`, `clear_barrier_compute_command`,
`set_threadgroup_memory_length_compute_command`,
`set_stage_in_region_compute_command`,
`concurrent_dispatch_threadgroups_compute_command`, and
`concurrent_dispatch_threads_compute_command`) into the 640-byte
`rdna4_icb_slot_t` layout (keeping `set_object_buffer_render_command` refused),
and extends `passes::icb_stage::lower_icb_stage` / `garnetx_lower_icb_stage` to
accept `stage_kind == 3` (`GLCompute`), rewriting compute buffer descriptors to
the dynamic BDA table at binding 676 (`vs_params_binding = 0`).

0071-metal3-msl3-atomics-bfloat-and-simd.patch adds MSL 2.2-3.2 `atomic<float>`
(`add`/`sub`/`load`/`store`/`xchg` via `VK_EXT_shader_atomic_float`
`AtomicFloat32AddEXT`/float atomics, plus structured CAS loops over `u32` words
for `min`/`max`/`cmpxchg.weak` and shared logical buffers), 64-bit integer
atomics (`atomic<int64_t>`/`atomic<uint64_t>` with `Int64Atomics`), `bfloat`
conversions (`bfloat <-> half`, `int <-> bfloat`), vector `llvm.fmuladd.*bf16`,
`bf16` GLSL math/saturate, generalized storage texture atomics
(`air.atomic_*_explicit_texture_*` across 1D/1D_array/2D/2D_array/3D/buffer_1d,
32/64-bit signed/unsigned, scalar & 4-lane ABIs, preserving explicit atomic
image formats in `garnetxlate`'s `patch_storage_formats`), and 3-arg
`air.simd_shuffle_and_fill_{up,down}` (default `modulo=32`).

0070-metal3-barycentrics-and-pull-model-interpolation.patch completes fragment
`[[barycentric_coord]]` (`SPV_KHR_fragment_shader_barycentric`,
`BaryCoordKHR`/`BaryCoordNoPerspKHR`) and adds pull-model fragment
interpolation (`interpolant<T>::interpolate_at_{center,centroid,sample,offset}`
-> `OpLoad` / `GLSL.std.450` `InterpolateAt{Centroid,Sample,Offset}` with
`InterpolationFunction`, `NoPerspective` preservation, Metal `[0,1)` to Vulkan
`[-0.5,+0.5)` offset shift, and `half` result conversion).

0068-gpu-icb-lowering-and-stage-bda.patch adds native AIR->SPIR-V lowering for
MSL 2.1 render indirect command buffer intrinsics (`air.reset_render_command`,
`air.set_pipeline_state_render_command`, `air.draw_primitives_render_command`,
`air.copy_render_command`, plus typed `set_vertex_buffer`, `set_fragment_buffer`,
`draw_indexed_primitives`, `draw_patches`, and `draw_indexed_patches`) to BDA
stores against `rdna4_icb_header_t`/`rdna4_icb_slot_t`, while keeping compute ICBs
and `set_object_buffer_render_command` refused. It also adds
`passes::icb_stage::lower_icb_stage` (and `garnetx_lower_icb_stage` in
`garnetxlate/`) to convert stage buffer descriptors and vertex inputs into
dynamic BDA bindings (`676`/`677`) for strict-order GPU ICB execution. User cache tag:
rdna4x-54-gpu-icb-render-lowering.

0067-exclude-opaque-and-nonbuffer-roots-from-bda.patch prevents opaque texture/sampler
handles and non-buffer raw parameters from entering `bda_forward_addresses` and
`bda_phi_value_is_addressable` when `large_buffer_addressing` is enabled (and guards
local pointer-field BDA loads against non-addressable opaque resource slots). This fixes
DockHelper's linked Liquid Glass SDF shader (`ffgea5cia6dheb2a3` + `custom_fn`), which
stores an opaque `texture2d` parameter into an `alloca` struct field alongside a
dynamically indexed `constant` buffer pointer. User cache tag:
rdna4x-53-opaque-resource-bda-exclusion.

0066-indirect-embedded-lod-queries.patch extends the indirect embedded-resource
gates (`body_uses_texture_intrinsic`, `is_air_sampler_operand`,
`register_embedded_samplers`, `texture_array::is_texture_intrinsic`, and
`discovery.rs`) to recognize query-only `air.calculate_clamped_lod_texture*`
and `air.calculate_unclamped_lod_texture*` uses inside structured argument
buffers (with AIR metadata providing the component type rather than assuming
float), unwraps prior `normalize_sampler_coordinate` selections when postpass
re-runs `lower_argument_lod_queries` (preventing double normalization and
pruning dead pre-postpass size queries), and fixes `lod_query/coordinates.rs`
to use ABI 9+ null-shape dummy indices for `argument_table_version >= 9`
(including ABI 10) rather than `== 9`. Dynamic untyped graphs without a known
component/access type remain rejected. User cache tag:
rdna4x-52-indirect-embedded-lod-queries.

0063-widen-fragment-color-outputs.patch converts half-precision fragment color
values to float32 at attachment stores. Internal half arithmetic and attachment
formats remain unchanged. This avoids relying on a 16-bit stage output interface
to write half-float or normalized attachments. Precompiled modules are unaffected.

0064-preserve-linked-static-samplers.patch preserves constexpr sampler state
when linking dependency modules. The linker merges only the sampler-state root
and gives colliding sampler globals distinct names before appending their module.
Reflection and SPIR-V keep the same name ordering. The bundled linked icon
fragment is regenerated from its original AIR inputs without changing its linked
shader key. User cache tag: rdna4x-50-linked-static-samplers.

0065-do-not-cache-forward-select-edge-address.patch stops `bda_phi_address_id`
from inserting an edge-local `OpSelect` emitted for a forward pointer select
into `bda_direct_addresses`. Caching that latch-local ID poisoned later uses
(such as a loop-exit phi) with a non-dominating SSA ID, forcing relooper
fallback and dropping the final iteration's update in RenderBox cubic-path
binary searches. User cache tag: rdna4x-51-forward-select-edge-addr.

ABI 2–8 Baseline & Patches 0022–0025
====================================
ABI 2 exports the optional garnetx_lower_argument_resources operation. It takes
cached SPIR-V and matching metadata, including linked translations, and returns
GPU token lookup plus runtime resource arrays. Existing translation entry points
retain the static descriptor contract for representable flat fields. Consumers must
implement metadata argument_resource_tables version 6 before using this operation.
Nested sampling graphs use actual GPU token words during translation and emit
table binding 672 and sorted categories for sets 2..4, without raw root
descriptors. The optional postpass preserves this completed contract. Both paths
require descriptor indexing and resource retention through command completion.
Nested texel reads/writes preserve 64-bit tokens and derive query descriptor types
from typed AIR image operations; unknown access/type combinations are rejected.
Dynamic is_null_texture checks compare both token words, including shaders that only test nullness.
Dimension queries add binding 673 aliasing the token buffer.
Its appended rows describe the actual Metal texture view; query-only shaders need
no typed image descriptor or runtime image array. Null queries return zero.
Token-backed texel reads/writes guard the image descriptor load on the GPU;
null reads return all-zero components and null writes are discarded. This also
applies to the static-resource postpass. Finished reflection drops unused embedded texture/sampler descriptors
while preserving the argument-buffer field layout.

ABI 5 appends shape/type-specific sampled image descriptors after the sampled token count;
these are null images when nullDescriptor is enabled, otherwise matching image views.
The ordinal is 2*Dim + Arrayed + 8*Depth + 16*Component, with Component
0=float32, 1=uint32, 2=int32. Float offsets remain compatible with ABI 4.

ABI 6 adds maxSamplerLodBias (float32 bits) at table word 6. Nonuniform fragment
samples use spatial gradients, with bias clamped to that runtime device limit.
Sampler mipLodBias must be zero. Depth sample_compare/gather_compare use native
Dref operations and depth image types. Null sampling/gather selects the matching
descriptor and an all-zero result without changing control flow. Scalar opaque handles also
remain opaque when ordinary helper inlining introduces a parameter identity.

ABI 7 / reflection 63 distinguishes the ordinary and comparison representations of each
Metal sampler. The ordinary descriptor stays at stage-local binding 160+n; the comparison
descriptor is at 160+n+576 with the same descriptor set and count. A mixed sampler reports
its ordinary descriptor as primary plus comparison_descriptor; a comparison-only sampler
reports the +576 binding as primary with no pair; an ordinary-only sampler keeps one binding.
Direct and constexpr samplers follow the same rule. This makes ordinary sample and depth
sample_compare/gather of one sampler independently selectable, including Never..Always
compare functions.

GPU argument table version 7 preserves header words 0..6, including finite/nonnegative
maxSamplerLodBias at word 6, and begins the sorted token pairs at word 7. JSON must carry a
comparison_sampler boolean in table 7; tables 1..6 must not carry it. For sampler token count
N (including descriptor index 0), set 4 binding 0 contains N ordinary samplers when the bool
is false and 2N samplers when true: ordinary at 0..N-1 and comparison at N..2N-1. The shader
loads N from table word 5, so compare-only graphs deliberately request 2N. Consumers must
reject an overflow of the normal/compare capacity rather than truncate it.

ABI 8 / reflection 64 adds the opt-in normalized sampler-coordinate contract. The top-level
`normalized_sampler_coordinates` JSON boolean must be true together with trimmed reflection
version 64; false or absent selects the legacy table-7 contract even when the crate's global
reflection version is 64. Non-boolean markers and true plus any other trimmed version are
invalid. A table-8 translation is the only path that may emit the direct synthetic flag buffer
at stage-local set 0 binding 674. Its exact binding is the six-key row `kind`, `metal_index`,
`descriptor`, `declared_size`, `extent`, and `access`, with descriptor `set`/`binding`/`count`
and extent `Object.bytes=128`; trim consumers must preserve that row exactly.

For GPU argument table version 8, the header words 0..6 and the token-pair order remain unchanged.
The sampler-coordinate flag tail starts at `word[4] + 2*N`, where N is table word 5. Bit zero is
one for Metal pixel coordinates. Image dimension rows then start at `word[4] + 3*N`, one row for
each category-0 image token (including row zero), and are indexed by the category-0 token lookup
rather than by N. Row 6 is `2*Dim + Arrayed + 8*Depth + 16*Component`; row 7 bit zero marks a
multisampled image. A stage that carries table 8 must not also carry direct binding 674. A
tableless direct-674 stage may coexist with a table-8 stage in another stage of the pipeline.

Patch 0022 teaches the argument-resource graph walker to resolve LOD query symbols on the
same typed image/sampler token after a typed image operation established the image shape. A
query-only graph without a component/access type is still rejected; query execution and replay
gating remain open.

Patch 0023 lowers native `llvm.fptoui.*` and `llvm.fptosi.*` conversions through the typed graph,
matching the text-emitter conversion path without treating an out-of-range or malformed cast as a
successful translation.

Patch 0025 unifies direct, constexpr/static, and GPU argument samplers under the same
ordinary/comparison lowering, including a single Metal sampler used for both ordinary sample
and sample_compare/gather_compare. Pixel coordinates are converted with the actual image extent
after comparison where required. The reference suite covers ordinary, compare, gather, and
gather-compare operations and Never/Always/Less compare functions.

Patches 0032–0060
=================
Patch 0032 adapts NullMoth's LGPL constant direct sampler-array materializer,
with local per-element reflection, ordinary/comparison representations and
coordinate provenance. It does not provide dynamic direct sampler indexing.
The source pin and attribution are in NOTICE; the separately licensed NullMoth
Metal plugin is not part of this adaptation.

Patch 0033 adds reflection 65 / table ABI 9 null-shape registration. The sorted,
duplicate-free null_shapes list carries canonical dimension, array, component,
depth, multisample and access words. Each stage uses a readonly 384-byte
StageResourceMetadata buffer at binding 674: words 0..31 contain direct sampler
coordinate flags and words 32..95 map that stage's null shapes into the pipeline's
shared descriptor arrays. Static null or direct-sampler stages may carry this
metadata without token binding 672. Image descriptors use sets 2/3 binding 0;
texel placeholders use binding 1. At this checkpoint the shape matrix contained 54 entries;
patch 0038 adds the six legal cube storage shapes.
ABI 9 preserves words 0..6 and token pairs from earlier tables. The explicit
normalized_sampler_coordinates option determines whether the sampler-flag tail
uses the ABI 8 layout; ABI 9 alone does not enable that option. Reflection 64 /
table 8 remains a legacy input, including its 128-byte SamplerCoordinateFlags.
Cache tag: rdna4x-18-null-shapes-abi9; postpass: rdna4x-argument-resources-19-abi9.
Cached lowering merges static and newly required null shapes, relocates existing
references, and validates the registry even on an idempotent return. Native
postpass selection also handles static null stages that still contain flat
argument fields. is_null_texture propagates the selected image's predicate through
select/phi; null read/query/write/sample protections remain project policy (MSL 2.1
does not define member calls on null textures). Live dynamic texel tokens stay
separate work.

Patch 0034 extends the derivative-group proof to unannotated uniform loop
induction and readonly constant-buffer data reached through logical or physical
pointers. Immutable address/length/access rows are uniform; writable data and
varying indices do not gain a uniform load proof. Cached lowering receives only
facts bound to the exact final SPIR-V and fragment entry identities.
Cache tag: rdna4x-19-readonly-uniform-control; postpass: rdna4x-argument-resources-20-uniform-control.

Patch 0035 guards GPU-loaded buffer addresses against the immutable registry of
declared buffers. Table version 3 preserves the 768-byte direct prefix, appends
a 32-byte header and uses 24-byte base/length/access rows from byte 800. Unknown
addresses, complete-range overflow and insufficient permissions fail the GPU
guard. Mixed pointers select direct bounds or registry validation. Vertex,
fragment and compute share this contract; loaded constant-space writes reject
translation. Direct bounded shaders retain table version 2.
Cache tag: rdna4x-20-buffer-registry-v3; postpass: rdna4x-argument-resources-21-buffer-registry-v3.

Patch 0036 integrates capability-gated LOD query replay with ABI 9. The product's
resources/lod_query modules share the fragment control/value proof, including
readonly buffer aliases and pure helper results. Actual subgroup properties
select quad broadcast or bounded uniform descriptor enumeration; both preserve
OpImageQueryLod and require a derivative-uniform dynamic instance. ABI 9 typed
dummy selection uses the null registry and binding 674 ordinal relocation.
Query-row shape tags remain independent of registry ordinals. Historical ABI 8
cached input retains its matching dummy calculation.
The matching wrapper uses C ABI 3 and adds capability-aware direct, linked and
cached exports while retaining ABI 2 signatures. Query input requires both actual
language/device flags and explicit Metal >= 2.2 language metadata (an extension
outside Metal 2.0/2.1). See garnetxlate/README.md for the exact 24-byte
capability record and cache policy.
Cache tag: rdna4x-21-abi9-query-replay; postpass: rdna4x-argument-resources-22-abi9-query-replay.
All six capability fields and the query policy participate in native cache identity.
StageResourceMetadata and legacy SamplerCoordinateFlags serialize only their exact
native binding schema.

Patch 0037 reuses each comparison sampler descriptor across repeated image
operations. The sampler-use pass updates its descriptor index when creating or
rebinding a representation, so subsequent uses see the same variable. This
prevents duplicate bindings that made the GPU argument postpass reject otherwise
valid mixed-use shaders. Direct, linked and cached postpass regressions preserve
both distinct comparison operations. Descriptor ambiguity checks stay strict.
Cache tag: rdna4x-22-shared-comparison-descriptors; postpass: rdna4x-argument-resources-23-shared-comparison-descriptors.
Reflection 65, resource-table ABI 9 and C ABI 3 are unchanged.

Patch 0038 adapts NullMoth's LGPL cube-array storage-coordinate lowering and
capability derivation. Writes combine the cube slice and face as slice*6+face,
use three integer coordinate components and require ImageCubeArray for storage
arrays. The macOS zero-LOD requirement remains enforced at the correct operand.
The ABI 9 shape validator now admits all six cube/cube-array float/uint/sint
storage forms, giving 60 legal shapes. MSL 2.1 sections 5.10.6/7 explicitly
permit these writes; storage depth, multisample storage and 3D arrays remain
invalid. The wrapper includes an authored indirect cube read/sample/gradient/
query/write regression through translation and cached postpass. Its shape registry,
texture creation and shape-matched dummy factory accept these forms and still
require actual storage-image format support.
Cache tag: rdna4x-23-cube-storage-shapes; postpass: rdna4x-argument-resources-24-cube-storage-shapes.

Patch 0039 adapts NullMoth's cube-array read operand parsing and implements the
sampled cube-array path that the reference rejects. Read lowering keeps face,
slice and LOD distinct. Sampled cubes use a texel-center direction and the existing
exact synthesized nearest/nearest-mip sampler; size query and sample both use the
read's explicit mip, including runtime LOD. Cube arrays append the cube slice as
the fourth coordinate. Storage cube arrays keep slice*6+face and reject nonzero
LOD at the correct operand. Fixed MSL 2.1 section 7.7 requires read coordinates
inside the texture; the special out-of-bounds defaults in section 2.8.1 apply to
texture buffers, not ordinary cubes. Read/cube logic now lives in read.rs and
cube_read.rs instead of growing the general write and fetch-coordinate modules.
Native stage layouts materialize the reflected SynthesizedReadSampler state and
relocate its descriptor for fragment stages; missing state is rejected instead of
falling back to an application sampler. Authored read/sample controls cover both
resources, all faces/slices, mip 0/1 and float/uint/sint.
Cache tag: rdna4x-24-cube-texel-reads; postpass: rdna4x-argument-resources-25-cube-texel-reads.

Patch 0040 retracts unused synthetic buffer-address table reads after resource
helper lowering, including the cached argument postpass. It preserves every
other value and keeps live address consumers rooted; opaque image/sampler
tokens no longer require BDA merely because a helper once carried their source
pointers. The authored noinline fragment-helper regression retains real sample
instructions and rejects the stale BufferAddressTable reflection entry.
Cache tag: rdna4x-25-live-resource-bindings; postpass: rdna4x-argument-resources-26-live-bindings.

Patch 0041 keeps cached-postpass sampler flag replacements in the block that
defines their token lookup. Separate uniform helper branches no longer share
an SSA value from only one arm. The direct/linked authored regression validates
both generated Grad samples with spirv-val.
Cache tag: rdna4x-26-sampler-flag-dominance; postpass: rdna4x-argument-resources-27-flag-dominance.

Patch 0042 places explicit derivatives at the nearest derivative-group-uniform
dominator when a branch-local coordinate can be recomputed from pure operations
and immutable stage inputs. Descriptor lookup, sampling, bias and side effects
stay at the original site; coordinates cannot move across loop instances.
Varying backedges invalidate the loop header's later dynamic instances, avoiding
an unsafe derivative placement there. Shared dependency graphs are walked
iteratively and copied once per definition, preserving result decorations.
The sampler pass and its control/image/placement tests now live in separate
files. Dynamic normalization dependencies, non-speculatable memory and broader
partial-exit cases still require work before full Tier 2 acceptance.
Cache tag: rdna4x-27-derivative-placement; postpass: rdna4x-argument-resources-28-derivative-placement.

Patch 0043 preserves the source spatial coordinate of implicit color, depth and
comparison samples independently from backend coordinate normalization, array
layers and offsets. Derivative placement therefore does not speculate descriptor
queries, sampler flags or layer-buffer loads. The source fact follows null-guard
sample renaming and canonical ID remapping. Wrapper source-proof version 2 binds
the additional facts to the exact SPIR-V hash and validates sample/coordinate
ownership and dominance; legacy version 1 remains valid for its previous facts.
Normalized nonuniform implicit sampling without its source coordinate is refused
instead of silently differentiating backend normalization. The wrapper's source
proof serialization now lives in its own module.
Cache tag: rdna4x-28-source-sample-coordinates; postpass: rdna4x-argument-resources-29-source-coordinates.

Patch 0044 validates cached GPU argument-table metadata against the finished
SPIR-V descriptor sets and comparison operations before taking the idempotent
postpass return. It rejects stale categories, table bindings, query bindings,
and comparison-sampler flags without changing the ABI or cache identity.

Patch 0045 extends targeted cursor repair to defined linked helpers. Selected
calls are expanded in AIR before emission so the buffer's 64-bit address,
runtime length and access permission stay attached to each memory operation.
Other calls and exported definitions remain intact. Declarations and recursive
call graphs are not expanded; the ordinary internal-only inliner is unchanged.
Cache tag: rdna4x-29-checked-helper-cursors; postpass: rdna4x-argument-resources-30-checked-helper-cursors.

Patch 0046 preserves explicit AIR indirect-buffer roots after nested resource
lowering removes opaque member descriptors. Reflection 66 adds sorted, validated
argument_buffer_roots; wrapper and native scanning retain them across direct,
linked and cached paths. Typed sparse scans stay sparse and ordinary storage
buffers are not inferred to be roots. Existing reflection 65 is readable without
the new field; the new cache identity regenerates lost roots.
Cache tag: rdna4x-30-declared-argument-roots; postpass: rdna4x-argument-resources-31-declared-argument-roots.

Patch 0047 permits pure GLSL.std.450 coordinate arithmetic, including Fma, to
be recomputed at a legal derivative-group-uniform dominator. Import identity
is checked and retained as a module dependency. Uniform-control analysis shares
the same arithmetic classification, preserving uniform math loop bounds without
relaxing varying-loop rejection. Pointer-output Modf/Frexp,
interpolation state and unknown instruction sets remain non-speculatable.
The descriptor/sample stays at its original site, and loop-instance checks
are unchanged. New regressions cover multiple fragment entries while preserving
vertex, compute and unreachable functions, plus direct/linked/cached Fma paths.
Cache tag: rdna4x-32-pure-math-uniformity; postpass: rdna4x-argument-resources-33-pure-math-uniformity.

Patch 0048 preserves readonly/dereferenceable constant-buffer parameter spans and
ordered-load flags in typed AIR. Only entry-rooted numeric reads with a bounded,
constant offset and proven alignment may move before a branch. Readonly alone,
nullable spans, helper-only promises, dynamic offsets, resource-pointer loads
and ordered reads remain guarded. This supplies dominating coordinate inputs
without weakening SPIR-V storage-load speculation or adding cached proof fields.
Cache tag: rdna4x-34-bounded-constant-paths; postpass: rdna4x-argument-resources-35-bounded-constant-paths.

Patch 0049 inlines ordinary descriptor-backed helpers whose source pointee view
differs from the caller's. One proven device/constant descriptor root and its
helper proxy share the raw byte layout; cloned loads/stores keep their types,
and argument offsets remain intact. Local/workgroup conflicts and ambiguous
roots retain their typed boundary. Root analysis is lazy and shared per caller.
This preserves entry accessibility across linked reinterpret helpers without
weakening the constant-read span or derivative-placement rules.
Cache tag: rdna4x-35-descriptor-helper-views; postpass: rdna4x-argument-resources-36-descriptor-helper-views.

Patch 0050 reconverges branch-protected numeric coordinate reads without an
unconditional dereferenceable span. Reads keep the original condition in a
guarded address/read phase; inactive paths supply zero at a new phi merge,
then the original sampling selection executes. Side effects and descriptor
operations retain their source sites. Single-predecessor arms, static entry
address paths and an effect-free prefix bound this transform. Existing CFG,
source-coordinate and loop-instance proofs remain required; unsupported cases
remain visible. Sampler depth/comparison ABI coordinate slots are distinct.
Cache tag: rdna4x-36-guarded-coordinate-reads; postpass: rdna4x-argument-resources-37-guarded-coordinate-reads.

Patch 0051 extends the guarded read phase to runtime GEP indices. Arm-local
integer address calculations and numeric entry-buffer read dependencies move
in source order under the original predicate, retaining 64-bit arithmetic and
the later checked BDA bounds/permission path. Unconditional read hoisting still
requires static offsets inside the explicit accessible span. Unknown pointer
roots, ordered reads and effectful prefixes remain excluded. Direct, actually
linked and repeated cached regression cases share the resulting CFG.
Before finalization drops dead descriptor views, readonly uniformity provenance
is copied to surviving aliases. This preserves constant-buffer early-return
predicates when another buffer uses checked dynamic addressing.
Cache tag: rdna4x-37-dynamic-coordinate-reads; postpass: rdna4x-argument-resources-38-dynamic-coordinate-reads.

Patch 0052 preserves ordinary numeric memory prefixes before guarded coordinate
reads. Every preceding numeric read and store moves together in source order
under the original predicate; stores execute once and read results return
through phis for later source uses. AIR numeric conversions may remain in the
guarded prefix. Typed memory carriers retain volatile/atomic ordering flags;
ordered operations, unknown calls and opaque prefix reads/stores remain
excluded. Descriptor selection and sampling keep their original arms. No new
serialized proof or numeric ABI field is added.
Cache tag: rdna4x-38-coordinate-memory-prefixes; postpass: rdna4x-argument-resources-39-coordinate-memory-prefixes.

Patch 0053 extends guarded coordinate planning across unconditional edges whose
successors have exactly one predecessor. Address calculations and ordinary memory
prefixes keep their order under the original predicate. Reads return through phis;
stores execute once. Original blocks, labels and sampling sites remain intact;
joins, conditional edges and cycles end the region. Existing memory, derivative
control and loop-instance proofs remain required.
Cache tag: rdna4x-39-coordinate-read-regions; postpass: rdna4x-argument-resources-40-coordinate-read-regions.

Patch 0054 reuses the typed single-predecessor phi canonicalizer before guarded
coordinate planning. Identity numeric values and pointers are substituted before
address-root tracing and prefix eligibility checks; joins, incomplete/conflicting
phis and self-references retain their normal analysis boundaries. No additional
phi-specific address walker or serialized proof is added.
Cache tag: rdna4x-40-coordinate-identities; postpass: rdna4x-argument-resources-41-coordinate-identities.

0055-depth-comparison-arrays.patch
  Includes sample_compare_depth in dynamic texture-array handle materialization
  and sampled-image discovery. Comparison-only argument arrays preserve their
  actual image and sampler instead of falling through to null-result protection.
  Authored depth-array regressions retain Dref and ConstOffset for implicit and
  level(0), ordinary 2D and arrayed depth images.
  Cache tag: rdna4x-41-depth-comparison-arrays; postpass: rdna4x-argument-resources-42-depth-comparison-arrays.

0056-depth-cube-read.patch
  Reuses the existing cube texel-center nearest read path for depth cube and
  depth cube-array reads. Preserves the depth AIR control prefix, runtime face,
  slice and mip, then reconstructs the scalar depth result. Avoids invalid
  ImageFetch on a sampled cube image; no new sampler algorithm is introduced.
  Cache tag: rdna4x-42-depth-cube-read; postpass: rdna4x-argument-resources-43-depth-cube-read.

0057-embedded-multisample-images.patch
  Preserves the declared multisample bit in fixed embedded textures and
  texture-handle arrays, including their materialized loads. The image type,
  Sample operand of read(), reflection and GPU token shape now agree instead
  of treating the sample index as a mip of an ordinary image. Authored scalar
  and dynamically indexed array regressions cover the metadata boundary.
  Cache tag: rdna4x-43-embedded-multisample; postpass: rdna4x-argument-resources-44-embedded-multisample.

0058-sampled-graph-reads.patch
  Accepts AIR sampled access=0 for legal read() operations reached through a
  GPU argument resource graph, including guarded null branches. Read access=1
  follows the same sampled-image path; write-only/unknown read classes and
  multisampled storage remain refused. Authored float/uint/sint, ordinary/MS,
  access=0/1 coverage validates each image fetch and preserves invalid classes.
  Cache tag: rdna4x-44-sampled-graph-reads; postpass: rdna4x-argument-resources-45-sampled-graph-reads.

0059-depth-null-predicates.patch
  Recognizes the actual AIR is_null_depth_* ABI family and shares null predicate
  recognition across native emission, resource discovery, typed image facts and
  graph lowering. Depth evidence remains explicit; ordinary textures keep their
  existing path. Authored direct/dynamic depth predicates cover 2D, arrays, MS,
  cube and cube arrays without undefined null member calls.
  Cache tag: rdna4x-45-depth-null-predicates; postpass: rdna4x-argument-resources-46-depth-null-predicates.

0060-dynamic-texel-argument-graphs.patch
  Lowers dynamic texture-buffer argument graphs to uniform/storage texel descriptor
  arrays under table ABI 10, fixes AIR texture-buffer read parsing so its final
  access selector is not mistaken for a mip LOD, and preserves the ABI 9 null-shape
  registry in ABI 10 metadata. Synthetic float read/write graphs validate as Vulkan
  1.2 SPIR-V and round-trip through the translator metadata type. Native ABI 10
  metadata, layout, and dynamic VkBufferView descriptor publication are wired in
  `src/garnet-metal/{rdna4_argument_tables.inc,rdna4_argument_table_commands.inc}`
  and gated via `rdna4ns_registry_build_abi`, while live RDNA4 GPU execution of
  ABI 10 dynamic texel graphs remains pending in Tier 2 verification.
  Cache tag: rdna4x-47-dynamic-texel; postpass: rdna4x-argument-resources-47-dynamic-texel.

0061-nested-buffer-element-paths.patch
  Preserves typed array/vector element paths when interface construction wraps a
  buffer parameter in a StorageBuffer block and runtime array. Keeps source
  parameter types available until those paths have been rewritten, and inserts
  both wrapper selectors for proven element-relative access chains. Synthetic
  direct and linked vertex tests validate SPIR-V and assert the exact constant
  and dynamic access paths, including the byte-20 nested-array scalar load.
  These tests do not establish that the desktop icon failure is fixed. The exact
  iconservicesagent request was subsequently captured and still refused after
  this patch; its separate helper address contract is corrected by patch 0062.
  Cache tag: rdna4x-48-nested-buffer-elements; postpass identity unchanged.

0062-constant-helper-device-addresses.patch
  Uses buffer-device-address representation for raw constant-space helper
  parameters selected by the large-buffer root analysis. The call argument,
  helper parameter type and helper load addressing now all carry the actual
  byte address. Unselected constant helpers retain their descriptor contract.
  Descriptor-only constant cursor conflict checks remain in place; checked
  argument-table bounds still require call-site inlining.
  Synthetic linked fixtures cover constant/device spaces, dynamic float strides
  and repeated helper calls at independent byte offsets. The CPU Vulkan replay
  additionally checks exact loaded values and input/output guards through linked
  and cached paths. The exact private
  iconservicesagent request also replays successfully with this correction;
  that translation result alone does not establish correct desktop pixels.
  Cache tag: rdna4x-49-constant-helper-addresses; postpass identity unchanged.

0063-widen-fragment-color-outputs.patch
  Widens 16-bit half scalar and vector fragment color outputs at location
  attachments to 32-bit float SPIR-V interface variables in
  `src/passes/stage_output/mod.rs` and emits `OpFConvert` on stores, with unit
  test `half_fragment_color_outputs_use_float_interface`.

0064-preserve-linked-static-samplers.patch
  Preserves and renames `!air.sampler_states` static sampler metadata globals
  when appending linked dependency modules in `src/linked_functions.rs` and
  exposes shared lookup helpers in `src/reflect/mod.rs`.

0065-do-not-cache-forward-select-edge-address.patch
  Stops caching latch-local forward BDA `OpSelect` IDs into
  `bda_direct_addresses` in `src/native/emitter/pointers/phi_merge.rs`, fixing
  loop-exit phi dominance regression
  `native_bda_forward_pointer_select_does_not_poison_loop_exit_phi` in
  `src/native/tests/buffers.rs`. Current cache tag:
  `rdna4x-51-forward-select-edge-addr`; postpass identity:
  `rdna4x-argument-resources-47-dynamic-texel`.
