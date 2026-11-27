# Native Metal implementation

`GarnetMetal.bundle` is built from 11 independent Objective-C compilation units
compiled in parallel with `-DGARNET_METAL_MODULAR_BUILD=1 -fvisibility=hidden
-Wall -Wextra -Werror` (see `build.sh`). Shared internal declarations live in
`garnet_metal_internal.h`.

`GarnetDevice.m` is a thin single-TU compatibility bridge (`#ifndef
GARNET_METAL_MODULAR_BUILD`) that includes `garnet_metal_internal.h` and the 11
`garnet_*.m` units in dependency order so existing mock fixtures that `#include
"GarnetDevice.m"` continue to compile without exporting a new runtime ABI.

| Responsibility | Compilation unit |
| --- | --- |
| Admission gates, immutable limits, lazy RADV open, Vulkan dispatch, pipeline cache | `garnet_backend.m` |
| Memory pool, draw/import caches, buffers, textures, heaps, samplers, fences, residency sets | `garnet_resources.m` |
| IOSurface ledger, scanout D-copy/HDMI routing, late-present, surface/present capture | `garnet_surface.m` |
| Atomic AIR dumps, in-process shader translation, linked functions, binary archives, dynlibs/VFT | `garnet_shaders.m` |
| Render/compute pipelines, stage bindings, argument scan prep, encoder bindings | `garnet_pipelines.m` |
| Format table, command queues, command buffers, counters, render/compute/blit encoders | `garnet_commands.m` |
| Argument table layouts, descriptor pools, command materialization, argument encoders, summary | `garnet_arguments.m` |
| Indirect command buffers (render & compute CPU/GPU paths, reset/copy transactions) | `garnet_icb.m` |
| Metal 3 IO command queues and sparse/PRT heaps, textures, and resource-state encoders | `garnet_sparse_io.m` |
| Ray tracing acceleration structures, IFT/runtime, and mesh/object shader pipelines | `garnet_mesh_rt.m` |
| `GarnetDevice` facade, System Profiler registration, GL-on-Metal drawable bridge | `garnet_device.m` |

Single-TU headers with process-local compatibility state remain included only by
the compilation unit that owns them (`garnet_ca_sw_compat.h`,
`garnet_coredisplay_compat.h`,
`garnet_census.h` in `garnet_device.m`; `garnet_stitch_reflection.h` in
`garnet_arguments.m`; `garnet_icb_expand.h` in `garnet_icb.m`).

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
  to expand `garnet_metal_internal.h` and `garnet_*.m` (plus legacy `*_impl.inc`
  fixtures in `tests/test-validation-runner.py`).
