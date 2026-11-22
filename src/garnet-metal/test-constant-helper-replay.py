#!/usr/bin/env python3
"""Execute linked constant helper byte cursors with a CPU Vulkan ICD."""
import argparse
import ctypes as C
import hashlib
import json
from pathlib import Path
import subprocess

class Dependency(C.Structure):
    _fields_ = [("symbol", C.c_char_p), ("symbol_len", C.c_size_t),
                ("ll", C.c_char_p), ("ll_len", C.c_size_t)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("translator", "vulkan", "replay", "output"):
        parser.add_argument(name, type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    fixtures = Path(__file__).resolve().parents[2] / "third-party/metal2vulkan-rdna4/garnetxlate/tests/fixtures"
    entry = (fixtures / "constant_helper_kernel.ll").read_bytes()
    helper = (fixtures / "constant_helper_body.ll").read_bytes()
    library = C.CDLL(str(args.translator.resolve()))
    pointer, size = C.POINTER(C.c_ubyte), C.c_size_t
    outputs = [C.POINTER(pointer), C.POINTER(size), C.POINTER(pointer), C.POINTER(size), C.c_char_p, size]
    library.garnetx_translate_linked.argtypes = [C.c_char_p, size, C.POINTER(C.c_uint32), C.POINTER(Dependency), size] + outputs
    library.garnetx_lower_argument_resources.argtypes = [C.c_char_p, size, C.c_char_p, size] + outputs
    library.garnetx_free.argtypes = [pointer, size]

    def call(name, parameters):
        code, metadata, code_size, meta_size = pointer(), pointer(), size(), size()
        error = C.create_string_buffer(4096)
        status = getattr(library, name)(*parameters, C.byref(code), C.byref(code_size), C.byref(metadata), C.byref(meta_size), error, len(error))
        try:
            if status:
                raise RuntimeError(f"{name}: {status}: {error.value!r}")
            return C.string_at(code, code_size.value), C.string_at(metadata, meta_size.value)
        finally:
            if code:
                library.garnetx_free(code, code_size)
            if metadata:
                library.garnetx_free(metadata, meta_size)

    local = (C.c_uint32 * 3)(1, 1, 1)
    dependency = Dependency(b"read_tail", 9, helper, len(helper))
    linked = call("garnetx_translate_linked", [entry, len(entry), local, C.byref(dependency), 1])
    cached = call("garnetx_lower_argument_resources", [linked[0], len(linked[0]), linked[1], len(linked[1])])
    repeated = call("garnetx_lower_argument_resources", [cached[0], len(cached[0]), cached[1], len(cached[1])])
    assert cached == repeated, "cached postpass is not stable"
    records = {}
    for name, (code, metadata) in (("linked", linked), ("cached", cached)):
        path = args.output / f"{name}.spv"
        path.write_bytes(code)
        path.with_suffix(".meta.json").write_bytes(metadata)
        subprocess.run(["spirv-val", "--target-env", "vulkan1.2", str(path)], check=True, timeout=20)
        process = subprocess.run([str(args.replay.resolve()), str(args.vulkan.resolve()), str(path.resolve()), "--constant-helper"],
                                 capture_output=True, text=True, timeout=60)
        (args.output / f"{name}.log").write_text(process.stdout + process.stderr)
        if process.returncode or "CONSTANT_HELPER_REPLAY_PASS cpu_backend=1 rounds=4 outputs=32 exact_bits=1 guards=1" not in process.stdout:
            raise RuntimeError(f"{name} execution failed: {process.stdout}\n{process.stderr}")
        records[name] = {"spv_sha256": hashlib.sha256(code).hexdigest(), "metadata_sha256": hashlib.sha256(metadata).hexdigest()}
    manifest = {"paths": records, "entry_sha256": hashlib.sha256(entry).hexdigest(),
                "helper_sha256": hashlib.sha256(helper).hexdigest(),
                "translator_sha256": hashlib.sha256(args.translator.read_bytes()).hexdigest(),
                "probe_sha256": hashlib.sha256(args.replay.read_bytes()).hexdigest(),
                "outputs": 64, "cpu_backend_only": True, "native_driver": "untested"}
    (args.output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print("CONSTANT_HELPER_REPLAY_PASS paths=linked,cached pipelines=8 outputs=64 exact_bits=1 guards=1 native_driver=untested")


if __name__ == "__main__":
    main()
