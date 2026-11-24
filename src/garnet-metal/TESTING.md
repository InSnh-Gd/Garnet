# Metal compatibility checks

## Contents

- [Verification tiers](#verification-tiers)
- [Compositor and WindowServer diagnostics](#compositor-and-windowserver-diagnostics)
- [Texture transfer bounds, device profile and log budget](#texture-transfer-bounds-device-profile-and-log-budget)
- [Compatibility runner and Tier 2 execution manifest](#compatibility-runner-and-tier-2-execution-manifest)
- [Texture buffers and CPU Vulkan shader replay](#texture-buffers-and-cpu-vulkan-shader-replay)
- [Quad controls, cold fallbacks and surface/present observation](#quad-controls-cold-fallbacks-and-surfacepresent-observation)
- [Broader VM comparisons, automatic heaps and live target mode](#broader-vm-comparisons-automatic-heaps-and-live-target-mode)

## Verification tiers

Results in this suite fall into separate execution classes (`host-cpu`, `host-mock`,
`offline-metadata`, `layout-only`, `capture-only`, VM software/MoltenVK GPU, and `live`
target hardware). Unless a section below explicitly specifies `live` target mode, host,
mock, simulated Vulkan, offline SPIR-V, capture-only and VM software/MoltenVK checks
validate CPU invariants, reflection, metadata, or guest lowering only; they do not
establish physical RDNA4 GPU execution, target timing, native scanout, or complete Tier 2
acceptance.

All new compiler commands retain `-Wall -Wextra -Werror` and x86_64 output; fixtures
that include the driver retain their framework links. No warning suppression is added
for these selectors. Source and fixture hashes describe the current files; archived
results retain their original source/bundle identities. When changing heap or segment
lifecycle fixtures, test both `-O0` and `-O2`: temporary Objective-C owners are drained
before weak-reference/accounting assertions, and C factories used by weak lifetime
checks declare retained returns.

---

## Compositor and WindowServer diagnostics

`test-compositor-observation.m` checks the temporary `[DEBUG-render]` observer
against the exact 25G332 SkyLight image: UUID rejection, guarded memory reads
and cached policy fields. Compile it with Foundation in the existing x86_64
VM. It must print `COMPOSITOR_OBSERVATION_PASS gpu_execution=0
server_callbacks_tested=0`. It does not call the WindowServer-only CoreDisplay
accessors. In the real WindowServer, the observer records the cached policy and
main display accelerator on the main queue at 3 and 15 seconds after device creation.
It changes no policy or framework memory and submits no GPU work. Remove it after
diagnosing desktop selection; its records are temporary evidence, not a rendering fix.

`test-ca-sw-flatten.m` separately diagnoses the QuartzCore software compositor
assertion seen at desktop login on macOS 25G332. It loads no RDNA4 bundle and
does no GPU work. Compile it with Foundation, QuartzCore and CoreGraphics in
the existing x86_64 VM. On the exact supported QuartzCore image, `--raw` must
abort at `push_surface`, `ogl-context.cpp:1186`; `--compat` must print
`SW_FLATTEN_COMPLETED --compat contexts=16 pixel_tolerance=1` and exit zero.
The `--plain` control must also exit zero. The compatibility run checks all
pixels against a plain-rendered control while creating and destroying sixteen
flattening contexts; one LSB covers offscreen blend rounding. Unknown images
and installation errors are unsupported or failing results, never PASS.

`test-coredisplay-compat.m` checks the version-independent CoreDisplay
compatibility guard without mutating process-local dyld state. Compile with
Foundation and IOKit and verify that `rdna4_cd_bound_primary(0)` returns 0,
`rdna4_coredisplay_compat(0)` is a safe no-op outside `WindowServer`, and
`CoreDisplay_UseIOPresentment` (when loaded) is unchanged. The identity and
actual display-pipe lookup require physical validation.

`test-ca-icons.m` renders four AppKit system-symbol images through a real
`CARenderer` into a Metal texture, checking all 65,536 pixels on the first frame
and two warm frames. Compile with AppKit, Foundation, QuartzCore and Metal;
run as root with `--reference OUTPUT_DIRECTORY`, then
`VM_TEST_BUNDLE OUTPUT_DIRECTORY` in the analysis VM. It reports frame time
separately from pixel correctness. This covers icon composition through the
bundle; the VM backend does not establish physical Settings loading latency,
application admission or native display output.

Four fragment translations captured from boot
`2A63AC8F-9491-4C2A-97FC-D37CEBADF446` are bundled with their metadata to avoid
the five observed no-write placeholders (one shader was requested twice).
Their import provenance and Vulkan 1.2 validation are under
`logs/rdna4-hardware-test/20261110-settings-icons/`. Unseen shaders still use
the existing translation path.

## Texture transfer bounds, device profile and log budget

### Texture transfer boundaries

Run the production CPU/backing and blit validation fixtures in the isolated
x86_64 macOS VM; neither fixture opens a GPU or changes the desktop:

```sh
bash src/garnet-metal/test-texture-transfer-bounds.sh \
  /path/to/vulkan/include /path/to/new-texture-results
bash src/garnet-metal/test-blit-buffer-layout.sh \
  /path/to/vulkan/include /path/to/new-blit-results
```

The texture suite runs 20 isolated cases. Invalid buffer extents, relocated
Vulkan offsets, overflowing CPU row/image pitches and truncated or
format-specific image geometry must fail before allocation or copying.
Guarded CPU mappings catch accidental accesses. Controls check exact backing
extents without final-row padding, format limits, partial host transfers and
the unused row stride of a single-row transfer.

The blit suite captures the production encoder's Vulkan commands in 26 cases
covering both buffer/texture directions. Invalid ranges, nonrepresentable row
and image strides, relocated-offset overflow, ended encoders and prior command
errors must record no copy or argument write intent. Tight, padded and existing
default-row handling have explicit `VkBufferImageCopy` controls (`gpu_execution=0`).

The shared span arithmetic also has an independent 128-bit host oracle:

```sh
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -O1 \
  src/garnet-metal/test-texture-transfer.c -o /path/to/test-texture-transfer
/path/to/test-texture-transfer
```

It checks 28,800 layout combinations, unchanged output on rejection and three
pointer-overflow edges. Pointer arithmetic validation does not establish that
arbitrary caller memory is mapped.

### Device profile and runtime log volume

Run the device identity fixture to check the Metal device fields consumed by
System Information without opening RADV or requiring a PCI device:

```sh
bash src/garnet-metal/test-device-identity.sh \
  /path/to/vulkan/include /path/to/new-device-profile-results
```

It checks the registry ID, discrete PCIe location, slot number, AMD vendor and
Gen5 x16 theoretical transfer rate from simulated IOKit data, alongside the
existing stable name and memory-snapshot cases. The PCI rate is a theoretical
host-to-VRAM link bound, not a measured copy rate. It cannot verify the live
System Information report or kext-provider label.

On macOS, the system report fixture runs the real `SPDisplaysReporter` with
mocked PCI and accelerator registry entries, without creating GPU resources:

```sh
clang -fobjc-arc -Wall -Wextra -Werror \
  -framework Foundation -framework Metal -framework IOKit \
  src/garnet-metal/test-system-report.m -o "$TMPDIR/test-system-report"
"$TMPDIR/test-system-report"
"$TMPDIR/test-system-report" --late-load
```

It checks both reporter load orders, the RX 9060 XT model, Metal family and
accelerator owner for the Garnet framebuffer, and that unrelated devices are
left alone. It does not validate live registry enumeration or Metal execution.

The resource creation fixture measures production CPU paths and captures
their log volume with simulated Vulkan:

```sh
bash src/garnet-metal/test-resource-log-budget.sh \
  /path/to/vulkan/include /path/to/new-log-results
```

Successful per-resource diagnostics are rate-limited; refusals and errors
remain unsampled. The test creates and retires 4,096 Shared buffers, then
checks 32 refusals still emit diagnostics. It verifies bounded logging and
cleanup, not WindowServer latency or GPU throughput.

## Compatibility runner and Tier 2 execution manifest

Run the existing tests serially in the isolated macOS VM, as root:

```sh
sudo env TMPDIR=/path/to/scratch python3 src/garnet-metal/run-compatibility.py \
  vm /path/to/GarnetMetal.bundle /path/to/vulkan/include /path/to/new-results
```

The output directory must be new. It contains source/bundle hashes, per-command
logs and `results.tsv`. Compilation errors, failed children, missing result
markers, timeouts and skipped cases make the suite exit nonzero. `SKIP` remains
separate from `FAIL`. Use repeated `--only sampler`, `--only arguments`, etc. to
run affected groups. Host checks for the runner need no GPU:

```sh
PYTHONDONTWRITEBYTECODE=1 python3 src/garnet-metal/test-validation-runner.py
```

`tier2-cases.json` is the selectable Tier 2 execution contract. Each entry fixes
the matrix IDs, shader stages, language, storage, operation, oracle, reference
and candidate domains, and backend gap. List the cases without Metal or root:

```sh
python3 src/garnet-metal/run-compatibility.py --list-cases
```

Select one entry, including both of its declared sides:

```sh
sudo env TMPDIR=/path/to/scratch python3 \
  src/garnet-metal/run-compatibility.py vm /path/to/GarnetMetal.bundle \
  /path/to/vulkan/include /path/to/new-results --case A02.resource-arrays
```

`--case` is repeatable and selects only the exact fixture invocations named by
the manifest; required builds still run. The output records the manifest hash,
the selection and the matched runner cases. A selected side that never reaches
its invocation is a `TIER2_SELECTION_MISSING` failure. Every declared pattern
must match an invocation; matching one pattern cannot hide another missing
pattern on the same side. A child that misses its marker is a `FAIL`. Successful capture-only
runs are recorded as `CAPTURE`, `SKIP` is separate, and neither is a GPU PASS.

The manifest records each side's execution class (`host-cpu`, `host-mock`,
`offline-metadata`, `layout-only`, `capture-only`) in `results.tsv`. A missing
backend feature or rejected resource shape remains a failing or explicitly
skipped invocation, and either makes the suite exit nonzero. Source metadata
includes C fixtures as well as Objective-C, headers, includes and Python.

### Additional Tier 2 selectors

Use `--case` to select one contract, or `--only` to run a complete group. The
new groups remain outside the default compatibility selection.

| Group | Manifest case | Execution and oracle |
| --- | --- | --- |
| `sampler-patterns` | `T03.sampler-patterns-gpu`, `T03.sampler-anisotropy-gpu` | Paired GPU direct/argument/constexpr checks: 28 filter/address/border/coordinate/mip/LOD patterns and five anisotropy states on two resources, with argument/input/output guards. |
| `sampler-binding` | `T03.sampler-binding-state`, `T03.sampler-binding-oracle-host` | Mock production sampler state, LOD setter and invalid-request checks; separate CPU pattern arithmetic. |
| `argument-fragment-flow` | `T02.fragment-flow.*` | Thirty-five individually selectable paired GPU control-flow checks, including 8/16-wide images, normalized/pixel sampler selection, branch-local array layers, bounded and guarded constant-buffer scales, runtime 64-bit scale indices, per-pixel memory effects, pointer-view helpers, structured exits and uniform-loop instances. Every invocation has four draws with exact target/argument/control guards. Recorded VM reference mip failures remain FAIL. |
| `argument-gather-cube` | `T02.gather-cube` | Paired GPU cube/cube-array ordinary, depth, comparison and integer gathers; base-mip 1 views, exact BL/BR/TR/TL order, data-dependent Less results and 11 entries/88 bytes. |
| `argument-image-operations` | `T02.image-operations` | Paired GPU 29-case/58-selection image read/query/sample/write oracle, including nonzero coordinates/layers/faces/mips/samples, typed multisample, Private depth staging and all unselected-texel guards. |
| `gather-capability` | `T02.gather-capability-host`, `T02.gather-offset-limits-host` | Mock stage rejection before layout allocation; separate 17-check CPU feature/offset-bound test. Core single `ConstOffset` stays available without the extended feature. |
| `null-runtime` | `PROJECT.null-runtime-host` | Mock ABI9 metadata/layout acceptance, static/no672, both normalized flags, strict 384-byte shape and fixed mixed image/texel bindings. |
| `null-shape` | `PROJECT.null-shape-host` | CPU shape words, canonical registry ordinals, full legal shape matrix, wire capacity and legacy ABI rejection. |
| `null-remap` | `PROJECT.null-remap-host` | CPU cross-stage image/texel ordinal remapping, in-place operation and malformed/missing-shape rejection. |
| `null-shape-reflection` | `PROJECT.null-shape-reflection` | Offline reflection of the checked-in valid and malformed SPIR-V fixtures; no GPU work. |
| `summary-event-progress` | `A04.summary-event-progress` | Candidate actual GPU lifecycle: eight same-command-buffer event-blocked summary rounds, old source/owner release, independent producer progress and final owner retirement. No translator/ABI8 claim. |
| `summary-plan` | `A04.summary-plan-host` | CPU chunk/page/range planning, including logical sources above 4 GiB. |
| `summary-owners` | `A04.summary-owners-host` | CPU accepted-epoch order, reverse queues, host-generation and completed-owner publication. |
| `summary-runtime` | `A04.summary-runtime-host` | Simulated pipeline creation, rollback/retry and dispatch partitioning; its `.m` source deliberately compiles as plain C. |
| `writer-epochs-host` | `A04.writer-epochs-host` | Nine CPU range/order/abandonment/copy-visibility cases; separate from the paired GPU `writer-epochs` group. |
| `argument-scan-host` | `A04.argument-scan-plan-host` | CPU reflected-root scan ranges, sparse logical roots above 64 MiB and bounded fallback; separate from `argument-scan-runtime`. |
| `argument-capacity` | `A02.argument-capacity-host` | CPU descriptor/token/pool/sampler object arithmetic with overflow and queried-limit rejection. |
| `argument-public-sampler` | `T03.public-sampler-admission-host` | CPU public sampler limits, alias admission and object-budget boundaries. |
| `argument-pool` | `A02.argument-pool-host` | Mock Vulkan global pool budgets, creation rollback and destruction accounting. |
| `argument-limit-snapshot` | `A02.limit-snapshot-host` | Synthetic provider built as a dynamic library; immutable snapshot, real loader round trip, same-handle re-export and source identity. |
| `argument-limit-snapshot-device` | `A02.limit-snapshot-device-host` | Mock device getter/fallback reads with no Vulkan objects or native device open. |
| `snapshot-admission` | `T03.snapshot-admission-host` | Mock production sampler admission at last/one-over limits, aliases, pixel reserve, LOD variants and rollback. |
| `argument-contract` | `A05.argument-descriptor-contract` | Candidate GPU descriptor-created encoder, sparse IDs/array destination, sixteen output words and seven atomic invalid-encoding checks. |
| `argument-resources` | `A02.argument-resource-values` | Paired GPU four-quadrant/sixteen-channel sampling with exact values 1..16, argument/output guards and indirect texture lifetime. |
| `large-buffer-addressing` | `A03.large-addressing-small-backing` | Opt-in, live-only paired GPU eight-case/sixteen-dispatch 64-bit addressing check on 256 KiB backing. Explicitly reports `actual_over_4g=0`. |
| `large-memory-over4g` | `A03.physical-over4g-private`, `A03.physical-over4g-shared`, `A03.physical-over4g-managed` | Opt-in, live-only candidate 8 GiB real-backing memory oracles with payload/guard checks and boundary RMW probes. |

### Group behavior and case notes

**Multisample & direct reads.** `A02.multisample-color-2d_ms-{0,1,2}` and
`A02.multisample-depth-2d_ms-0` reuse the typed-image fixture. Scissor and
fragment sample-mask outputs populate every pixel/sample with the original
independent pattern, avoiding a dependency on the VM host's sample-position
intrinsic. The read still selects sample 2 at a nonzero texel from each of two
images, and checks typed values, dimensions, sample count and all guards.
`A02.nested-ms-query-copy` separately checks width/height/sample count through
GPU-copied nested arguments; its VM reference remains capture-only because the
host cannot lower that nested access.

`T02.direct-ms-read-*` reuses the same population and exact sample-2 oracle
with `--direct-ms-read --operation read`. It rebinds texture slot 0 to each
image and checks that every unselected output word remains untouched.
`T02.multisample-binding-host` separately exercises production SPIR-V reflection
and descriptor admission: twelve single/MS type combinations must publish only
matching descriptors.

`T02.multisample-query-only-*` and `T02.direct-ms-query-controls-*` retain the
query-only and mixed direct-read/argument-query controls. On the analysis VM,
the integer mixed cases fail width 4/1 on both endpoints; the integer query-only
cases pass on the candidate and fail on the reference. Valid token lookup and
a successful direct-image query probe localize the mixed failure to descriptor
array queries in that VM. These cases remain FAIL and keep the original oracle.

**Null guards & null shapes.** `A02.null-guard-*` adds defined null checks to every typed-image shape. Each
case runs eight dispatches: Shared/Private argument transport, null slot 0/1
and selected slot 0/1. A missing source writes fixed branch sentinels; a live
source reads the original typed texel and dimensions. Writable shapes use the
opposite null/live destination pattern and check every texel after the guarded
write. Whole argument bytes, output guards and selection guards remain intact.
No member is called on a null texture. These cases are separate from the
existing project-only zero-read and discard-write compatibility tests.

The 0059 analysis candidate passes all 21 non-cube cases on both endpoints and
the two depth cube cases on the candidate. Eight cube sources compile/capture
on each endpoint; color cube storage execution and the depth cube references
remain unexecuted because of previously observed VM lowering failures. Capture
does not count as GPU PASS. The native log confirms `nullDescriptor=false` and
typed dummy use; this does not complete task 4.1, including texel buffers and
dual-use comparison dummies.

`A02.null-sampling-depth-*-{never,less,always}` extends the defined null guards
with ordinary and comparison samples using the same valid sampler token.
Each live branch checks the original exact depth value and the comparison at
reference 0.25; null branches emit fixed sentinels without member calls. The
eight Shared/Private/null-slot/resource-selection dispatches and all guards
remain. The analysis VM passes 2D/array on both endpoints and cube/cube-array
on the candidate. Cube references remain compile/capture evidence only.

The null reflection runner consumes prebuilt `fixtures/null-shapes-abi9.spv`
and `fixtures/null-shapes-invalid.spv`; it needs no assembler at runtime. To
regenerate them during development from the checked-in assembly:

```sh
cd src/garnet-metal
spirv-as --target-env vulkan1.2 fixtures/null-shapes-abi9.spvasm \
  -o fixtures/null-shapes-abi9.spv
spirv-as --target-env vulkan1.2 fixtures/null-shapes-invalid.spvasm \
  -o fixtures/null-shapes-invalid.spv
spirv-val --target-env vulkan1.2 fixtures/null-shapes-abi9.spv
```

The valid fixture passes Vulkan 1.2 validation. The malformed multisample-1D
fixture intentionally fails validation (including missing `Sampled1D`
capability); the reflection test checks its invalid-shape flag. Generator
commands, assembly/binary hashes and the 15-check parser result are recorded
in `logs/rdna4-metal2/20261106-tier2-implementation/native/integration-r20/fixture-generation.json`.

**Samplers, arguments, scanning and fragment flow.**
The sampler patterns share `test-sampler-pattern-oracle.h` with the host fixture.
Nearest filtering selects `floor(u * size)` for normalized coordinates and
`floor(u)` for pixel coordinates; linear filtering includes the half-texel
offset. Nearest mip controls avoid halfway ties. Anisotropy uses a box-filtered
stripe mip chain and an explicit 8:1 gradient footprint: isotropic sampling must
produce the known mip-3 average, anisotropy 8 must recover the stripe, and direct,
argument and constexpr paths must agree. The test does not assume a portable
anisotropic tap layout. These selectors cover this change's shared T03 sampler
state; the parent texture-format matrix remains a separate requirement.

`--only sampler-usage` builds the mixed normal/comparison sampler fixture, the
mocked-Vulkan runtime check, and the host-only sampler-state checks. Both the
system-Metal reference and the bundle execute the full fixture by default and
must print `ARGUMENT_SAMPLER_USAGE_PASS`. `--capture-only` records AIR without a
GPU verdict (`CAPTURE`, not PASS). The runtime check prints
`SAMPLER_RUNTIME_PASS ... simulated_vulkan=1 gpu_execution=0`; the state
executable prints `sampler-state:` checks.

The sampler test accepts `--reference` or `BUNDLE [--vm|--live]`; the default
bundle mode is VM/mock. It checks direct texture/sampler binding, texture-only
argument binding, and complete argument buffer dispatch. RDNA4 drops its
application resource references before committing; the reference case keeps its
sampler alive as an output oracle.

`--only sampler-scope` checks RDNA4 sampler ownership on overwrite, nil,
cross-layout writes, rejected batches and buffer release (RDNA4-specific weak-object
assertions, not an Apple object-lifetime contract). Paired GPU cases compare CPU
byte copies, CPU updates after a GPU write, and nonzero-offset blits to Private
arguments across command buffers with Repeat/Clamp outputs, guards and 128
unrelated samplers (the 128-decoy requirement stands even when the analysis VM's
sampler limit is smaller).

`--only argument-scan-runtime` also checks eight declared-child scopes: BUFFER,
UNKNOWN and dynamic reflection admit eligible unbound Shared/Managed transports,
while typed-only reflection retains sparse scanning. Private children contribute
owner metadata without reading raw token bytes. Aliasable, foreign-device and
undeclared buffers, plus unrelated live samplers, stay outside the scope. The
fixture exercises the production heap declaration expansion (`gpu_execution=0`).
Reflection 66 preserves explicit `air.indirect_buffer` roots as
`argument_buffer_roots`, including nested records without static field metadata.
The scan fixture rejects malformed, duplicate, unmatched and old-version roots;
an opaque root receives the conservative span while a typed root stays sparse.
`argument-nested` checks the resulting sampler identity in compute and render
consumers with Private copies and exact output guards.

`--only argument-arrays` runs descriptor capability/metadata rejection checks,
then compares runtime texture, sampler and buffer-pointer array selections with
system Metal (128 compute outputs and 32 rendered pixels per backend, with
guards; vertex resource choices also control geometry visibility). It does not
cover GPU-generated resource handles.

`--only argument-nested` also executes unguarded null texture reads/writes for
2D, integer, 3D and array textures, an empty resource table, and flat argument
arrays through the static postpass. Sampling and gather cover 2D/3D/array/cube
shapes with field and constexpr samplers; fragment sampling checks mip selection
and bias. Backends without `nullDescriptor` use shape-specific dummy images and
shader-side zero reads or discarded writes for the verified cases. It copies
arguments to Private storage before dispatch and checks output guards. VM
reference captures report `gpu_execution=0`; live runs also emit the explicitly
registered nested reference captures (`capture-only` evidence).

`--only argument-gpu` checks GPU copying and offsetting of a buffer pointer from
one argument buffer into another, followed by fenced compute/vertex/fragment
consumers (32 words, 16 pixels, argument/output guards after dropping indirect
target references). It captures thread-to-device sampler AIR for offline
rejection checks; legal GPU copies of device texture/sampler references stay out
of scope here. `--only argument-copy` compares legal GPU texture/sampler copies
and constant pointer offset/writeback with the CPU argument encoder across two
rounds, Shared/Private destinations, dynamic nonzero resource indices, compute/vertex/fragment
consumers, dropped application references and 128 unrelated live samplers.

`arguments-layout` checks layouts and setter boundaries (`gpu_execution=0`);
`arguments-dispatch` checks writes and guard regions on the GPU. Reflection and
stitching cases inspect compiler metadata without submitting commands. Archive
modes check rejection; `archive-fallback` additionally compiles source in the
same process and checks cold/warm dispatch output. NoCopy probes preserve the
read-only write gate; never set `RDNA4_TEST_RO_GPU_WRITE=1`.

The reference argument checks run eight layouts and fourteen valid A7 cases.
Four exploratory `bulk.*` calls pass mixed types or missing IDs to Apple's
encoder and abort under `MTL_ERROR_MODE=assert`; they are not valid reference
operations. The RDNA4 eighteen-case survey still tests their atomic rejection.
Use `test-argument-encoder --reference --a7-probe CASE` to repeat an individual
observation, or `--reference --layout-only` for the eight layout checks.

`--only argument-fragment-flow` includes seven quad-uniform/coordinate selectors
prefixed `T02.fragment-flow.runtime-coordinate-` with suffixes `discard`,
`continue`, `continue-grad`, `affine`, `affine-grad`, `math-loop`,
`math-loop-grad`, plus `buffer`, `buffer-grad`, `buffer-helper`,
`buffer-helper-grad`, `guarded`, `guarded-grad`, `guarded-null`, `dynamic`,
`dynamic-grad`, `dynamic-null`, `effects`, `effects-grad` and `effects-null`.
`continue` skips iteration zero for selected quads; `affine` swaps coordinate
axes with FMA scaling; `math-loop` proves pure math preserves uniform control;
`buffer`/`guarded`/`dynamic` test constant/pointer/64-bit-indexed scales (with
state-0 early return returning 7 for `*-null` and nonzero aligned offset reads
for state 1); `effects*` tests two device-buffer words per pixel across 128
words and byte guards (`effects-grad` passes while dependent-read reproduces
the old implicit Phi refusal). Candidate and reference verdicts remain separate.

## Texture buffers and CPU Vulkan shader replay

`PROJECT.null-sampler-bindings-host` exercises production missing-sampler
descriptor binding, compare-enabled Never versus ordinary representations,
allocation failure rollback/retry and bounded handle reuse (no GPU output oracle
for missing samplers). `PROJECT.null-texel-dummies-host` exercises the real typed
VkBufferView factory for six uniform/storage/component shapes, forty-two
construction failures, exact zeroing, padding preservation, rollback and reuse.
`PROJECT.texture-buffer-runtime-host` adds production live texture-buffer
construction (requiring enabled `robustBufferAccess2`), checked backing ranges,
absolute Vulkan offsets, format/access rejection, Private and read-only CPU
access, view/backing release, typed descriptor publication, buffer barriers and
delegation of texture-buffer blits to the existing buffer owner/copy path. Live
dynamic-texel execution across the translator/postpass/runtime boundary stays separate.

`test-texture-buffer.m` captures or executes fixed MSL 2.1 R32 float/uint/sint
read, write and width-query programs. `--read-write` adds a texture fence and
reads back the valid written texel; `--dynamic` uses two argument-buffer arrays
with independently checked source/destination backing. All bindings use nonzero
offsets. `--compile-only` records AIR; `--create-only` checks API construction.
A `--standalone` creation probe also exists without an accepted native GPU
result. The VM executes the ordinary direct reference; its native candidate
lacks `robustness2`, and a provisional module also exposed the VM host's
`ArgumentBufferTextures/NotImplemented` pipeline path (do not retry that known
fatal path as an ordinary regression).

**Independent attachment blending.** `CC=clang python3 src/garnet-metal/test-independent-blend.py -I/path/to/vulkan/include`
compiles the production device-feature setup and checks that it requests
`independentBlend` when advertised, without requiring a Vulkan device.

**CPU Vulkan sampler replay.** Build and run against a Linux CPU Vulkan ICD (refuses non-CPU devices):

```sh
cc -std=c11 -O1 -Wall -Wextra -Werror -I/path/to/vulkan/include \
  src/garnet-metal/test-sampler-replay.c -ldl -o /path/to/replay
VK_DRIVER_FILES=/path/to/lvp_icd.json python3 \
  src/garnet-metal/test-sampler-replay.py /path/to/libgarnetxlate.so \
  /path/to/libvulkan.so.1 /path/to/replay /path/to/new-results
```

Direct AIR, linked value-return helpers, linked checked-pointer helpers and
cached postpass paths run under ABI 9 with empty and reused `VkPipelineCache`:
four runtime output ranges (64 floats, 31 floats, read-only range, three-byte
range), 190 exact values across eight pipelines, dynamic sampler ordinary/Dref
selection across all eight compare ops, four corrupted-metadata rejection pairs,
and repeated-postpass byte identity. A `--source /path/to/sampler-mixed.ll` capture
may replace the authored default while preserving its sampling body and oracle.
(On macOS VM, the original flat mixed-sampler shader hits the host's missing
texture lowering; `test-argument-depth-mips.m` exercises mixed ordinary/Dref
through native Metal separately.)

**CPU Vulkan texel replay.** Build `test-texel-replay.c` with strict C flags and
Vulkan headers, select a CPU ICD, and invoke
`test-texel-replay.py TRANSLATOR VULKAN_LOADER PROBE AIR_DIRECTORY NEW_OUTPUT`
using `capture-0/texel.air`..`capture-2/texel.air` (or `capture-0-read-write`..`capture-2-read-write`
with `--read-write`). Each mode runs direct, linked-entry and repeated cached
postpass results against seven buffer-view formats (R32 float/uint/sint,
R8/RG8/RGBA8 UNORM, RGBA16Float), requiring `robustness2`, exact outputs,
untouched backing and out-of-bounds alpha rules. Note: the translator emits
ABI 10 for dynamic texture-buffer argument graphs, which the native
postpass/runtime consumer still rejects; same-invocation out-of-bounds
write/read forwarding and Vulkan volatile requirements also need an explicit audit.

**CPU Vulkan buffer-reinterpret replay.** Build `test-buffer-reinterpret-replay.c`
with the same strict flags and Vulkan headers, select a CPU ICD, and invoke
`test-buffer-reinterpret-replay.py TRANSLATOR_LIBRARY VULKAN_LOADER PROBE NEW_OUTPUT`.
Direct, linked and repeated cached translations execute four cold/warm pipelines
and 128 exact sign-bit results with nonzero descriptor offsets and guards.

**CPU Vulkan constant-helper replay.** Use the same probe and CPU ICD with
`test-constant-helper-replay.py TRANSLATOR_LIBRARY VULKAN_LOADER PROBE NEW_OUTPUT`.
The authored linked helper reads constant and dynamic float offsets from two
distinct buffer cursors. Linked and cached translations execute eight cold/warm
pipelines and check 64 exact float results, nonzero descriptor offsets, unchanged
read-only input and address tables, and output guards. This checks helper address
semantics on the CPU ICD; accelerated macOS icon rendering remains a separate check.

**GPU resource tables.** Compile `test-gpu-resource-tables.c` (shares `test-vulkan-replay.h`),
generate `tables.spv` via `garnetxlate` `gpu_resource_tables` with `GPU_RESOURCE_TABLE_EVIDENCE`,
compile `test-gpu-resource-producer.comp` with `glslangValidator -V --target-env vulkan1.2`,
and pass `VULKAN_LIBRARY CONSUMER_SPV PRODUCER_SPV`. Checks four GPU-only
update/consume rounds, high-word token collisions, missing/null tokens and guards
under ABI 9 seven-word token headers and version-3 buffer registry (descriptor
range must match recorded registry byte count).

## Quad controls, cold fallbacks and surface/present observation

`--only quad-margin` retains implicit, bias1 and gradient sampling and adds
`T02.quad-margin-sampler-implicit` and `T02.quad-margin-image-implicit` plus two
controls:

| Case | Exact invocation suffix | Required control count |
| --- | --- | --- |
| `T02.quad-margin-control-coordinate` | `--control coordinate` | One mode, one sampling, one flip, one case. |
| `T02.quad-margin-control-lod1` | `--control lod1` | Three modes, one sampling, two flips, six cases. |

The `r14-r18` archive records PASS on both endpoints for both controls; original
sampler-only (`actual=13 expected=11`) and image-only (`actual=23 expected=21`)
implicit sampling still fail on system Metal while candidate runs PASS (logs in
`logs/rdna4-metal2/20261106-tier2-implementation/native/RESULT-r14-r18-checkpoints.md`
and `r14-r18-log-index.tsv`).

The quad entry preserves the original divergent implicit-LOD failure: system
reference and RDNA4 both produced `actual=23 expected=22` at `pixel=(3,1)`.
`--mode uniform` and `--mode gradient` isolate descriptor divergence from sampler
clamp, upload and explicit gradient behavior; keep the divergent oracle and
tolerance unchanged. Raw logs are archived in
`logs/rdna4-metal2/20261106-depth-comparison/guest-red-and-quad-controls-r1.tar`
(`quad-lod-reference-r5.log`, `quad-lod-candidate-r5.log`).

`test-cold-pipelines.sh` runs nineteen default cases, including compute and
render deadline/retry and first results from concurrent cold compute pipelines. Its test translator
forwards ABI3 capability-aware exports to the matching real library; refusal
cases check both the injected reason and underlying error (render failures
preserve cache-miss code 41 and stage timeout code 9 within the three-second
application deadline).

`test-render-daemon-miss.sh` runs on macOS without a GPU device. It checks that
render cache misses queue and wait for only the missing stages, retain cached
stages, and leave in-process requests out of the daemon queue.

`test-render-fallback.m` exercises the production render placeholder and watcher
in an isolated MoltenVK VM bundle (`sudo test-render-fallback VM_TEST_BUNDLE EMPTY_CACHE_DIRECTORY`).
Checks that a cold miss preserves 1024 existing pixels, then publishes translated
shaders and repaints only 64 pixels through the original pipeline object while
960 pixels stay untouched. Runs in `test-execution.sh` as well.

`test-surface-capture.m` checks the opt-in completion observer on a 330px-wide
RGBA16Float IOSurface (2816-byte stride): off by default, bounded to 2 snapshots
per geometry and 64 MiB per process, skipping display surfaces and writing on its
own queue. `/private/var/tmp/rdna4m-observe` (root-owned) recreates CRC/frame
trace markers and `/private/tmp/rdna4m-surfaces` after boot cleanup;
`tools/rdna4/tests/render-observe-test.sh` checks two simulated boots and symlink
refusal.

For a short desktop interaction trace, the optional `/private/tmp/rdna4m-cblog`
log records command-buffer submission (`S`), retired path-(b) host copies (`W`),
terminal GPU status (`F`), and scanout presentation attempts (`P`). Join `S`,
`W`, and `F` by `cb`; join the terminal display sequence `dseq` to `P.seq`.
`W.copies` counts copies in one successfully retired segment, not commands
just encoded. Multiple `W` records may share a `cb`; intermediate segments may
have `dseq=0`. Direct-mapped surfaces require no path-(b) copy. A `P` with `rc=0`
is an accepted request, not proof of latch or pixel contents. Compare deltas
during an interaction that changes the desktop; an idle interval requires no
advance. `test-nocopy-writeback.sh` exercises the `W` boundary without GPU work.

`T` records identify read-only or display-classified BGRA8 textures by the
root texture and backing addresses, IOSurface ID (zero for a buffer alias),
geometry, pitch and command-buffer `cb`. `use=read` is source use;
`use=write` records an encoded write, not a completed copy. The distinct
`attachment-refused` and `writeback-refused` records identify the first
blocked stage. Match the backing address to the import record in the same
WindowServer process, then check `disp`, `S`, `W`, `F` and `P`. Matching only
the geometry or the kernel's present ID cannot identify an IOSurface.
For completed segments, `W` reports `copies` for image-to-buffer write-back
and `linear` for direct writes made visible to the host. Earlier logs can omit
`W` for completed linear writes.

`/private/tmp/rdna4m-present-capture` (root- or WindowServer-owned regular file)
enables top-64-row captures after accepted DP presents for WindowServer only
(rolling 16 records, <=2 Hz with backoff). Normal, deferred-copy and retry
presents can schedule one background capture at a time. The capture reserves
the accepted slot through the GPU copy into host-cached staging memory and
samples the IOSurface before and after that copy. GPU completion and file I/O
run outside the scanout mutex. For both top and full captures, the observer
checks the accepted Present ID, the latched slot and its hardware fetch address
before and after the readback. If a newer frame replaces it, the pixels remain
available for comparison but are not verified scanout evidence. Legacy top
captures have no latch binding. Verify offline with the simulated scanout tests.
- `test-present-capture.sh SOURCE OUTPUT`
- `test-crc-observer.sh SOURCE OUTPUT`
- `analyze-present-capture.py CAPTURE_DIRECTORY --output REPORT.json`
  (regression: `test-present-analysis.py`)

The existing `/private/tmp/rdna4m-present-full` opt-in, used together with
`/private/tmp/rdna4m-present-capture`, captures the entire accepted DP slot
instead of the top rows. Full mode retains two records and refuses a frame if
its padded GPU copy would exceed 64 MiB. The record includes the boot UUID,
the slot and hardware latch observed on both sides of the read, and a SHA-256
made before writing the bytes. A frame displaced before verification yields an
unverified record.
On the development host, analyze the extracted capture with
`analyze-present-capture.py CAPTURE_DIRECTORY --require-full-boot BOOT_UUID`.
Use the independently recorded boot UUID from the desktop run. This checks
geometry, complete bytes, matching Present ID and latch evidence, and integrity.
Legacy full captures without Present ID binding remain readable for offline
comparison but cannot pass this gate. The gate does not compare to expected
desktop pixels or prove panel appearance. Retain interaction and
submission/write-back/presentation evidence separately.

The installed collector archives up to 32 regular capture files per live
WindowServer PID from `rdna4m/88` or `rdna4m/0`, excluding symlinks and other PIDs.

## Broader VM comparisons, automatic heaps and live target mode

`test-texture-host-access.m` runs in `test-execution.sh`: `readonly` checks that
`replaceRegion` refuses writes on an `mprotect`-protected no-copy buffer while
CPU/GPU reads stay exact; `iosurface` checks nonzero partial updates, row padding
and readback for BGRA8 1446x1250/5824, RGBA16Float 330x1507/2816 and A8
1698x1248/1792 via the VM IOSurface copy path.

For the broader VM pixel comparisons, supply Vulkan headers:

```sh
sudo env TMPDIR=/path/to/scratch RDNA4_VK_INC="-I/path/to/vulkan/include" \
  bash src/garnet-metal/test-execution.sh \
  /path/to/vm/GarnetMetal.bundle /path/to/radv/GarnetMetal.bundle /path/to/results
```

Each case gets its own translator cache. Core Image concurrent cold pipelines
use a root test hook to wait up to three seconds per filter for placeholder
replacement, then rerender (deferred pixels are logged as unvalidated);
`concurrent-compute` in the cold suite checks strict root refusal and retry
without placeholders.

Automatic-heap sizing, backing, aliasing and GPU checks are documented in
`docs/rdna4/automatic-heaps.md`. `test-heap-execution.sh` uses simulated Vulkan;
`test-heap-gpu.sh` builds a GPU data oracle (`--reference` / candidate), both run
serially in the existing VM.

**Live target mode.** Only after offline/VM validation and explicit installation
approval, use `run-compatibility.py` with `live` on the target bench. Live mode
rejects mock and VM selection, requires PCI `1002:7590`, requires the native-kext
ABI handshake and rejects software/VM backend names in logs (`reflection-only`
passes do not count as live GPU execution). Both large-addressing groups reject
VM mode before opening a bundle or creating output and await hardware validation.
The VM's CPU `machdep.cpu.features` must contain `VMM` (`kern.hv_vmm_present`
does not reliably identify this VM); keep VM tests serial and never passthrough
the RX 9060 XT into the analysis VM.

---

## Standalone OpenGL-on-Metal surface bridge (`host-cpu`)

`test-gl-surface.c` exercises the pure surface table and `IOAccelDeviceRegion`
geometry parser in `rdna4_gl_surface_pure.h` under ASan + UBSan and verifies
source pins on the assembled `GarnetDevice.m` output (`supportsUnalignedVertexFetch`,
`newGLDrawable`, `getSurfaceMode:width:height:forId:`, `commitAndHold`,
`setLineWidth:`, `rdna4_gl_surfaces_install()`, `+1` `CFRetain` retention in
`lookupIOSurfaceAtIndex:`, `WindowServer` exclusion, all six SkyLight
`IOAccel*Surface*` import names, and zero `AppleParavirt` / `glProgramAir` paths).
Run on the dev host after assembling `GarnetDevice.m` with `assemble-device.py`;
it validates CPU invariants and source wiring rather than live macOS WindowServer
or OpenGL runtime execution.

---

## Hardware tessellation (`offline-metadata` and `live`)

Post-tessellation vertex pipelines (`[[patch(triangle, N)]]` / `[[patch(quad, N)]]`)
compile through a pass-through vertex stage, a synthesized Tessellation Control
Shader (`rdna4_tessellation.h`) that reads `MTLTriangleTessellationFactorsHalf` /
`MTLQuadTessellationFactorsHalf` factors and `PerPatchControlPoint` / `PerPatch` /
`Constant` vertex attributes via buffer device addresses, and the translated
Tessellation Evaluation stage with `VK_TESSELLATION_DOMAIN_ORIGIN_LOWER_LEFT`:

```objc
MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
vd.attributes[0].format = MTLVertexFormatFloat2;
vd.attributes[0].bufferIndex = 0;
vd.layouts[0].stride = sizeof(simd_float2);
vd.layouts[0].stepFunction = MTLVertexStepFunctionPerPatchControlPoint;
vd.layouts[0].stepRate = 1;

MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
d.vertexFunction = [lib newFunctionWithName:@"tri_tes"];
d.fragmentFunction = [lib newFunctionWithName:@"pass_fs"];
d.vertexDescriptor = vd;
d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
d.tessellationPartitionMode = MTLTessellationPartitionModeFractionalEven;
d.tessellationFactorStepFunction = MTLTessellationFactorStepFunctionPerPatch;
d.tessellationOutputWindingOrder = MTLWindingClockwise;
d.maxTessellationFactor = 16;

id<MTLRenderPipelineState> pso = [dev newRenderPipelineStateWithDescriptor:d error:&err];
[enc setRenderPipelineState:pso];
[enc setVertexBuffer:cpBuf offset:0 atIndex:0];
[enc setTessellationFactorBuffer:factorBuf offset:0 instanceStride:0];
[enc drawPatches:3 patchStart:0 patchCount:1 patchIndexBuffer:nil
  patchIndexBufferOffset:0 instanceCount:1 baseInstance:0];
```

Offline SPIR-V generation, 16-bit capability gating, cache-key isolation and negative
configuration validation:

```sh
clang -std=c11 -O2 -Wall -Wextra -Werror -I/path/to/vulkan/include \
  src/garnet-metal/test-tessellation-spv.c -lm -o "$TMPDIR/test-tessellation-spv"
"$TMPDIR/test-tessellation-spv"
```

Live RX 9060 XT tessellation execution (all 9 scenarios: direct triangle/quad patches,
compute-generated half factors and scale, all four partition modes with same-function
isolation, CW/CCW winding and culling plus asymmetric quad edge check, factor step
functions and `instanceStride`/`baseInstance`, `UInt16`/`UInt32` `drawIndexedPatches:`
with `patchIndexBuffer`, GPU indirect direct/indexed patch draws, zero/negative/NaN
culling, zero-CP patches, and negative validation checks):

```sh
clang -arch x86_64 -fobjc-arc -O2 -Wall -Wextra -Werror \
  -framework Foundation -framework Metal \
  src/garnet-metal/test-tessellation.m -o "$TMPDIR/test-tessellation"
sudo RDNA4M_ALLOW=1 RDNA4M_TEST_INPROC=1 RDNA4M_TEST_USERCACHE="$TMPDIR/xlcache" \
  "$TMPDIR/test-tessellation" /path/to/GarnetMetal.bundle
```

## Indirect Command Buffers (7.1–7.3)

Offline SPIR-V expander generator validation and host-only transaction semantics
(no Vulkan headers or GPU submission required for `test-icb-transactions.sh`):

```sh
clang -std=c11 -O2 -Wall -Wextra -Werror -I/path/to/mesa-src/include \
  -Isrc/garnet-metal src/garnet-metal/test-icb-spv.c -o "$TMPDIR/test-icb-spv"
"$TMPDIR/test-icb-spv"

bash src/garnet-metal/test-icb-transactions.sh "$TMPDIR/icb-tx"
```

Live RX 9060 XT CPU ICB (draw, indexed draw, direct/indexed patch equivalence,
deferred commit ordering, sparse large command counts, negative checks), GPU
compute-encoded ICB (`cpu_readback=0`, indirect range buffer, overlapping blit),
and lifecycle/heap/event/fence/quarantine suites (test both `-O0` and `-O2` for
ARC lifetime coverage; note that compute encoders writing a private indirect
range buffer through an argument buffer must declare `[ce useResource:buf
usage:MTLResourceUsageWrite]`):

```sh
clang -arch x86_64 -fobjc-arc -O2 -Wall -Wextra -Werror \
  -framework Foundation -framework Metal \
  src/garnet-metal/test-icb.m -o "$TMPDIR/test-icb"
clang -arch x86_64 -fobjc-arc -O2 -Wall -Wextra -Werror \
  -framework Foundation -framework Metal \
  src/garnet-metal/test-icb-gpu.m -o "$TMPDIR/test-icb-gpu"
clang -arch x86_64 -fobjc-arc -O0 -Wall -Wextra -Werror \
  -framework Foundation -framework Metal \
  src/garnet-metal/test-icb-lifecycle.m -o "$TMPDIR/test-icb-lifecycle-o0"
clang -arch x86_64 -fobjc-arc -O2 -Wall -Wextra -Werror \
  -framework Foundation -framework Metal \
  src/garnet-metal/test-icb-lifecycle.m -o "$TMPDIR/test-icb-lifecycle-o2"

for b in test-icb test-icb-gpu test-icb-lifecycle-o0 test-icb-lifecycle-o2; do
  sudo RDNA4M_ALLOW=1 RDNA4M_TEST_INPROC=1 RDNA4M_TEST_USERCACHE="$TMPDIR/xlcache" \
    "$TMPDIR/$b" /path/to/GarnetMetal.bundle
done
```

## Remaining Metal 2 Formats, MSL 2.1, and Limits (8.1–8.3)

Host pure-C checks for occlusion 256 KiB boundary (`L20`), depth/stencil target
validation, integer clears/formats, texture descriptors, Wave32 `threadExecutionWidth`,
and Mac2 feature sets / capability source pins (compile against an assembled
`GarnetDevice.m` directory via `assemble-device.py`):

```sh
python3 src/garnet-metal/assemble-device.py --output "$TMPDIR/GarnetDevice.m" src/garnet-metal/GarnetDevice.m
cp src/garnet-metal/Info.plist "$TMPDIR/"
for t in test-gate test-occ test-depth test-intfmt test-texdesc test-devcaps; do
  cc -std=c11 -D_GNU_SOURCE -O2 -Wall -Wextra -Werror \
    -Isrc/garnet-metal -Isrc/garnet/src -o "$TMPDIR/$t" "src/garnet-metal/$t.c"
done
"$TMPDIR/test-gate" "$TMPDIR"
"$TMPDIR/test-occ" "$TMPDIR"
"$TMPDIR/test-depth" "$TMPDIR"
"$TMPDIR/test-intfmt" "$TMPDIR"
"$TMPDIR/test-texdesc" "$TMPDIR"
"$TMPDIR/test-devcaps"
```

Offline translator checks for patch `0069` (dual-source `Decoration::Index`,
`raster_order_group(0..7)` pixel interlock & `< 8` enforcement,
`air.simd_active_threads_mask.i64`, `air.simd_shuffle_rotate_up`,
`air.quad_shuffle_rotate_up`):

```sh
cargo test --offline -p garnetxlate --lib --test linked --test stack --test metal2_msl21
cargo test --offline -p metal2vulkan --lib -- \
  fragment_dual_source_and_raster_order_groups_are_parsed \
  air_simd_active_threads_mask_and_rotate_up_lower_cleanly \
  native_fragment_dual_source_and_raster_order_group_lower_cleanly
```

Live RX 9060 XT Objective-C verification suites (build with Xcode CLT on macOS;
pre-`01:30` baseline ran `test-metal2-formats-render` 9/10 before patch `0069`,
and all three must run on next macOS boot against the rebuilt bundle and
`libgarnetxlate.dylib`):

```sh
for t in test-metal2-formats-render test-metal2-msl-probe test-metal2-limits; do
  clang -fobjc-arc -O2 -Wall -Wextra -Werror -framework Foundation -framework Metal \
    "src/garnet-metal/$t.m" -o "$TMPDIR/$t"
  sudo RDNA4M_ALLOW=1 RDNA4M_TEST_INPROC=1 RDNA4M_TEST_USERCACHE="$TMPDIR/xlcache" \
    "$TMPDIR/$t" /path/to/GarnetMetal.bundle
done
```

## Metal 3 Texture Swizzle and Placement Heaps (2.1–2.2)

Host pure-C checks (`host-cpu`, ASan + UBSan) cover swizzle composition
(including the Apple doc parent/view composition rule, `Zero`/`One` constants,
`BGRA`, `A8Unorm`, depth swizzles, and descriptor/view usage refusal),
placement-heap offset/alignment/granularity/overflow validation, explicit
overlap aliasing, interval-union `usedSize` / `maxAvailableSizeWithAlignment:`
accounting, and assembled `GarnetDevice.m` source pins:

```sh
python3 src/garnet-metal/assemble-device.py --output "$TMPDIR/GarnetDevice.m" src/garnet-metal/GarnetDevice.m
cc -std=c11 -D_GNU_SOURCE -O2 -Wall -Wextra -Werror -Isrc/garnet-metal \
  -o "$TMPDIR/test-swizzle-placement" src/garnet-metal/test-swizzle-placement.c
"$TMPDIR/test-swizzle-placement" "$TMPDIR"
```

Live RX 9060 XT / `--reference` Objective-C suites (`live`, build with Xcode CLT
on macOS; compile `test-metal3-placement-heap.m` with both `-O0` and `-O2` for
ARC teardown accounting):

```sh
clang -arch x86_64 -fobjc-arc -O2 -Wall -Wextra -Werror \
  -framework Foundation -framework Metal \
  src/garnet-metal/test-metal3-swizzle.m -o "$TMPDIR/test-metal3-swizzle"
clang -arch x86_64 -fobjc-arc -O0 -Wall -Wextra -Werror \
  -framework Foundation -framework Metal \
  src/garnet-metal/test-metal3-placement-heap.m -o "$TMPDIR/test-metal3-placement-heap-o0"
clang -arch x86_64 -fobjc-arc -O2 -Wall -Wextra -Werror \
  -framework Foundation -framework Metal \
  src/garnet-metal/test-metal3-placement-heap.m -o "$TMPDIR/test-metal3-placement-heap-o2"

for b in test-metal3-swizzle test-metal3-placement-heap-o0 test-metal3-placement-heap-o2; do
  sudo RDNA4M_ALLOW=1 RDNA4M_TEST_INPROC=1 RDNA4M_TEST_USERCACHE="$TMPDIR/xlcache" \
    "$TMPDIR/$b" /path/to/GarnetMetal.bundle
done
```

## Metal 3 Barycentrics/Pull-Model, Counters/Amp, MSL 3 Atomics, Binary Archives, I/O Queue, and Residency Sets (2.4–2.6, 3.1, 4.1–4.2)

Host pure-C suites (`host-cpu`, run under both GCC `-O2 -Wall -Wextra -Werror`
and Clang ASan + UBSan) cover timestamp counter sampling & vertex amplification
(`test-counters-amp.c`), Garnet binary archive container codec/SHA-256/UUID/miss
semantics (`test-binary-archive.c`), fast I/O queue state machine/8192-command
cap & residency set snapshot retention (`test-io-residency.c`), plus Metal 3
capability probes and wiring pins (`test-devcaps.c`, `test-gate.c`, `test-occ.c`):

```sh
python3 src/garnet-metal/assemble-device.py \
  --output "$TMPDIR/src/garnet-metal/GarnetDevice.m" src/garnet-metal/GarnetDevice.m
cp src/garnet-metal/Info.plist "$TMPDIR/src/garnet-metal/Info.plist"

for t in test-devcaps test-gate test-occ test-counters-amp test-binary-archive test-io-residency; do
  cc -std=c11 -D_GNU_SOURCE -O2 -Wall -Wextra -Werror -Isrc/garnet-metal \
    -o "$TMPDIR/$t" "src/garnet-metal/$t.c"
done
"$TMPDIR/test-devcaps"
"$TMPDIR/test-gate" "$TMPDIR/src/garnet-metal"
"$TMPDIR/test-occ" "$TMPDIR/src/garnet-metal"
"$TMPDIR/test-counters-amp" "$TMPDIR/src/garnet-metal" src/garnet-metal
"$TMPDIR/test-binary-archive"
"$TMPDIR/test-io-residency" src/garnet-metal "$TMPDIR/src/garnet-metal"
```

Live RX 9060 XT / `--reference` Objective-C suites (`live`, build with Xcode CLT
on macOS; compile ARC lifetime suites `test-metal3-binary-archive.m`,
`test-metal3-io-queue.m`, and `test-metal3-residency-set.m` with both `-O0` and
`-O2`):

- `test-metal3-bary-pull.m` (Task `2.4`): `[[barycentric_coord]]` (perspective +
  `center_no_perspective`) and 4x MSAA `interpolant<T>::interpolate_at_{center,centroid,sample,offset}`
  for `float2` and `half2` vs analytical oracle.
- `test-metal3-counters-amp.m` (Task `2.5`): `MTLCommonCounterSetTimestamp`
  sampling across compute/render/blit passes and pass descriptors, timestamp
  monotonicity & error paths, and `supportsVertexAmplificationCount:` /
  `setVertexAmplificationCount:viewMappings:` boundary checks.
- `test-metal3-msl3-atomics.m` (Task `2.6`): `atomic<float>` (device +
  threadgroup add/sub/exchange/CAS min+max), 64-bit `atomic_ulong` /
  `atomic<int64_t>` (`Int64Atomics`), `bfloat` / `bfloat4` FMA/math/conversions,
  storage-texture atomics (`R32Uint` / `R32Sint`), and
  `simd_shuffle_and_fill_{up,down}` vs race-free CPU oracle.
- `test-metal3-binary-archive.m` (Task `3.1`): compute + render archive
  creation, tile refusal, serialization/reload in a fresh archive,
  `MTLPipelineOptionFailOnBinaryArchiveMiss` hit/miss, bit-exact GPU execution,
  and corrupt/tampered archive rejection.
- `test-metal3-io-queue.m` (Task `4.1`): `MTLIOFileHandle`, `MTLIOCommandQueue`,
  and `MTLIOCommandBuffer` async buffer/texture loads into Private storage,
  `MTLSharedEvent` handoff, serial barriers, 8,192-command cap, early handle
  release, and EOF/missing-file/compression/`tryCancel` error paths.
- `test-metal3-residency-set.m` (Task `4.2`): `MTLResidencySet` staged vs
  committed snapshots, queue `addResidencySet:` and command-buffer
  `useResidencySet:`, early ARC release of BDA/heap-backed resources at `-O0`
  and `-O2`, and leak-free reclamation.

```sh
for t in test-metal3-bary-pull test-metal3-counters-amp test-metal3-msl3-atomics; do
  clang -arch x86_64 -fobjc-arc -O2 -Wall -Wextra -Werror \
    -framework Foundation -framework Metal \
    "src/garnet-metal/$t.m" -o "$TMPDIR/$t"
done
for t in test-metal3-binary-archive test-metal3-io-queue test-metal3-residency-set; do
  for opt in O0 O2; do
    clang -arch x86_64 -fobjc-arc "-$opt" -Wall -Wextra -Werror \
      -framework Foundation -framework Metal \
      "src/garnet-metal/$t.m" -o "$TMPDIR/$t-$opt"
  done
done

for b in test-metal3-bary-pull test-metal3-counters-amp test-metal3-msl3-atomics \
         test-metal3-binary-archive-O0 test-metal3-binary-archive-O2 \
         test-metal3-io-queue-O0 test-metal3-io-queue-O2 \
         test-metal3-residency-set-O0 test-metal3-residency-set-O2; do
  sudo RDNA4M_ALLOW=1 RDNA4M_TEST_INPROC=1 RDNA4M_TEST_USERCACHE="$TMPDIR/xlcache" \
    "$TMPDIR/$b" /path/to/GarnetMetal.bundle
done
```

## Metal 3 Compute ICBs, Sparse/PRT Textures, and Ray Tracing Acceleration Structures (2.3, 5.3, 7.1)

Host pure-C suites (`host-cpu`, run under both GCC `-O2 -Wall -Wextra -Werror`
and Clang ASan + UBSan) cover compute ICB 640-byte ABI, command validation,
expander SPIR-V, and optional LavaPipe GPU replay (`test-compute-icb.c`), 64 KiB
sparse tile geometry/conversions, mip-tail layouts, tile pool, atomic bind/unbind
planning, indirect mapping validation, and `Apple6 == NO` pins
(`test-sparse-metal.c`), ray-tracing AS capability gating, scratch alignment,
triangle/AABB BLAS validation, 4x3->3x4 transpose & Default/UserID/Motion
instance conversion, and lifecycle state machine (`test-rt-accel.c`), plus
assembled `GarnetDevice.m` wiring pins (`test-gate.c`, `test-occ.c`):

```sh
python3 src/garnet-metal/assemble-device.py \
  --output "$TMPDIR/src/garnet-metal/GarnetDevice.m" src/garnet-metal/GarnetDevice.m
cp src/garnet-metal/Info.plist "$TMPDIR/src/garnet-metal/Info.plist"

for t in test-compute-icb test-sparse-metal test-rt-accel test-gate test-occ; do
  cc -std=c11 -D_GNU_SOURCE -O2 -Wall -Wextra -Werror -Isrc/garnet-metal \
    -o "$TMPDIR/$t" "src/garnet-metal/$t.c" -lm
done
"$TMPDIR/test-compute-icb"
"$TMPDIR/test-sparse-metal" src/garnet-metal
"$TMPDIR/test-rt-accel"
"$TMPDIR/test-gate" "$TMPDIR/src/garnet-metal"
"$TMPDIR/test-occ" "$TMPDIR/src/garnet-metal"
```

Pass `-DRDNA4_SPARSE_TEST_VK -DVK_NO_PROTOTYPES -I/path/to/vulkan/include` to
`test-sparse-metal.c` to validate real Vulkan 1.4 `VkBindSparseInfo` struct
builders, or `-DRDNA4_TEST_LAVAPIPE -I/path/to/vulkan/include -ldl` to
`test-compute-icb.c` for software Vulkan LavaPipe execution.

Live RX 9060 XT / `--reference` Objective-C suites (`live`, build with Xcode CLT
on macOS; compile `test-metal3-rt-accel.m` with both `-O0` and `-O2` for early
ARC release coverage):

- `test-metal3-compute-icb.m` (Task `2.3`): CPU-encoded and GPU-encoded
  multi-pipeline compute ICBs (`ConcurrentDispatch` + `ConcurrentDispatchThreads`),
  per-slot `setBarrier`, threadgroup memory, `stageInRegion`, indirect execution
  ranges, zero CPU readback, and negative error paths.
- `test-metal3-sparse-prt.m` (Task `5.3`): 64 KiB sparse tile queries &
  pixel/tile region conversions, `MTLHeapTypeSparse` + mipmapped sparse texture
  creation, `MTLResourceStateCommandEncoder` direct/batch/indirect map & unmap
  with `MTLFence`, MSL `sparse_color<float4>::resident()` / non-resident
  zero-read & write-discard, negative rejections, and `MTLGPUFamilyApple6 == NO`.
- `test-metal3-rt-accel.m` (Task `7.1`): `accelerationStructureSizesWithDescriptor:`,
  triangle & AABB BLAS builds, compacted size readback (`UInt32` / `UInt64`) +
  `copyAndCompactAccelerationStructure:`, clone copies, in-place/out-of-place
  refit, Default & UserID TLAS build + refit, early ARC release safety, and
  controlled negative rejections (`supportsRaytracing` remains `NO` until
  Tasks `7.2`–`7.3` land).

```sh
clang -arch x86_64 -fobjc-arc -O2 -Wall -Wextra -Werror \
  -framework Foundation -framework Metal -framework QuartzCore \
  src/garnet-metal/test-metal3-compute-icb.m -o "$TMPDIR/test-metal3-compute-icb"
clang -arch x86_64 -fobjc-arc -O2 -Wall -Wextra -Werror \
  -framework Foundation -framework Metal \
  src/garnet-metal/test-metal3-sparse-prt.m -o "$TMPDIR/test-metal3-sparse-prt"
for opt in O0 O2; do
  clang -arch x86_64 -mmacosx-version-min=12.0 -fobjc-arc "-$opt" -Wall -Wextra -Werror \
    -framework Foundation -framework Metal \
    src/garnet-metal/test-metal3-rt-accel.m -o "$TMPDIR/test-metal3-rt-accel-$opt"
done

for b in test-metal3-compute-icb test-metal3-sparse-prt \
         test-metal3-rt-accel-O0 test-metal3-rt-accel-O2; do
  sudo RDNA4M_ALLOW=1 RDNA4M_TEST_INPROC=1 RDNA4M_TEST_USERCACHE="$TMPDIR/xlcache" \
    "$TMPDIR/$b" /path/to/GarnetMetal.bundle
done
```

## Metal 3 Dynamic Libraries & Visible Function Tables (3.2, 3.3)

Host pure-C suite (`host-cpu`, run under both GCC `-O2 -Wall -Wextra -Werror`
and Clang ASan + UBSan with `-Wshadow -Wsign-conversion`) validates the `.grdl`
dynamic library container codec (atomic roundtrip, magic/version/SHA-256/truncation/tamper
rejection), Stage 2 linked cache-key determinism, patch `0074` VFT buffer encoding &
OOB-clamping simulation, `visible_function_tables` JSON reflection parsing, Footnote 10
mesh vs render function-pointer/RT exclusion, and source pins (`test-dynlib-vft.c`,
plus assembled `GarnetDevice.m` Wave 3B wiring pins in `test-gate.c`):

```sh
python3 src/garnet-metal/assemble-device.py \
  --output "$TMPDIR/src/garnet-metal/GarnetDevice.m" src/garnet-metal/GarnetDevice.m
cp src/garnet-metal/Info.plist "$TMPDIR/src/garnet-metal/Info.plist"

cc -std=c11 -D_GNU_SOURCE -O2 -Wall -Wextra -Werror -Wshadow -Wsign-conversion \
  -Isrc/garnet-metal -o "$TMPDIR/test-dynlib-vft" src/garnet-metal/test-dynlib-vft.c
"$TMPDIR/test-dynlib-vft" src/garnet-metal
cc -std=c11 -D_GNU_SOURCE -O2 -Wall -Wextra -Werror -Isrc/garnet-metal \
  -o "$TMPDIR/test-gate" src/garnet-metal/test-gate.c
"$TMPDIR/test-gate" "$TMPDIR/src/garnet-metal"
```

Live RX 9060 XT / `--reference` Objective-C suites (`live`, build with Xcode CLT
on macOS at both `-O0` and `-O2` for early ARC release coverage):

- `test-metal3-dynamic-lib.m` (Task `3.2`): `newDynamicLibrary:error:`,
  `serializeToURL:error:` (`.grdl` container on Garnet), `newDynamicLibraryWithURL:error:`
  reload, compute + vertex + fragment dynamic library linking via
  `preloadedLibraries`, `MTLLinkedFunctions.binaryFunctions`, and non-dynamic /
  missing-symbol / corrupt / truncated / foreign `.metallib` rejection.
- `test-metal3-function-tables.m` (Task `3.3`): compute and fragment
  `functionHandleWithFunction:` / `newVisibleFunctionTableWithDescriptor:`,
  `setVisibleFunctionTable:atBufferIndex:` / `setFragmentVisibleFunctionTable:atBufferIndex:`,
  dynamic handle swaps on the same PSO without recreation, null & OOB slot safe
  fallback without GPU hang, ARC early-release lifetime test, and Footnote 10
  mesh + render function-pointer rejection.

```sh
for t in test-metal3-dynamic-lib test-metal3-function-tables; do
  for opt in O0 O2; do
    clang -arch x86_64 -mmacosx-version-min=12.0 -fobjc-arc "-$opt" -Wall -Wextra -Werror \
      -framework Foundation -framework Metal \
      "src/garnet-metal/$t.m" -o "$TMPDIR/$t-$opt"
  done
done

for b in test-metal3-dynamic-lib-O0 test-metal3-dynamic-lib-O2 \
         test-metal3-function-tables-O0 test-metal3-function-tables-O2; do
  sudo RDNA4M_ALLOW=1 RDNA4M_TEST_INPROC=1 RDNA4M_TEST_USERCACHE="$TMPDIR/xlcache" \
    "$TMPDIR/$b" /path/to/GarnetMetal.bundle
done
```

## Metal 3 Mesh Shaders & Ray Tracing Runtime / IFT (6.2, 7.3, Wave 3E)

Host pure-C suites (`host-cpu`, run under both GCC `-O2 -Wall -Wextra -Werror`
and Clang ASan + UBSan) validate mesh descriptor/threadgroup/emulated-payload
planning, SPIR-V mesh execution-model inspection, and Footnote 10 rules
(`test-mesh-runtime.c`), plus ray-tracing reflection JSON parsing, patch `0077`
IFT storage-buffer ABI encoding/dispatch simulation, direct-AS vs 64-bit BDA
argument-buffer planning, AIR intersection kind inference, and honest capability
gating (`test-rt-runtime.c`), alongside assembled `GarnetDevice.m` Wave 3E
wiring pins (`test-gate.c`, `test-occ.c`):

```sh
python3 src/garnet-metal/assemble-device.py \
  --output "$TMPDIR/src/garnet-metal/GarnetDevice.m" src/garnet-metal/GarnetDevice.m
cp src/garnet-metal/Info.plist "$TMPDIR/src/garnet-metal/Info.plist"

for t in test-mesh-runtime test-rt-runtime test-gate test-occ; do
  cc -std=c11 -D_GNU_SOURCE -O2 -Wall -Wextra -Werror -Isrc/garnet-metal \
    -o "$TMPDIR/$t" "src/garnet-metal/$t.c"
done
"$TMPDIR/test-mesh-runtime"
"$TMPDIR/test-rt-runtime"
"$TMPDIR/test-gate" src/garnet-metal
"$TMPDIR/test-gate" "$TMPDIR/src/garnet-metal"
"$TMPDIR/test-occ" "$TMPDIR/src/garnet-metal"
```

Live RX 9060 XT / `--reference` Objective-C suites (`live`, build with Xcode CLT
on macOS; compile `test-metal3-raytracing.m` with both `-O0` and `-O2` for early
ARC release coverage):

- `test-metal3-mesh-shaders.m` (Task `6.2`): mesh-only and object+mesh pipelines
  (`newRenderPipelineStateWithMeshDescriptor:options:reflection:error:`),
  `drawMeshThreadgroups:`, `drawMeshThreads:`,
  `drawMeshThreadgroupsWithIndirectBuffer:`, `setObjectThreadgroupMemoryLength:`
  (up to 16 KiB), object/mesh buffer/texture/sampler binding, `privateFunctions`
  linkage, Footnote 10 mesh + function-pointer rejection, and `MTLGPUFamilyApple6 == NO`.
- `test-metal3-raytracing.m` (Task `7.3`): honest capability assertions
  (`supportsRaytracing`, `supportsRaytracingFromRender`,
  `supportsPrimitiveMotionBlur == NO`), compute & fragment ray-query traversal
  against triangle/AABB BLAS + compacted/refit TLAS via direct
  `setAccelerationStructure:` / `setFragmentAccelerationStructure:` and Tier 2
  argument-buffer 64-bit BDA (`setAccelerationStructure:atIndex:`),
  `MTLIntersectionFunctionTable` creation/binding/opaque-triangle signatures/null
  slots, and early ARC release safety under `-O0` and `-O2`.

```sh
clang -arch x86_64 -mmacosx-version-min=13.0 -fobjc-arc -O2 -Wall -Wextra -Werror \
  -framework Foundation -framework Metal \
  src/garnet-metal/test-metal3-mesh-shaders.m -o "$TMPDIR/test-metal3-mesh-shaders"
for opt in O0 O2; do
  clang -arch x86_64 -mmacosx-version-min=12.0 -fobjc-arc "-$opt" -Wall -Wextra -Werror \
    -framework Foundation -framework Metal \
    src/garnet-metal/test-metal3-raytracing.m -o "$TMPDIR/test-metal3-raytracing-$opt"
done

for b in test-metal3-mesh-shaders \
         test-metal3-raytracing-O0 test-metal3-raytracing-O2; do
  sudo RDNA4M_ALLOW=1 RDNA4M_TEST_INPROC=1 RDNA4M_TEST_USERCACHE="$TMPDIR/xlcache" \
    "$TMPDIR/$b" /path/to/GarnetMetal.bundle
done
```
