# Native Metal implementation

`GarnetDevice.m` is the bundle entry point and the Metal device facade. It includes
private `*_impl.inc` modules in dependency order. Build it once; do not compile
the includes separately. This keeps private Objective-C classes and static
helpers available to the production implementation and the existing mock
fixtures without exporting a new runtime interface.

| Responsibility | Implementation |
| --- | --- |
| Lazy RADV loading, immutable limits, device open and pipeline cache | `rdna4_backend_impl.inc` |
| Memory pool, draw and import caches | `rdna4_resource_caches_impl.inc` |
| IOSurface ownership, scanout, presentation and diagnostics | `rdna4_surface_impl.inc` |
| QuartzCore 25G332 software import tracking compatibility | `rdna4_ca_sw_compat.h` |
| Version-independent CoreDisplay verification for `AMDGarnetPrimaryFramebuffer` | `rdna4_coredisplay_compat.h` |
| Temporary 25G332 desktop selection observations (read-only, UUID checked) | `rdna4_compositor_observation.h` |
| Atomic AIR dump publication and dump-directory policy | `rdna4_shader_dump_impl.inc` |
| Buffer backing, resource identity and shared heap declarations | `rdna4_buffer_impl.inc` |
| Shared command helpers and event objects | `rdna4_command_support_impl.inc` |
| Texture allocation, views, IOSurface import and CPU access | `rdna4_texture_impl.inc` |
| Texture-buffer views sharing Vulkan buffer storage | `rdna4_texture_buffer_impl.inc` |
| Heap capacity, placement, aliasing and resource creation | `rdna4_heap_impl.inc` |
| Sampler configuration, admission, shared handles and LOD bindings | `rdna4_sampler_runtime_impl.inc` |
| Depth/stencil state and fences | `rdna4_depth_fence_impl.inc` |
| Translation, linked functions, reflection, descriptor layouts and hot-swap | `rdna4_shader_runtime_impl.inc` |
| Render and compute pipelines | `rdna4_render_pipeline_impl.inc`, `rdna4_compute_pipeline_impl.inc` |
| Bound stage resources, argument scan preparation and dispatch regions | `rdna4_stage_bindings_impl.inc` |
| Queue scheduling, segment declarations and IOSurface command preparation | `rdna4_command_queue_impl.inc` |
| Deferred scanout copy when a display write initially has no reusable slot | `rdna4_display_late_impl.inc` |
| Bounded compositor input and accepted scanout pixel observation | `rdna4_surface_capture.inc`, `rdna4_present_capture.inc` |
| Committed display updates awaiting GPU submission | `rdna4_display_commits.h` |
| System Profiler association with the verified native accelerator | `rdna4_system_report_impl.inc` |
| Command recording, submission, completion and descriptor snapshots | `rdna4_command_buffer_impl.inc` |
| Render, compute and blit encoding | `rdna4_render_encoder_impl.inc`, `rdna4_compute_encoder_impl.inc`, `rdna4_blit_encoder_impl.inc` |
| Standalone OpenGL-on-Metal surface table and drawable bridge | `rdna4_gl_surface_pure.h`, `rdna4_gl_drawable_impl.inc` |

The existing argument includes keep their roles: `rdna4_argument_tables.inc`
owns table layouts, `rdna4_argument_pool.inc` owns descriptor pool budgets,
`rdna4_argument_table_commands.inc` materializes command descriptors, and
`rdna4_argument.inc` plus the summary includes manage argument encoders and
sampler-owner evidence. This split preserves their include positions and the
shader ABI. Cross-module dependencies are still private to one translation unit.

Sampler callers acquire and release canonical storage through
`rdna4_sampler_storage_acquire` and `rdna4_sampler_storage_release`. Those two
operations own map membership, reference counts, argument admission, reservation
rollback and last-public-owner release. Callers adapt Metal descriptors or LOD
overrides; they do not update storage counters. Handle creation retains the
existing budget rule `committed = actual + reserved - reservedActual`.

Tests that include `GarnetDevice.m` compile normally. Tests that extract or mutate
production source use `assemble-device.py` to expand only the private
`*_impl.inc` includes. Ordinary includes remain intact, so historical source
guards can compare the same implementation text. The compatibility runner's
source metadata already includes every top-level `.inc` file.
