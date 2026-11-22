#!/usr/bin/env python3
"""Make an x86_64 fileset member readable as a standalone Mach-O.

Keep the full KC and its absolute file offsets; replace only the outer header
with the member's header. This deliberately keeps shared LINKEDIT bytes and
chained pointer encodings intact for the layout tools. Output stays local.
Usage: extract_fileset.py <collection.kc> <bundle-id> <new-output-file>
"""

from __future__ import annotations

import mmap
from pathlib import Path
import shutil
import struct
import sys
from typing import Final, NamedTuple

MAGIC: Final = 0xFEEDFACF
X86_64: Final = 0x01000007
FILESET: Final = 12
KEXT_BUNDLE: Final = 11
FILESET_ENTRY: Final = 0x80000035
SEGMENT_64: Final = 0x19


class ExtractionError(ValueError):
    """Unsupported or malformed kernel collection."""


class Command(NamedTuple):
    kind: int
    offset: int
    size: int


def commands(data: mmap.mmap | bytes, start: int) -> list[Command]:
    if start < 0 or start > len(data) - 32:
        raise ExtractionError("Mach-O header outside collection")
    magic, cpu, _, _, count, size, _, _ = struct.unpack_from("<8I", data, start)
    if magic != MAGIC or cpu != X86_64:
        raise ExtractionError("only little-endian x86_64 Mach-O is supported")
    pos = start + 32
    end = pos + size
    if end > len(data) or count > size // 8:
        raise ExtractionError("invalid load-command extent")
    result: list[Command] = []
    for _ in range(count):
        if end - pos < 8:
            raise ExtractionError("truncated load command")
        kind, length = struct.unpack_from("<II", data, pos)
        if length < 8 or length % 8 or length > end - pos:
            raise ExtractionError("invalid load-command size")
        result.append(Command(kind, pos, length))
        pos += length
    if pos != end:
        raise ExtractionError("load commands do not fill sizeofcmds")
    return result


def member_header(data: mmap.mmap | bytes, image_id: str) -> bytes:
    outer = commands(data, 0)
    if struct.unpack_from("<I", data, 12)[0] != FILESET:
        raise ExtractionError("input is not MH_FILESET")
    members: list[int] = []
    for command in outer:
        if command.kind != FILESET_ENTRY:
            continue
        if command.size < 32:
            raise ExtractionError("short LC_FILESET_ENTRY")
        _, fileoff, nameoff = struct.unpack_from("<QQI", data, command.offset + 8)
        if nameoff < 32 or nameoff >= command.size:
            raise ExtractionError("invalid fileset entry name offset")
        raw = data[command.offset + nameoff:command.offset + command.size]
        name, terminator, _ = raw.partition(b"\0")
        if not terminator:
            raise ExtractionError("unterminated fileset entry name")
        if name == image_id.encode("utf-8"):
            members.append(fileoff)
    if len(members) != 1:
        raise ExtractionError("bundle id must identify exactly one fileset member")
    start = members[0]
    nested = commands(data, start)
    if struct.unpack_from("<I", data, start + 12)[0] != KEXT_BUNDLE:
        raise ExtractionError("member is not MH_KEXT_BUNDLE")
    header_size = 32 + struct.unpack_from("<I", data, start + 20)[0]
    have_segments = False
    for command in nested:
        if command.kind != SEGMENT_64:
            continue
        if command.size < 72:
            raise ExtractionError("short LC_SEGMENT_64")
        fileoff, filesize = struct.unpack_from("<QQ", data, command.offset + 40)
        if fileoff > len(data) or filesize > len(data) - fileoff:
            raise ExtractionError("segment outside collection")
        if filesize and fileoff < header_size:
            raise ExtractionError("member segment would be overwritten by relocated header")
        have_segments = True
    if not have_segments:
        raise ExtractionError("member has no segments")
    return data[start:start + header_size]


def extract(collection: Path, image_id: str, destination: Path) -> None:
    with collection.open("rb") as source:
        if source.seek(0, 2) == 0:
            raise ExtractionError("empty collection")
        source.seek(0)
        with mmap.mmap(source.fileno(), 0, access=mmap.ACCESS_READ) as data:
            header = member_header(data, image_id)
        # Exclusive creation protects the input and any existing extraction.
        with destination.open("xb") as output:
            shutil.copyfileobj(source, output)
            output.seek(0)
            output.write(header)


def main() -> None:
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    extract(Path(sys.argv[1]), sys.argv[2], Path(sys.argv[3]))
    print("extracted fileset member with original file offsets: " + sys.argv[2])


if __name__ == "__main__":
    main()
