# Argument summary shader

This directory is partial task 5.4 groundwork. It contains the executable
Private live-token summary shader and its generated SPIR-V header. It does not
submit work or change resource retirement; root/lifetime code must connect it to
the existing segment, fence, and checkpoint path.

## GPU ABI

- Set 0, binding 0 is one GPU-copied source chunk. The runtime bridge allocates
  one Private scratch buffer at `round_up(chunk_bytes + 7, 4)`, copies exactly
  `source_end` bytes from the caller's unchanged source range, and binds offset
  zero for every chunk. The shader still checks `source_end - 8`, so the
  three-byte word-rounding padding is never parsed as token data.
- Set 0, binding 1 is a sorted `uvec2[]` dictionary page. Each entry is
  `(lo, hi)` and sorting is by the full 64-bit value (`hi`, then `lo`).
- Set 0, binding 2 is a zero-initialized page-local `uint[]` output bitset.
  Found tokens set the bit for their global dictionary index with `atomicOr`;
  `push.output_word_base` converts that index into the page-local word.
- The 32-byte std430 push block is `rdna4_argument_summary_push` from
  `../garnet_argument_summary.h`.
- Use `ceil(source_count / 64)` workgroups. One dispatch handles a bounded
  source slice and one dictionary page. Partition by
  `maxComputeWorkGroupCount[0]`; never truncate a slice.

The shader scans every byte start, not just 8-byte-aligned starts. It assembles
the little-endian token from two 32-bit reads and needs no 64-bit integer or
atomic capability. The planner includes seven readable overlap bytes after each
non-final source chunk, so a token crossing a chunk boundary is still found.
The shader checks `source_end - 8` before a token load and never reads an
8-byte tail past the supplied range.

## Publication rule

`rdna4_argument_summary_plan_publish` returns `READY` only after every source
chunk and dictionary page completed and the live generation still equals the
plan generation. `INCOMPLETE`, `STALE`, `FAILED`, and `OBSOLETE` are never
authorization to clear or replace owners. The output scratch and completion
bitmap are caller-owned and bounded by the selected chunk/page sizes. The
publication ticket is zeroed unless the result is `READY`.

`READY` is the only state from which the lifetime owner may atomically replace
the persistent possible-token set. In-flight snapshots remain unchanged.

## Runtime bridge

`../rdna4_argument_summary.inc` contains the host-tested planning core and an
Objective-C/Vulkan bridge for integration into `GarnetDevice.m`. The bridge:

- creates the three-binding descriptor set layout, pipeline layout, shader
  module and compute pipeline lazily, with reverse-order rollback on failure;
- allocates one source staging buffer, one dictionary-page staging buffer and
  one output-page staging buffer per attempt, not per dispatch;
- GPU-copies Private source/dictionary bytes into those bounded Private
  scratch buffers and never reads Private storage through `contents`;
- copies each completed page bitset into the caller's host-coherent output
  buffer, then keeps the attempt and all resources alive until the existing
  segment fence completes.

The caller must invoke `rdna4_argument_summary_encode` after the existing
segment barrier and before `vkEndCommandBuffer`, retain the attempt on the
segment, and call `rdna4_argument_summary_attempt_complete` only after the
segment fence. Publication never clears owners itself.

Stable bridge API:

```c
RDNA4ArgumentSummaryTask *task = rdna4_argument_summary_task_new(
    source, source_offset, source_size,
    dictionary, dictionary_offset, candidate_count,
    output, output_offset, generation, limits,
    requested_chunk_bytes, requested_dictionary_page, &error);

BOOL ok = rdna4_argument_summary_encode(task, command_buffer, &attempt, &error);
/* Retain attempt and task resources until this segment's existing fence. */
status = rdna4_argument_summary_attempt_complete(
    attempt, fence_succeeded, live_generation);
status = rdna4_argument_summary_attempt_publish(
    attempt, live_generation, &publication);
```

`limits` may be `NULL` to read `RDNA4R.lim` at task creation. Otherwise its
fields are `max_storage_buffer_range`,
`min_storage_buffer_offset_alignment`, `max_compute_workgroup_count_x`,
`max_compute_workgroup_invocations`, `max_compute_workgroup_size_x`,
`requested_chunk_bytes` and `requested_dictionary_page`. The fixed shader local
size is 64, so both workgroup-size limits must be at least 64. A zero requested
size selects 1 MiB or 1024 entries. Chunk bytes are aligned down to the storage
offset alignment, capped so `round_up(chunk_bytes + 7, 4)` fits
`maxStorageBufferRange`; dictionary pages are a multiple of 32 candidates and
capped by `maxStorageBufferRange / 8`. The attempt allocates exactly one source
scratch, one dictionary-page scratch and one output-page scratch.

`rdna4_argument_summary_attempt_complete` returns `COMPLETE` only when the
existing fence succeeded, recording finished every job, and the generation is
still live. `rdna4_argument_summary_attempt_publish` returns `READY` only from
that state. Release the attempt only after the fence; device teardown calls
`rdna4_argument_summary_pipeline_destroy` before destroying `RDNA4R.dev`.

## Regenerate and validate

```sh
cd "$(git rev-parse --show-toplevel)"
src/garnet-metal/argument-summary/gen.sh
mkdir -p temp/scratch/rdna4-argument-summary-20261106
cc -std=c11 -Wall -Wextra -Werror -pedantic -O2 \
  -I src/garnet-metal \
  src/garnet-metal/test-argument-summary-plan.c \
  -o temp/scratch/rdna4-argument-summary-20261106/test-argument-summary-plan
clang -std=c11 -Wall -Wextra -Werror -pedantic -O2 \
  -I src/garnet-metal -x objective-c \
  src/garnet-metal/test-argument-summary-runtime.m \
  -o temp/scratch/rdna4-argument-summary-20261106/test-argument-summary-runtime
temp/scratch/rdna4-argument-summary-20261106/test-argument-summary-plan
temp/scratch/rdna4-argument-summary-20261106/test-argument-summary-runtime
```

`gen.sh` runs `glslangValidator -V --target-env vulkan1.2` and
`spirv-val --target-env vulkan1.2`. The shader ABI follows the Vulkan std430
storage-buffer and push-constant layout rules documented by the Khronos Vulkan
specification (Context7: `/khronosgroup/vulkan-docs`).
