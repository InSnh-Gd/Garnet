# Argument summary shader

Compute shader and generated SPIR-V header used by `GarnetMetal` to summarize
live resource tokens inside Private argument buffers without reading Private
storage on the CPU.

## GPU ABI

- **Set 0, binding 0**: GPU-copied source chunk. The bridge allocates one
  Private scratch buffer of size `round_up(chunk_bytes + 7, 4)`, copies
  `source_end` bytes from the caller's source range, and binds offset zero for
  every chunk. The shader bounds-checks `source_end - 8` so word-rounding
  padding is never parsed as token data.
- **Set 0, binding 1**: Sorted `uvec2[]` dictionary page (`(lo, hi)`, ordered
  by the full 64-bit value `hi`, then `lo`).
- **Set 0, binding 2**: Zero-initialized page-local `uint[]` output bitset.
  Matched tokens set their global dictionary index via `atomicOr`;
  `push.output_word_base` maps the global index into the page-local word.
- **Push constants**: 32-byte `std430` `rdna4_argument_summary_push` from
  `../garnet_argument_summary.h`.
- **Dispatch**: `ceil(source_count / 64)` workgroups per bounded source slice
  and dictionary page, partitioned by `maxComputeWorkGroupCount[0]`.

The shader scans every byte offset (not just 8-byte aligned offsets),
assembling little-endian 64-bit tokens from two 32-bit reads so no 64-bit
integer or atomic extension is required. Non-final source chunks include 7
overlap bytes so tokens straddling a chunk boundary are still detected.

## Publication rule

`rdna4_argument_summary_plan_publish` returns `READY` only after every source
chunk and dictionary page finishes and the live generation still matches the
plan generation. `INCOMPLETE`, `STALE`, `FAILED`, and `OBSOLETE` never clear or
replace owners, and the publication ticket is zeroed unless the result is
`READY`. Only from `READY` may the lifetime owner atomically replace the
persistent possible-token set; in-flight snapshots stay unchanged.

## Runtime bridge

Implemented in `../garnet_argument_summary.m` (and `../garnet_argument_summary.h`),
the Objective-C/Vulkan bridge:

- Lazily creates the descriptor set layout, pipeline layout, shader module, and
  compute pipeline with reverse-order rollback on failure.
- Allocates one source staging buffer, one dictionary-page staging buffer, and
  one output-page staging buffer per attempt (not per dispatch).
- Copies Private source/dictionary bytes on the GPU without touching CPU
  `contents`, then copies completed page bitsets into the host-coherent output
  buffer and retains all attempt resources until the segment fence completes.

Callers invoke `rdna4_argument_summary_encode` after the segment barrier and
before `vkEndCommandBuffer`, retain the attempt on the segment, and call
`rdna4_argument_summary_attempt_complete` only after the segment fence retires.

```c
RDNA4ArgumentSummaryTask *task = rdna4_argument_summary_task_new(
    source, source_offset, source_size,
    dictionary, dictionary_offset, candidate_count,
    output, output_offset, generation, limits,
    requested_chunk_bytes, requested_dictionary_page, &error);

BOOL ok = rdna4_argument_summary_encode(task, command_buffer, &attempt, &error);
/* Retain attempt and task resources until this segment's fence completes. */
status = rdna4_argument_summary_attempt_complete(
    attempt, fence_succeeded, live_generation);
status = rdna4_argument_summary_attempt_publish(
    attempt, live_generation, &publication);
```

Passing `NULL` for `limits` reads `RDNA4R.lim` at task creation. Because the
shader uses a fixed local size of 64, `max_compute_workgroup_invocations` and
`max_compute_workgroup_size_x` must be at least 64. Passing `0` for chunk size
or dictionary page selects defaults of 1 MiB or 1024 entries; chunk size aligns
down to `min_storage_buffer_offset_alignment` and dictionary pages round to
multiples of 32 candidates within `maxStorageBufferRange / 8`. Device teardown
calls `rdna4_argument_summary_pipeline_destroy` before destroying `RDNA4R.dev`.

## Regenerate and validate

```sh
cd "$(git rev-parse --show-toplevel)"
src/garnet-metal/argument-summary/gen.sh
make -C src/garnet-metal/tests host
```

`gen.sh` compiles the GLSL source with `glslangValidator -V --target-env vulkan1.2`
and validates the output with `spirv-val --target-env vulkan1.2`. The runtime
mock check (`src/garnet-metal/tests/test-argument-summary-runtime.m`) can be
built on macOS via `src/garnet-metal/tests/run-metal-test.sh`.
