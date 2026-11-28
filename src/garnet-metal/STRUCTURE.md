# Native Metal implementation

`GarnetMetal.bundle` is built from 32 independent Objective-C compilation units
compiled in parallel with `-DGARNET_METAL_MODULAR_BUILD=1 -fvisibility=hidden
-Wall -Wextra -Werror` (see `build.sh`). Shared internal declarations live in
`garnet_metal_internal.h`, with subsystem-private declarations in
`garnet_resources_internal.h`, `garnet_shaders_internal.h`,
`garnet_pipelines_internal.h`, `garnet_commands_internal.h`,
`garnet_arguments_internal.h`, `garnet_icb_internal.h`, `garnet_io_internal.h`,
`garnet_sparse_internal.h`, `garnet_rt_internal.h`, and `garnet_mesh_internal.h`.

`GarnetDevice.m` is a thin single-TU compatibility bridge (`#ifndef
GARNET_METAL_MODULAR_BUILD`) that includes `garnet_metal_internal.h` and the 32
`garnet_*.m` units in dependency order so existing mock fixtures that `#include
"GarnetDevice.m"` continue to compile without exporting a new runtime ABI.

| Responsibility | Compilation unit |
| --- | --- |
| Admission gates, immutable limits, lazy RADV open, Vulkan dispatch, pipeline cache | `garnet_backend.m` |
| Memory pool, draw/import caches, `RDNA4HeapBacking`, `RDNA4Buffer` | `garnet_resources.m` |
| `RDNA4Texture`, linear/IOSurface-backed textures, vram surface binding | `garnet_textures.m` |
| `RDNA4Heap` automatic and placement heaps, retired-block eviction | `garnet_heaps.m` |
| `RDNA4Sampler*`, `RDNA4DepthStencilState`, `RDNA4Fence*`, `RDNA4ResidencySet` | `garnet_states_residency.m` |
| IOSurface ledger, scanout D-copy/HDMI routing, late-present, surface/present capture | `garnet_surface.m` |
| Atomic AIR dumps, in-process shader translation, linked functions | `garnet_shaders.m` |
| `spvcache` sidecar lookup, reflection metadata, hot-swap watcher (`RDNA4HSEntry`) | `garnet_spvcache.m` |
| `MTLBinaryArchive` (`RDNA4BinaryArchive`) and Vulkan pipeline cache persistence | `garnet_binary_archive.m` |
| `MTLDynamicLibrary` (`RDNA4DynamicLibrary`), `RDNA4FunctionHandle`, `RDNA4VisibleFunctionTable` | `garnet_dynlib_vft.m` |
| Render pipeline state (`RDNA4RenderPipelineState`), GPL fast-link, pre-warm | `garnet_pipelines.m` |
| Compute pipeline state (`RDNA4ComputePipelineState`), heavy compile guard, ICB variants | `garnet_compute_pipelines.m` |
| Shared encoder resource bindings (`RDNA4ResourceBindings`), descriptor filling, dummy resources | `garnet_pipeline_bindings.m` |
| Format table, `RDNA4CommandSegment`, `RDNA4CommandQueue` | `garnet_commands.m` |
| `RDNA4CommandBuffer`, fence wait, submission, IOSurface write-back, resource tracking | `garnet_command_buffer.m` |
| `RDNA4Counter`, `RDNA4CounterSet`, `RDNA4CounterSampleBuffer`, timestamp sampling | `garnet_counters.m` |
| `RDNA4RenderEncoder`, dynamic state emission, render passes and draw paths | `garnet_render_encoder.m` |
| `RDNA4ComputeEncoder`, `RDNA4BlitEncoder`, dispatch and copy/blit operations | `garnet_compute_blit_encoder.m` |
| `RDNA4ArgumentPool` and `RDNA4CommandBuffer` argument preparation | `garnet_arguments.m` |
| Argument table layouts, reflection stitching, incremental scanning, `RDNA4ArgumentEncoder` | `garnet_argument_encoder.m` |
| GPU argument summary task/attempt runtime and Vulkan compute encode bridge | `garnet_argument_summary.m` |
| Argument summary sampler-owner capture and epoch-ordered publication | `garnet_argument_summary_owners.m` |
| Indirect command buffers (render & compute CPU/GPU paths, reset/copy transactions) | `garnet_icb.m` |
| Indirect command execution on render, compute, and blit encoders | `garnet_icb_execute.m` |
| Metal 3 Fast Resource Loading (`RDNA4IOFileHandle`, `RDNA4IOCommandQueue`, `RDNA4IOCommandBuffer`) | `garnet_io_queue.m` |
| Metal 3 sparse/PRT backing pools, heaps, textures, and `RDNA4ResourceStateEncoder` | `garnet_sparse.m` |
| Ray tracing IFT, argument-buffer BDA, pipeline handles, and capability gates | `garnet_rt_runtime.m` |
| Ray tracing VK dispatch, compute pipes, staging/query pools, AS storage/handles, unit-box proxy | `garnet_rt_accel.m` |
| Ray tracing AS descriptor validation, build/refit plans, and BLAS/TLAS sidecars | `garnet_rt_plan.m` |
| Ray tracing acceleration structure command encoder and device/heap AS factories | `garnet_rt_encoder.m` |
| Mesh and object shader pipelines, encoder bindings, and emulated/native draw paths | `garnet_mesh.m` |
| `GarnetDevice` facade, System Profiler registration, GL-on-Metal drawable bridge | `garnet_device.m` |

Single-TU headers with process-local compatibility state remain included only by
the compilation unit that owns them (`garnet_ca_sw_compat.h`,
`garnet_coredisplay_compat.h`, `garnet_census.h` in `garnet_device.m`;
`garnet_stitch_reflection.h` in `garnet_argument_encoder.m`;
`garnet_icb_expand.h` in `garnet_icb.m`;
`garnet_heap_backing.h` in `garnet_heaps.m`; `garnet_residency_set.h` in
`garnet_states_residency.m`; `garnet_sparse.h` in `garnet_sparse.m`;
`garnet_mesh.h` in `garnet_mesh.m`).

Sampler callers acquire and release canonical storage through
`rdna4_sampler_storage_acquire` and `rdna4_sampler_storage_release`. Those two
operations own map membership, reference counts, argument admission, reservation
rollback and last-public-owner release. Callers adapt Metal descriptors or LOD
overrides; they do not update storage counters. Handle creation retains the
existing budget rule `committed = actual + reserved - reservedActual`.

Tests, fixtures, and runners live under `src/garnet-metal/tests/`:
- `make -C src/garnet-metal/tests all` builds and runs the offline C11 host
  suites (ASan + UBSan), assembled-device source-pin tests, and Python/shell
  checks without a GPU.
- `run-metal-test.sh` builds and runs standalone macOS `.m` mock/VM fixtures.
- `run-compatibility.py` drives the Tier 2 compatibility manifest
  (`tier2-cases.json`).
- Tests that include `GarnetDevice.m` compile normally via the single-TU bridge;
  tests that extract or inspect full production source use `assemble-device.py`
  to expand `garnet_*_internal.h` and `garnet_*.m` (plus legacy `*_impl.inc`
  fixtures in `tests/test-validation-runner.py`).
