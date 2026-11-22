#!/usr/bin/env python3
"""Bounds and byte-preservation regressions for the fileset extractor."""

from pathlib import Path
import struct
import tempfile

from extract_fileset import ExtractionError, extract, member_header


def fixture() -> bytearray:
    data = bytearray(1024)
    struct.pack_into("<8I", data, 0, 0xFEEDFACF, 0x01000007, 3, 12, 1, 48, 0, 0)
    struct.pack_into("<IIQQII", data, 32, 0x80000035, 48, 256, 256, 32, 0)
    data[64:68] = b"gpu\0"
    struct.pack_into("<8I", data, 256, 0xFEEDFACF, 0x01000007, 3, 11, 1, 72, 0, 0)
    struct.pack_into("<II16sQQQQIIII", data, 288, 0x19, 72, b"__TEXT", 256, 512,
                     256, 512, 5, 5, 0, 0)
    data[512:520] = b"payload!"
    return data


def rejected(data: bytearray, name: str = "gpu") -> None:
    try:
        member_header(bytes(data), name)
    except ExtractionError:
        return
    raise AssertionError("malformed fileset was accepted")


def main() -> None:
    data = fixture()
    assert member_header(bytes(data), "gpu") == data[256:360]
    rejected(data, "absent")
    rejected(data[:16])
    bad = fixture()
    struct.pack_into("<I", bad, 36, 0)
    rejected(bad)
    bad = fixture()
    struct.pack_into("<I", bad, 56, 48)
    rejected(bad)
    bad = fixture()
    bad[64:80] = b"X" * 16
    rejected(bad)
    bad = fixture()
    struct.pack_into("<Q", bad, 48, 1000)
    rejected(bad)
    bad = fixture()
    struct.pack_into("<Q", bad, 336, 1024)
    rejected(bad)
    bad = fixture()
    struct.pack_into("<Q", bad, 328, 0)
    rejected(bad)
    bad = fixture()
    struct.pack_into("<I", bad, 268, 2)
    rejected(bad)
    with tempfile.TemporaryDirectory(prefix="rdna4-fileset-") as tmp:
        source = Path(tmp) / "collection.kc"
        output = Path(tmp) / "gpu"
        source.write_bytes(data)
        extract(source, "gpu", output)
        actual = output.read_bytes()
        assert len(actual) == len(data)
        assert actual[:104] == data[256:360]
        assert actual[104:] == data[104:]
        assert actual[512:520] == b"payload!"
        try:
            extract(source, "gpu", output)
        except FileExistsError:
            pass
        else:
            raise AssertionError("existing extraction was overwritten")
        assert source.read_bytes() == data
    print("fileset extraction: 11 scenarios passed")


if __name__ == "__main__":
    main()
