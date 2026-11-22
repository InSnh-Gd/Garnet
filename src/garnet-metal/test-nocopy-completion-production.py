#!/usr/bin/env python3
import re
from pathlib import Path

root = Path(__file__).parent
blit = (root / "rdna4_blit_encoder_impl.inc").read_text()
compute = (root / "rdna4_compute_encoder_impl.inc").read_text()
render = (root / "rdna4_render_encoder_impl.inc").read_text()
queue = (root / "rdna4_command_queue_impl.inc").read_text()
command_buffer = (root / "rdna4_command_buffer_impl.inc").read_text()
runtime_test = (root / "test-nocopy-readonly.m").read_text()

compute_rejection = compute[compute.index("if (rdna4nc_stage_writes("):].split("return;", 1)[0]
render_rejection = render[render.index("if (roV || roF)"):].split("return NO;", 1)[0]
attachment_rejection = render[render.index("if (_hostWriteDropped)"):].split("return self;", 1)[0]
writeback_rejection = queue[queue.index("static BOOL rdna4_ios_writeback"):].split("if (![t rdna4IsIOS])", 1)[0]
finish = command_buffer[command_buffer.index("- (BOOL)rdna4Finish:"):]
flush_at = finish.index("[self rdna4FlushEventWrites:_vk];")
display_at = finish.index("for (RDNA4Texture *t in _iow)")
finish_after_flush = finish[flush_at:display_at]
written = command_buffer[command_buffer.index("- (void)rdna4IOWritten:"):]
written = written[:written.index("\n}") + 2]

checks = {
    "blit rejections fail the command buffer": not re.search(
        r"if\s*\(rdna4nc_drop_write\([^\n]+\)\)\s*return;", blit
    ),
    "compute rejections fail the command buffer": "[_cb rdna4Fail:" in compute_rejection,
    "render-stage rejections fail the command buffer": "[_cb rdna4Fail:" in render_rejection,
    "read-only attachments fail the command buffer": "[_cb rdna4Fail:" in attachment_rejection,
    "write-back reports refusal": bool(
        "static BOOL rdna4_ios_writeback" in queue
        and "rdna4nc_drop_write" in writeback_rejection
        and "return NO;" in writeback_rejection
    ),
    "failed write-back aborts command-buffer finish": "if (_encErr)" in finish_after_flush and "return NO;" in finish_after_flush,
    "read-only write test requires an error status": "command.status != MTLCommandBufferStatusError" in runtime_test,
    "read-only render attachment test expects command failure": bool(
        "renderCommandEncoderWithDescriptor:pass" in runtime_test
        and "NOCOPY_%s_RENDER_TARGET" in runtime_test
    ),
    "linear and copy-back IOSurface writes defer scheduled publication": bool(
        "[t rdna4IsIOS]" in written
        and "_scheduleAfterPublish = YES;" in written
        and "![t rdna4IOSLinear]" not in written
    ),
}

failed = [name for name, passed in checks.items() if not passed]
for name, passed in checks.items():
    print(f"{'PASS' if passed else 'FAIL'} {name}")
if failed:
    raise SystemExit(f"NOCOPY_COMPLETION_FAIL {len(failed)}/{len(checks)}")
print(f"NOCOPY_COMPLETION_PASS {len(checks)} production-path checks")
