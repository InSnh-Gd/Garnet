#!/usr/bin/env python3
"""Compare the bounded present captures offline; no native GPU work."""
import argparse
import hashlib
import json
from pathlib import Path
from uuid import UUID


def boot_uuid(value):
    if not isinstance(value, str):
        raise ValueError('missing boot UUID')
    parsed = str(UUID(value))
    if parsed != value.lower():
        raise ValueError('invalid boot UUID format')
    return parsed.upper()


def scanout_binding(meta, full):
    present_keys = ('scanout_before_present', 'scanout_after_present')
    has_present = any(key in meta for key in present_keys)
    if not full and not has_present:
        return 'not-recorded'
    if meta.get('scanout_verified') is not True:
        if full:
            raise ValueError('scanout slot was not verified latched throughout capture')
        return 'unverified'
    slot_mc, latch = meta['scanout_slot_mc'], meta['scanout_latched_frame']
    if (type(slot_mc) is not int or slot_mc <= 0 or
            not slot_mc == meta['scanout_before_mc'] == meta['scanout_after_mc'] or
            type(latch) is not int or latch <= 0):
        raise ValueError('scanout slot was not verified latched throughout capture')
    if not has_present:
        return 'latch-verified-legacy'
    present = meta['present_id']
    if type(present) is not int or present <= 0 or any(
            type(meta.get(key)) is not int or meta[key] != present for key in present_keys):
        raise ValueError('scanout present ID was not verified throughout capture')
    return 'present-and-latch-verified'


def analyze(path):
    meta = json.loads(path.read_text())
    row, rows = meta['bytes_per_row'], meta['captured_rows']
    mode = meta.get('capture_mode', 'top')
    if mode not in ('top', 'full'):
        raise ValueError('invalid capture mode')
    full = mode == 'full'
    if type(row) is not int or type(rows) is not int or row <= 0 or rows < 1 or (not full and rows > 64):
        raise ValueError('invalid capture geometry')
    if full:
        width, height = meta['width'], meta['height']
        if (type(width) is not int or type(height) is not int or width < 1 or rows != height or
                row != width * 4 or meta['full_frame'] is not True or meta['pixel_format'] != 'BGRA8'):
            raise ValueError('not a complete BGRA8 frame')
    binding = scanout_binding(meta, full)
    name = meta['raw_file']
    if not isinstance(name, str) or Path(name).name != name or not name.endswith('.raw'):
        raise ValueError('invalid raw filename')
    size = row * rows
    if size > (64 if full else 1) * 1024 * 1024:
        raise ValueError('capture exceeds byte budget')
    raw_path = path.parent / name
    valid = not full and meta['source_valid'] is True
    expected_size = size * (3 if valid else 1)
    if raw_path.is_symlink() or not raw_path.is_file() or raw_path.stat().st_size != expected_size:
        raise ValueError('invalid raw file')
    raw = raw_path.read_bytes()
    if len(raw) != expected_size or meta['slot_offset'] != (size if valid else 0):
        raise ValueError('capture length or slot offset differs')
    slot = raw[meta['slot_offset']:meta['slot_offset'] + size]
    report = {key: meta[key] for key in ('sequence', 'surface', 'slot', 'present_id', 'time_ns', 'cost_ns', 'latest_submission')}
    report['scanout_binding'] = binding
    if full or 'boot_uuid' in meta:
        report['boot_uuid'] = boot_uuid(meta.get('boot_uuid'))
    if binding in ('present-and-latch-verified', 'latch-verified-legacy'):
        report.update(scanout_latched_frame=meta['scanout_latched_frame'], scanout_slot_mc=meta['scanout_slot_mc'])
    report['slot_sha256'] = hashlib.sha256(slot).hexdigest()
    report['record'] = str(path)
    if full:
        if report['slot_sha256'] != meta.get('slot_sha256'):
            raise ValueError('scanout SHA-256 mismatch')
        report.update(comparison='verified-full-scanout-memory', width=width, height=height)
    elif valid:
        before, after = raw[:size], raw[2 * size:]
        mismatch = [y for y in range(rows) if before[y * row:(y + 1) * row] != slot[y * row:(y + 1) * row]]
        unstable = [y for y in range(rows) if before[y * row:(y + 1) * row] != after[y * row:(y + 1) * row]]
        report.update(source_sha256=hashlib.sha256(before).hexdigest(), slot_diff_rows=mismatch, source_changed_rows=unstable)
        if unstable:
            report['comparison'] = 'source-changed-during-read'
        elif mismatch and meta['latest_submission'] > meta['sequence']:
            report['comparison'] = 'source-differs-with-later-submission'
        elif mismatch:
            report['comparison'] = 'stable-source-differs-from-slot'
        else:
            report['comparison'] = 'source-matches-slot'
    else:
        report['comparison'] = 'slot-only-source-unavailable'
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--require-full-boot', type=boot_uuid,
                        help='require a full scanout capture with verified Present ID and latch from this boot')
    args = parser.parse_args()
    results, errors = [], []
    for path in sorted(args.directory.rglob('present-*.json')):
        try:
            if path.is_symlink():
                raise ValueError('metadata is a symlink')
            results.append(analyze(path))
        except (ValueError, OSError, KeyError, TypeError) as error:
            errors.append({'record': str(path), 'error': str(error)})
    results.sort(key=lambda entry: entry['time_ns'])
    if args.require_full_boot and not any(entry['comparison'] == 'verified-full-scanout-memory' and
            entry['scanout_binding'] == 'present-and-latch-verified' and
            entry.get('boot_uuid') == args.require_full_boot for entry in results):
        errors.append({'error': f'no verified full capture with matching Present ID and latch from boot {args.require_full_boot}'})
    text = json.dumps({'captures': results, 'errors': errors,
        'limitation': 'Memory captures do not prove panel appearance or pixel correctness. Later GPU/CPU writers can change the source.'}, indent=2) + '\n'
    if args.output:
        args.output.write_text(text)
    else:
        print(text, end='')
    return 1 if errors or not results else 0


if __name__ == '__main__':
    raise SystemExit(main())
