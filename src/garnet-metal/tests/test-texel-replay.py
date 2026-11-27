#!/usr/bin/env python3
"""Replay captured R32 float/uint/sint texture-buffer AIR on an explicit CPU ICD."""
import argparse
import ctypes as C
import hashlib
import json
from pathlib import Path
import subprocess


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("translator", "vulkan", "replay", "air_directory", "output"):
        parser.add_argument(name, type=Path)
    parser.add_argument("--read-write", action="store_true")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    library = C.CDLL(str(args.translator.resolve()))
    pointer, size = C.POINTER(C.c_ubyte), C.c_size_t
    outputs = [C.POINTER(pointer), C.POINTER(size), C.POINTER(pointer), C.POINTER(size), C.c_char_p, size]
    library.garnetx_translate.argtypes = [C.c_char_p, size, C.POINTER(C.c_uint32)] + outputs
    library.garnetx_translate_linked.argtypes = [C.c_char_p, size, C.POINTER(C.c_uint32), C.c_void_p, size] + outputs
    library.garnetx_lower_argument_resources.argtypes = [C.c_char_p, size, C.c_char_p, size] + outputs
    library.garnetx_free.argtypes = [pointer, size]

    def call(name, parameters):
        code, metadata, nc, nm = pointer(), pointer(), size(), size()
        error = C.create_string_buffer(4096)
        status = getattr(library, name)(*parameters, C.byref(code), C.byref(nc), C.byref(metadata), C.byref(nm), error, len(error))
        try:
            if status:
                raise RuntimeError(f"{name}: {status}: {error.value!r}")
            return C.string_at(code, nc.value), C.string_at(metadata, nm.value)
        finally:
            if code:
                library.garnetx_free(code, nc)
            if metadata:
                library.garnetx_free(metadata, nm)

    records = []
    for kind in range(3):
        air = args.air_directory / (f"capture-{kind}" + ("-read-write" if args.read_write else "")) / "texel.air"
        source_path = args.output / f"kind-{kind}.ll"
        subprocess.run(["llvm-dis", str(air), "-o", str(source_path)], check=True, timeout=20)
        source = source_path.read_bytes()
        local = (C.c_uint32 * 3)(1, 1, 1)
        direct = call("garnetx_translate", [source, len(source), local])
        linked = call("garnetx_translate_linked", [source, len(source), local, None, 0])
        assert direct == linked, "direct/linked-entry mismatch"
        cached = call("garnetx_lower_argument_resources", [direct[0], len(direct[0]), direct[1], len(direct[1])])
        assert cached == call("garnetx_lower_argument_resources", [cached[0], len(cached[0]), cached[1], len(cached[1])])
        for entry, (code, metadata) in (("direct", direct), ("linked-entry", linked), ("cached", cached)):
            spv = args.output / f"kind-{kind}-{entry}.spv"
            spv.write_bytes(code)
            meta = spv.with_suffix(".meta.json")
            meta.write_bytes(metadata)
            subprocess.run(["spirv-val", "--target-env", "vulkan1.2", str(spv)], check=True, timeout=20)
            assembly = subprocess.check_output(["spirv-dis", str(spv)], text=True, timeout=20)
            spv.with_suffix(".spvasm").write_text(assembly)
            assert " Buffer " in assembly and "OpImageQuerySize " in assembly
            assert "OpImageFetch " in assembly and "OpImageWrite " in assembly
            for line in assembly.splitlines():
                if "OpImageFetch " in line:
                    assert "Lod" not in line and "Sample" not in line, line
            for format_index in ([0, 3, 4, 5, 6] if kind == 0 else [kind]):
                result = subprocess.run([str(args.replay.resolve()), str(args.vulkan.resolve()),
                    str(spv.resolve()), str(kind), str(format_index), str(int(args.read_write))], capture_output=True, text=True, timeout=60)
                log = args.output / f"kind-{kind}-{entry}-format-{format_index}.log"
                log.write_text(result.stdout + result.stderr)
                if result.returncode or "TEXEL_REPLAY_PASS cpu_backend=1 rounds=4 outputs=64 exact_bits=1 guards=1 native_driver=untested" not in result.stdout:
                    raise RuntimeError(f"texel replay failed: {log}")
                records.append({"kind": kind, "entry": entry, "format": format_index,
                    "air_sha256": digest(air), "spv_sha256": digest(spv), "metadata_sha256": digest(meta),
                    "log": log.name, "rounds": 4, "outputs": 64, "read_write": args.read_write, "cpu_backend_only": True})
    (args.output / "manifest.json").write_text(json.dumps({"records": records,
        "translator_sha256": digest(args.translator), "replay_sha256": digest(args.replay),
        "native_driver": "unverified", "robustBufferAccess2": "required"}, indent=2) + "\n")
    print(f"TEXEL_REPLAY_MATRIX_PASS cases={len(records)} pipelines={4 * len(records)} "
          f"outputs={64 * len(records)} formats=7 paths=direct,linked-entry,cached cpu_backend_only=1 native_driver=untested")


if __name__ == "__main__":
    main()
