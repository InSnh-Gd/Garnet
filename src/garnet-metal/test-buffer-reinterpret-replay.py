#!/usr/bin/env python3
"""Check direct, linked and cached descriptor pointer views on an explicit CPU ICD."""
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
    fixture = Path(__file__).resolve().parents[2] / "third-party/metal2vulkan-rdna4/garnetxlate/tests/fixtures/descriptor_reinterpret.ll"
    source = fixture.read_text()
    start = source.index("define void @reinterpret_helper(")
    end = source.index("\n}", start) + 2
    helper = source[start:end].encode()
    entry = (source[:start] + source[end:]).replace("@reinterpret_helper(", "@reinterpret_helper.MTL_VISIBLE_FN_REF(")
    entry += ('\ndeclare void @reinterpret_helper.MTL_VISIBLE_FN_REF(ptr addrspace(2), ptr addrspace(1), i32) section "air.externally_defined"\n'
              '!air.visible_function_references = !{!1000}\n'
              '!1000 = !{!"air.visible_function_reference", ptr @reinterpret_helper.MTL_VISIBLE_FN_REF, !"reinterpret_helper"}\n')
    entry = entry.encode()
    library = C.CDLL(str(args.translator.resolve()))
    pointer, size = C.POINTER(C.c_ubyte), C.c_size_t
    outputs = [C.POINTER(pointer), C.POINTER(size), C.POINTER(pointer), C.POINTER(size), C.c_char_p, size]
    library.garnetx_translate.argtypes = [C.c_char_p, size, C.POINTER(C.c_uint32)] + outputs
    library.garnetx_translate_linked.argtypes = [C.c_char_p, size, C.POINTER(C.c_uint32), C.POINTER(Dependency), size] + outputs
    library.garnetx_lower_argument_resources.argtypes = [C.c_char_p, size, C.c_char_p, size] + outputs
    library.garnetx_free.argtypes = [pointer, size]

    def call(name, parameters):
        code, meta, nc, nm = pointer(), pointer(), size(), size()
        error = C.create_string_buffer(4096)
        result = getattr(library, name)(*parameters, C.byref(code), C.byref(nc), C.byref(meta), C.byref(nm), error, len(error))
        try:
            if result:
                raise RuntimeError(f"{name}: {result}: {error.value!r}")
            return C.string_at(code, nc.value), C.string_at(meta, nm.value)
        finally:
            if code:
                library.garnetx_free(code, nc)
            if meta:
                library.garnetx_free(meta, nm)

    local = (C.c_uint32 * 3)(32, 1, 1)
    dependency = Dependency(b"reinterpret_helper", len(b"reinterpret_helper"), helper, len(helper))
    direct = call("garnetx_translate", [source.encode(), len(source.encode()), local])
    linked = call("garnetx_translate_linked", [entry, len(entry), local, C.byref(dependency), 1])
    cached = call("garnetx_lower_argument_resources", [direct[0], len(direct[0]), direct[1], len(direct[1])])
    assert cached == call("garnetx_lower_argument_resources", [cached[0], len(cached[0]), cached[1], len(cached[1])])
    records = {}
    for name, (code, metadata) in (("direct", direct), ("linked", linked), ("cached", cached)):
        path = args.output / f"{name}.spv"
        path.write_bytes(code)
        (args.output / f"{name}.meta.json").write_bytes(metadata)
        subprocess.run(["spirv-val", "--target-env", "vulkan1.2", str(path)], check=True, capture_output=True)
        process = subprocess.run([str(args.replay.resolve()), str(args.vulkan.resolve()), str(path.resolve())],
                                 capture_output=True, text=True, timeout=60)
        (args.output / f"{name}.log").write_text(process.stdout + process.stderr)
        if process.returncode or "BUFFER_REINTERPRET_REPLAY_PASS cpu_backend=1 rounds=4 outputs=128 exact_bits=1 guards=1" not in process.stdout:
            raise RuntimeError(f"{name} replay failed ({process.returncode}): {process.stdout}\n{process.stderr}")
        records[name] = {"spv_sha256": hashlib.sha256(code).hexdigest(), "metadata_sha256": hashlib.sha256(metadata).hexdigest()}
    manifest = {"source_sha256": hashlib.sha256(source.encode()).hexdigest(), "paths": records,
                "translator_sha256": hashlib.sha256(args.translator.read_bytes()).hexdigest(),
                "replay_sha256": hashlib.sha256(args.replay.read_bytes()).hexdigest(), "outputs": 384,
                "cpu_backend_only": True, "native_driver": "untested"}
    (args.output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print("BUFFER_REINTERPRET_REPLAY_PASS paths=direct,linked,cached pipelines=12 outputs=384 exact_bits=1 guards=1 native_driver=untested")


if __name__ == "__main__":
    main()
