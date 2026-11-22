# Metal 2 implementation baseline

Change: `complete-rdna4-metal2-large-memory`. Branch:
`feat/rdna4-metal2-large-memory`, starting at
`d77930b869707fe231ce23fd26feebf3cf21e765` plus the inherited migration worktree.
The commit alone does not reproduce this baseline.

Evidence root: `logs/rdna4-metal2/20261105-110230/`.

## Frozen inputs

`inherited-source.tar.gz` and `inherited-source.json` preserve all 1691 source
entries from the accepted delivery sidecar, including ignored build inputs.
Every entry matched the accepted sidecar before implementation started.
`inherited.patch` and `inherited-status.txt` record the inherited tracked and
untracked changes. No stash, reset or commit was used to create the branch.

The four display patches and their effective tree identity are frozen in
`final-patch-identity.json` and `source-identity.txt`. The production payload
inputs and component output hashes are in `accepted-MANIFEST.txt`,
`accepted-LOCAL-SOURCE-MANIFEST.txt` and `accepted-SHA256SUMS`.
`b1-identity.json` binds the isolated VM bundle to the production bundle and
records their intentional backend differences.

| Component | Fixed input |
| --- | --- |
| Display | RX4Darwin/AMDRadeonRDNA4.kext `7aa397b522e335ed5607f601a64e4a3a401dfa9a` plus patches 0001..0004 |
| RADV | Mesa `f5cb8ee032adabef599ae892ec4e42d796b84da9` plus `mesa-patches/0001..0015` |
| AIR translator | `43c46ac8a24adf1a6e872b8a52c706ec9614fad0` plus `third-party/metal2vulkan-rdna4/patches/` and the local wrapper |
| Production | RELEASE, VMTEST=0, PVGPU=0, local compute/VRAM owner |
| Isolated VM | The accepted VM backend, for software validation only |

The accepted delivery unit contains 748 regular checksum entries and two
loader symlinks; its extracted production bundle contains 707 regular files.
Their preceding verification and hashes are retained in
`logs/rdna4-refactor/amdradeonrdna4/20261105-migration/round-reverification/`.
Lilu retains its recorded unsigned exact-hash exception.

## Existing output contracts

`old-cases/` preserves the original named case TSVs, commands, modes, evidence
paths and B1 allowlist. `output-contracts.json` expands the 346 ordered PASS
lines into the original verifier's patterns. Only elapsed seconds and absolute
SPIR-V cache paths are normalized. `baseline-check.json` records a fresh
comparison of the preserved migration candidate outputs against these contracts:

| Suite | Required old result |
| --- | --- |
| compatibility | 47/47 |
| execution | 95/95 |
| cold | 18/18 |
| preparation | 8/8; preparation does not prove GPU execution |
| B1 | 43 per side; 26 identical, 17 permitted differences; 50 raw binaries |

This comparison reads preserved evidence. It does not claim a new GPU run.
New software acceptance must rerun the suites serially in one VM and retain
their pixel, compute, layout, ownership and rejection assertions.

## Intended changes to old capability expectations

Each expectation below changes only after its implementation and named new
execution oracle pass. These are the allowed upgrade categories, not permission
to weaken the old functional comparisons.

| Existing query or rejection | Intended result | Tasks and affected cases |
| --- | --- | --- |
| Shared non-WS VRAM cap = pool / 4 | Pool minus internal/PT reserve, WS occupancy and growth reserve | 2.1/2.2; native_g2, native_g4, Mesa budget/runtime, devcaps |
| Provisional pre-open buffer limit | Read-only negotiated device limit without opening RADV | 2.2/3.3; devcaps, query-only, capability queries |
| Logical buffer limited by 32-bit descriptor view | Large logical length and 64-bit BDA, bounded descriptor windows | 3.1..3.7; gpu-pointer, buffer-blit, native-buffer-route, new memory group |
| Per-client imports limited to 4 GiB | Independent bounded system-memory budget; retain 64 MiB per import and ownership | 3.5; NoCopy and host-import boundary cases |
| Argument buffers Tier 1 | Tier 2 after all-stage and GPU-produced argument execution | 5.1/5.2; argument layout/reflection/setters, sampler and argument dispatch |
| Tessellation or ICB request refused | Supported baseline patch and CPU/GPU ICB execution | 6.1..7.3; new tessellation and ICB groups, pipeline queries |
| Heap budget-only behavior and CB-granularity events | Actual reusable backing and event-position ordering | 4.1..4.3; heapalloc, command-completion, new synchronization cases |
| Format/workgroup/family reports | Reports match the fixed matrix and actual implementation | 8.1..8.3; format, device and B1 capability rows individually identified at integration |

The B1 allowlist is unchanged here. Updating a concrete query row later requires
its old value, new value and evidence, while retaining its other assertions.
The existing WS compatibility path remains separately identified.

### Null-descriptor fallback

Recorded 2026-11-06 after the isolated VM exposed a false negative in the GPU
argument-table feature gate. MoltenVK 1.4.2 reports
`nullDescriptor=false` on the Apple Paravirtual device while the shader-probe
still reports runtime descriptor arrays, partially bound update-after-bind
descriptors and non-uniform sampled/storage indexing. The extension advertisement
is not a feature guarantee. Wiring `gpuArgumentTables` to `nullDescriptor`
therefore disabled the entire argument-table path and made unrelated nested
resource tests fail.

`nullDescriptor` is not required for Tier 2 argument tables. When it is absent,
binding `VK_NULL_HANDLE` to a sampled-image slot is not a legal zero-return
descriptor. The ABI-4 fallback keeps the token tables and samples a valid
shape-matching dummy descriptor for a missing token, then selects an all-zero
result. Texel reads/writes are guarded before their descriptor load. This
preserves the shader's original sample/gather operands and implicit-LOD control
flow; it is not evidence that the unsupported null-descriptor feature itself is
implemented.

Two limitations remain tracked for later work. The probe also reports
`quadDivergentImplicitLod=false`, so a fragment shader whose image/sampler
selection diverges within a quad has undefined implicit-LOD behavior; the current
fallback does not claim to solve that. Dummy descriptors must also match the
shader's image dimensionality, arrayed/cube/depth form, so every newly admitted
sampled image shape needs a matching valid dummy (or a shader-side zero path).
Raw at
`logs/rdna4-metal2/20261106-null-sampling/feature-query.log` and
`logs/rdna4-metal2/20261106-null-sampling/vm-r1.log`.

The automatic-heap update changes the old tight-size assumptions in
`mtlprobe heap`: on the isolated VM, a 1000-byte buffer now reports a 1008-byte,
16-aligned placement extent while charging the actual 1000-byte Vulkan
requirement. A live small allocation can leave a 60 MiB unbacked range in a
64 MiB heap; the test allocates the reported range to verify that promise,
instead of asserting a contiguous budget-only remainder. Texture charges use
their actual requirements. A8 sampled heap textures now share the normal
texture implementation and pass a 4096-pixel alpha oracle; A8 render targets
remain rejected. The updated heap command passes all 51 checks, with its GPU
data and rejection checks retained. Evidence: the run's
`heaps/vm-final-mtlprobe-v7.log` and `heaps/mtlprobe-v7-source.json`.

## Physical work still pending

Migration tasks 7.1/7.2 and the stabilization change's hardware tasks remain
incomplete. This baseline does not prove attachment of the new display plugin,
native transport, same-boot integration or desktop stability on the RX 9060 XT.
Main change tasks 10.1..10.3 remain last and require later explicit release.
The separate `enable-rdna4-16g-bar` change is not a prerequisite for this work.
