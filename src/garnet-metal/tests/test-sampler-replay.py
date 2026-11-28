#!/usr/bin/env python3
"""Execute direct, linked and cached mixed-sampler translations on a CPU ICD."""
import argparse
import ctypes as C
import hashlib
import json
from pathlib import Path
import re
import subprocess


class Dependency(C.Structure):
    _fields_ = [("symbol", C.c_char_p), ("symbol_len", C.c_size_t),
                ("ll", C.c_char_p), ("ll_len", C.c_size_t)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("translator", type=Path)
    parser.add_argument("vulkan", type=Path)
    parser.add_argument("replay", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--source", type=Path, default=Path(__file__).resolve().parents[3] /
                        "third-party/metal2vulkan-rdna4/garnetxlate/tests/fixtures/abi8/sampler-mixed.ll")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    library = C.CDLL(str(args.translator.resolve()))
    pointer = C.POINTER(C.c_ubyte)
    size = C.c_size_t
    outputs = [C.POINTER(pointer), C.POINTER(size), C.POINTER(pointer),
               C.POINTER(size), C.c_char_p, size]
    library.garnetx_translate.argtypes = [C.c_char_p, size, C.POINTER(C.c_uint32)] + outputs
    library.garnetx_translate_linked.argtypes = [C.c_char_p, size, C.POINTER(C.c_uint32),
                                             C.POINTER(Dependency), size] + outputs
    library.garnetx_lower_argument_resources.argtypes = [C.c_char_p, size, C.c_char_p, size] + outputs
    library.garnetx_free.argtypes = [pointer, size]
    local = (C.c_uint32 * 3)(32, 1, 1)

    def call(name, parameters, reject=False):
        spv, metadata, ns, nm = pointer(), pointer(), size(), size()
        error = C.create_string_buffer(4096)
        result = getattr(library, name)(*parameters, C.byref(spv), C.byref(ns),
                                       C.byref(metadata), C.byref(nm), error, len(error))
        try:
            if reject:
                if result == 0 or spv or metadata or ns.value or nm.value:
                    raise RuntimeError(f"invalid metadata accepted or output published: {result}")
                return error.value.decode(errors="replace")
            if result != 0:
                raise RuntimeError(f"{name}: {result}: {error.value!r}")
            return C.string_at(spv, ns.value), C.string_at(metadata, nm.value)
        finally:
            if spv:
                library.garnetx_free(spv, ns)
            if metadata:
                library.garnetx_free(metadata, nm)

    def lower(pair, reject=False):
        return call("garnetx_lower_argument_resources",
                    [pair[0], len(pair[0]), pair[1], len(pair[1])], reject)

    source = args.source.read_text()
    # Exercise both a value-return helper and a helper that consumes the checked output pointer.
    body_pattern = r"(define void @sampler_mixed\([^\n]+\) [^\n]*\{)\n(.*?)\n\}"
    match = re.search(body_pattern, source, re.S)
    if not match or "  %mul = shl i32 %i, 1" not in match[2]:
        raise RuntimeError("captured sampler_mixed fixture layout changed")
    pointer_entry = source[:match.start(2)] + (
        'entry:\n  call void @sampler_pointer_helper.MTL_VISIBLE_FN_REF('
        'ptr addrspace(2) %args, ptr addrspace(1) %out, i32 %i)\n  ret void') + source[match.end(2):]
    pointer_entry += ('\ndeclare void @sampler_pointer_helper.MTL_VISIBLE_FN_REF('
                      'ptr addrspace(2), ptr addrspace(1), i32) section "air.externally_defined"\n'
                      '!air.visible_function_references = !{!9000}\n'
                      '!9000 = !{!"air.visible_function_reference", '
                      'ptr @sampler_pointer_helper.MTL_VISIBLE_FN_REF, !"sampler_pointer_helper"}\n')
    pointer_entry = pointer_entry.encode()
    pointer_helper = re.sub(r"^!air.kernel = .*\n", "", source, flags=re.M).replace(
        "@sampler_mixed", "@sampler_pointer_helper").encode()
    (args.output / "linked-pointer-entry.ll").write_bytes(pointer_entry)
    (args.output / "linked-pointer-helper.ll").write_bytes(pointer_helper)
    stores = match[2][match[2].index("  %mul = shl i32 %i, 1"):]
    stores = stores.replace("store float %6,", "store float %ordinary,").replace(
        "store float %5,", "store float %comparison,")
    entry_body = ('entry:\n  %pair = call <2 x float> @sampler_helper.MTL_VISIBLE_FN_REF('
                  'ptr addrspace(2) %args, i32 %i)\n'
                  '  %ordinary = extractelement <2 x float> %pair, i32 0\n'
                  '  %comparison = extractelement <2 x float> %pair, i32 1\n' + stores)
    entry = source[:match.start(2)] + entry_body + source[match.end(2):]
    helper_body = (match[2][:match[2].index("  %mul = shl i32 %i, 1")] +
                   '  %pair0 = insertelement <2 x float> undef, float %6, i32 0\n'
                   '  %pair = insertelement <2 x float> %pair0, float %5, i32 1\n'
                   '  ret <2 x float> %pair')
    helper = source[:match.start(2)] + helper_body + source[match.end(2):]
    helper = re.sub(r"^!air.kernel = .*\n", "", helper, flags=re.M)
    helper = helper.replace("define void @sampler_mixed", "define <2 x float> @sampler_mixed")
    helper = re.sub(r'ptr addrspace\(1\)[^,\n]* %out, ', "", helper)
    helper = helper.replace("@sampler_mixed", "@sampler_helper").encode()
    entry += ('\ndeclare <2 x float> @sampler_helper.MTL_VISIBLE_FN_REF(ptr addrspace(2), '
              'i32) section "air.externally_defined"\n'
              '!air.visible_function_references = !{!9000}\n'
              '!9000 = !{!"air.visible_function_reference", '
              'ptr @sampler_helper.MTL_VISIBLE_FN_REF, !"sampler_helper"}\n')
    entry = entry.encode()
    (args.output / "source.ll").write_text(source)
    (args.output / "linked-entry.ll").write_bytes(entry)
    (args.output / "linked-helper.ll").write_bytes(helper)
    dependency = Dependency(b"sampler_helper", len(b"sampler_helper"), helper, len(helper))
    direct = call("garnetx_translate", [source.encode(), len(source.encode()), local])
    linked = call("garnetx_translate_linked", [entry, len(entry), local,
                                            C.byref(dependency), 1])
    pointer_dependency = Dependency(b"sampler_pointer_helper", len(b"sampler_pointer_helper"),
                                    pointer_helper, len(pointer_helper))
    linked_pointer = lower(call("garnetx_translate_linked", [pointer_entry, len(pointer_entry), local,
                                C.byref(pointer_dependency), 1]))
    direct_gpu, linked_gpu = lower(direct), lower(linked)
    cached = lower(direct_gpu)
    if cached != direct_gpu or lower(linked_gpu) != linked_gpu or lower(linked_pointer) != linked_pointer:
        raise RuntimeError("warm argument postpass changed shader or reflection")
    contract = json.loads(direct_gpu[1])["argument_resource_tables"]
    if contract != {"version": 9, "table_binding": 672, "categories": [0, 2],
                    "comparison_sampler": True, "null_shapes": [1, 65],
                    "roots": [{"binding": 707, "buffer_index": 3}]}:
        raise RuntimeError(f"unexpected replay contract: {contract}")
    for pair in (linked_gpu, linked_pointer):
        if json.loads(pair[1])["argument_resource_tables"] != contract:
            raise RuntimeError("linked AIR changed table contract")
    manifest = {"source_sha256": hashlib.sha256(source.encode()).hexdigest(),
                "linked_entry_sha256": hashlib.sha256(entry).hexdigest(),
                "linked_helper_sha256": hashlib.sha256(helper).hexdigest(),
                "linked_pointer_entry_sha256": hashlib.sha256(pointer_entry).hexdigest(),
                "linked_pointer_helper_sha256": hashlib.sha256(pointer_helper).hexdigest(),
                "translator_sha256": hashlib.sha256(args.translator.read_bytes()).hexdigest(),
                "replay_sha256": hashlib.sha256(args.replay.read_bytes()).hexdigest(),
                "contract": contract, "paths": {}, "negative": {}}
    for name, pair in [("direct", direct_gpu), ("linked", linked_gpu), ("cached", cached),
                       ("linked-pointer", linked_pointer)]:
        path = args.output / f"{name}.spv"
        path.write_bytes(pair[0])
        (args.output / f"{name}.meta.json").write_bytes(pair[1])
        subprocess.run(["spirv-val", "--target-env", "vulkan1.2", str(path)], check=True)
        run = subprocess.run([str(args.replay.resolve()), str(args.vulkan.resolve()), str(path)],
                             text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=70)
        (args.output / f"{name}.log").write_text(run.stdout)
        rounds = re.findall(r"^MIXED_SAMPLER_REPLAY_ROUND_PASS mode=(\d) pipeline=(cold|warm) "
                            r"outputs=(\d+) guards=1$", run.stdout, re.M)
        expected_rounds = [(str(mode), temperature, str(count))
                           for mode, count in enumerate((64, 31, 0, 0))
                           for temperature in ("cold", "warm")]
        marker = "MIXED_SAMPLER_REPLAY_PASS cpu_backend=1 rounds=8 outputs=190 bounds=4"
        if run.returncode or marker not in run.stdout.splitlines() or rounds != expected_rounds:
            raise RuntimeError(f"{name} replay failed ({run.returncode}): {run.stdout}")
        manifest["paths"][name] = {"spv_sha256": hashlib.sha256(pair[0]).hexdigest(),
                                    "metadata_sha256": hashlib.sha256(pair[1]).hexdigest()}
        print(f"SAMPLER_REPLAY_PATH_PASS path={name} pipelines=8 outputs=190 bounds=4", flush=True)
    for field, value in [("comparison_sampler", False), ("categories", [0]),
                         ("table_binding", 671), ("version", 12345)]:
        bad = json.loads(direct_gpu[1])
        bad["argument_resource_tables"][field] = value
        manifest["negative"][field] = lower((direct_gpu[0], json.dumps(bad).encode()), True)
        print(f"SAMPLER_REPLAY_REJECT_PASS field={field}", flush=True)
    (args.output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print("SAMPLER_REPLAY_PASS paths=4 pipelines=32 outputs=760 negatives=4 native_driver=untested")


if __name__ == "__main__":
    main()
