# Command events

Event signal and wait calls split a Metal command buffer at their encoded
positions. A shared scheduler selects ready heads from the logical queues and
uses the existing Vulkan queue. A blocked consumer leaves other queues free to
submit its producer; later command buffers on the same logical queue stay
behind their head. CPU shared-event updates wake the same scheduler.

Each submitted segment has a fence. Its signals become visible after that
segment completes, without waiting for unrelated later segments. Completion
handlers and resource retirement still cover the whole Metal command buffer.
An unsuccessful producer fails dependent work instead of signaling success.
Unresolved accepted work keeps its resources in quarantine and is never replayed.

Internal ordered host actions use their own segments. They run once after the
preceding segment and its waits complete, and before the successor can submit.
An action failure terminates the command buffer without replaying earlier GPU
work. Terminal processing releases queued action blocks. Callbacks must avoid
a strong reference back to their owning command buffer. The four scheduler
cases in `heaps/vm-heap-ordered-final-v5-build.log` check wait/fence ordering,
successor gating, failure and capture lifetime with simulated Vulkan. The ICB
reset/copy/execute path is not yet connected to these actions.

For IOSurface textures that use an optimal image and staging buffer, a segment
boundary writes dirty image data back to the imported pages before publishing
the signal. The next segment uploads CPU changes when it first touches the
texture. Terminal processing writes only dirty data from that last segment,
so it cannot overwrite CPU updates after an earlier signal. Display selection
and IOSurface use counts remain attached to the whole command buffer.

For example, a CPU/GPU exchange can encode GPU writes, signal a shared event,
wait for a second value, and then consume the CPU's update. The CPU notification
must observe the first value before changing the shared pages and publishing
the second value. No separate producer command buffer is needed.

Build and run the simulated submission/error tests in the existing macOS VM:

```sh
sudo bash tools/native/rdna4metal/test-command-events.sh \
    "$VULKAN_INCLUDE" "$RADV_DYLIB" "$NEW_OUTPUT"
```

Build the GPU data oracle separately, then run it against the isolated VM
bundle or use `--reference` for system Metal:

```sh
bash tools/native/rdna4metal/test-command-event-execution.sh "$TOOLS_OUTPUT"
sudo "$TOOLS_OUTPUT/test-command-event-execution" "$VM_BUNDLE"
sudo env RDNA4M_IOS_PATH=b \
    "$TOOLS_OUTPUT/test-command-event-execution" "$VM_BUNDLE"
```

Reviewed evidence: `logs/rdna4-metal2/20261105-110230/events/`, especially
`source-final-4.1.json`, `vm-final-events-lifetime.log`, and `final-artifacts/`.
The final source passes four simulated event modes, nine original completion
modes, fourteen one-shot modes, nineteen native-route modes, and the
production-export completion check. Five GPU data cases pass on each of the
linear and copy IOSurface paths. These are software/VM results; target-RDNA4
event execution remains part of the final physical acceptance.

## Encoder fences

`newFence` returns a device-owned identity with separate update checkpoints.
An encoder update ends the current internal segment, including pending
IOSurface write-back. A consumer segment becomes ready after its captured
checkpoint completes. Later work in the producer command buffer can remain
blocked on an event while the consumer runs. Failed producer checkpoints fail
their consumers; unresolved GPU work keeps the existing quarantine rules.

Encode the producing work and its fence update before encoding the consuming
wait. For different command buffers, commit the producer first. A render update
records a clear-only pass before publishing its checkpoint. Stage and resource
dependencies use conservative full barriers; render continuations preserve
attachments with LOAD/STORE. Use encoder memory barriers for dependencies
within a pass. When reusing a fence in a pass, encode its wait before its update.

The fixed system-Metal VM completes a wait on a new, never-updated fence and
captures the latest update at wait encoding. A later update does not retarget
that wait, even when its producer commits first. The implementation follows
these observations with a ready initial checkpoint and separate subsequent
checkpoints. The probes identify the VM device and report their actual data;
they do not establish target-RDNA4 execution.

Build the production scheduler/error checks and the GPU oracle in the existing
macOS VM:

```sh
sudo bash tools/native/rdna4metal/test-fences.sh \
    "$VULKAN_INCLUDE" "$RADV_DYLIB" "$NEW_OUTPUT"
bash tools/native/rdna4metal/test-fence-execution.sh "$TOOLS_OUTPUT"
"$TOOLS_OUTPUT/test-fence-execution" --reference a
sudo env RDNA4M_TEST_INPROC=1 \
    "$TOOLS_OUTPUT/test-fence-execution" "$VM_BUNDLE" a
sudo env RDNA4M_TEST_INPROC=1 \
    "$TOOLS_OUTPUT/test-fence-execution" "$VM_BUNDLE" b
```

Fence evidence is under `logs/rdna4-metal2/20261105-110230/fences/`.
`vm-v3-gpu.log` records six data cases on system Metal and both candidate
IOSurface paths: fresh-fence wait, render-to-blit, clear-only render producer,
blit-compute-blit, cross-command-buffer handoff, and IOSurface CPU exchange.
`vm-system-generation-probe-v2.log` and `vm-two-producer-probe.log` record the
additional system-Metal ordering observations.
