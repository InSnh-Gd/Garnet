#!/usr/bin/env python3
import importlib.util
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile

spec = importlib.util.spec_from_file_location('capture', Path(__file__).with_name('analyze-present-capture.py'))
capture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(capture)
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    meta = dict(bytes_per_row=8, captured_rows=2, raw_file='frame.raw', source_valid=True,
                slot_offset=16, sequence=5, surface=1, slot=0, present_id=9, time_ns=10,
                cost_ns=1, latest_submission=5)
    path = root / 'present-1.json'
    def run(before, slot, after):
        path.write_text(json.dumps(meta))
        (root / 'frame.raw').write_bytes(before + slot + after)
        return capture.analyze(path)
    original = bytes(16)
    changed = bytes(8) + bytes([1]) * 8
    assert run(original, original, original)['comparison'] == 'source-matches-slot'
    result = run(original, changed, original)
    assert result['comparison'] == 'stable-source-differs-from-slot' and result['slot_diff_rows'] == [1]
    meta['latest_submission'] = 6
    assert run(original, changed, original)['comparison'] == 'source-differs-with-later-submission'
    assert run(original, original, changed)['source_changed_rows'] == [1]
    assert run(original, original, original)['scanout_binding'] == 'not-recorded'
    boot = '0113E762-C3E8-430B-95EA-5BA535D14237'
    meta.update(scanout_verified=True, scanout_before_present=9, scanout_after_present=9,
                scanout_before_mc=4096, scanout_after_mc=4096, scanout_slot_mc=4096,
                scanout_latched_frame=12, boot_uuid=boot)
    assert run(original, original, original)['scanout_binding'] == 'present-and-latch-verified'
    command = [sys.executable, spec.origin, str(root), '--require-full-boot', boot]
    gate = subprocess.run(command, capture_output=True, text=True, check=False)
    assert gate.returncode == 1 and 'no verified full capture' in gate.stdout
    meta.update(scanout_after_present=10)
    try:
        run(original, original, original)
        raise AssertionError('top capture verified with a different present')
    except ValueError:
        pass
    meta.update(scanout_verified=False)
    assert run(original, original, original)['scanout_binding'] == 'unverified'
    for key in ('scanout_before_present', 'scanout_after_present'):
        del meta[key]
    meta.update(source_valid=False, slot_offset=0)
    assert run(original, b'', b'')['comparison'] == 'slot-only-source-unavailable'
    meta['raw_file'] = '../outside.raw'
    try:
        run(original, b'', b'')
        raise AssertionError('unsafe path accepted')
    except ValueError:
        pass

    original = bytes(3840 * 2160 * 4)
    meta.update(raw_file='frame.raw', capture_mode='full', boot_uuid=boot, width=3840, height=2160,
                bytes_per_row=15360, captured_rows=2160, full_frame=True, scanout_verified=True,
                scanout_before_present=9, scanout_after_present=9,
                scanout_before_mc=4294967296, scanout_after_mc=4294967296, scanout_slot_mc=4294967296,
                scanout_latched_frame=12, pixel_format='BGRA8', source_valid=True,
                slot_sha256=hashlib.sha256(original).hexdigest())
    result = run(b'', original, b'')
    assert result['comparison'] == 'verified-full-scanout-memory' and result['boot_uuid'] == boot
    gate = subprocess.run(command, capture_output=True, text=True, check=False)
    assert gate.returncode == 0 and not json.loads(gate.stdout)['errors']

    present_fields = {key: meta.pop(key) for key in ('scanout_before_present', 'scanout_after_present')}
    assert run(b'', original, b'')['scanout_binding'] == 'latch-verified-legacy'
    gate = subprocess.run(command, capture_output=True, text=True, check=False)
    assert gate.returncode == 1 and 'no verified full capture' in gate.stdout, 'legacy full frame passed acceptance'
    meta.update(present_fields)

    def refused(label, data, **changes):
        saved = meta.copy()
        meta.update(changes)
        try:
            run(b'', data, b'')
            raise AssertionError(label + ' accepted as full scanout')
        except ValueError:
            pass
        finally:
            meta.clear()
            meta.update(saved)

    refused('changed payload', original[:-1] + b'\x01')
    refused('truncated payload', original[:-1])
    refused('missing latch', original, scanout_latched_frame=0)
    refused('different scanned slot', original, scanout_after_mc=4295032832)
    refused('invalid boot', original, boot_uuid='not-boot-uuid')
    refused('missing hash', original, slot_sha256=None)
    refused('mismatched present ID', original, scanout_before_present=9, scanout_after_present=10)
print('PRESENT_ANALYSIS_PASS; top comparison and latch binding, full 4K integrity and fail-closed checks')
