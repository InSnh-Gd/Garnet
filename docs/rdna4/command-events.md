# Command events and encoder fences

## Command events (`MTLEvent` / `MTLSharedEvent`)

Event signal and wait calls split a Metal command buffer at their encoded
positions. A shared scheduler picks ready heads from the logical queues and
submits them to the backend Vulkan queue. A blocked consumer leaves other queues
free to submit its producer, while later command buffers on the same logical
queue stay ordered behind their head. CPU shared-event updates wake the same
scheduler.

Each submitted segment owns a fence. Its signals become visible after that
segment retires, without waiting for unrelated later segments. Completion
handlers and resource retirement still cover the whole Metal command buffer. A
failed producer fails dependent work rather than signaling success. Unresolved
accepted work keeps its resources quarantined and is never replayed.

Internal ordered host actions run in their own segments once the preceding
segment and its waits complete, and before the successor submits. A host-action
failure terminates the command buffer without replaying earlier GPU work.
Terminal processing releases queued action blocks; callbacks must not hold a
strong reference back to their owning command buffer.

For IOSurface textures backed by an optimal image and staging buffer, a segment
boundary writes dirty image data back to the imported pages before publishing
the signal. The next segment uploads CPU modifications when it first touches
the texture. Terminal processing writes back only dirty data from the final
segment, preventing it from overwriting CPU updates performed after an earlier
signal. Display selection and IOSurface use counts remain attached to the whole
command buffer.

A CPU/GPU exchange can therefore run inside a single command buffer: encode GPU
writes, signal a shared event, wait for a second value, and read the CPU's
update once the CPU observes the first value and writes the shared pages.

## Encoder fences (`MTLFence`)

`newFence` creates a device-owned fence with distinct update checkpoints. An
encoder update closes the current internal segment (including pending IOSurface
write-back). A consumer segment becomes ready once its captured checkpoint
retires. Later segments in the producer command buffer can stay blocked on an
event while the consumer runs. Failed producer checkpoints fail their consumers;
unresolved GPU work keeps standard quarantine rules.

Encode the producing work and its fence update before encoding the consuming
wait; across separate command buffers, commit the producer first. A render
update records a clear-only pass before publishing its checkpoint. Stage and
resource dependencies use conservative full barriers, and render continuations
preserve attachments via `LOAD`/`STORE`. Use encoder memory barriers for
dependencies within a single pass. When reusing a fence inside a pass, encode
its wait before its update.

System Metal completes a wait on a fresh, never-updated fence and captures the
latest update present at wait encoding (later updates do not retarget that wait
even if their producer commits first). Garnet matches that model with a ready
initial checkpoint and distinct subsequent checkpoints.

## Verification

- **Simulated scheduler and error paths** (macOS):
  ```sh
  bash src/garnet-metal/tests/run-metal-test.sh --vk-inc "$VULKAN_INCLUDE" \
      test-command-events "$OUT"
  bash src/garnet-metal/tests/run-metal-test.sh --vk-inc "$VULKAN_INCLUDE" \
      test-fences "$OUT"
  ```
- **GPU data oracles** (system Metal `--reference` or candidate bundle, testing
  both linear `a` and copy `b` IOSurface paths):
  ```sh
  bash src/garnet-metal/tests/run-metal-test.sh test-command-event-execution "$OUT"
  bash src/garnet-metal/tests/run-metal-test.sh test-fence-execution "$OUT"
  ```

Host/VM simulated runs verify scheduling, ordering, failure propagation, and
quarantine semantics; live target-GPU execution requires running on physical
RDNA 4 hardware (see [TESTING.md](../../src/garnet-metal/tests/TESTING.md)).
