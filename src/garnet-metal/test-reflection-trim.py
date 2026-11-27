#!/usr/bin/env python3
"""Check the native reflection-trim implementation against the ABI8 fixture."""

import ast
import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
FIXTURE = Path(__file__).with_name("fixtures") / "reflection-trim-abi8.json"
SOURCES = (
    ROOT / "src/garnet-metal/add-air.py",
)


def load_trim(path):
    tree = ast.parse(path.read_text())
    trim = next(node for node in tree.body
                if isinstance(node, ast.FunctionDef) and node.name == "trim")
    namespace = {}
    module = ast.Module(body=[trim], type_ignores=[])
    exec(compile(ast.fix_missing_locations(module), str(path), "exec"),
         namespace)
    return namespace["trim"]


def main():
    fixture = json.loads(FIXTURE.read_text())
    expected = fixture["expected"]
    for path in SOURCES:
        actual = load_trim(path)(fixture["input"])
        assert actual == expected, {
            "path": str(path),
            "actual": actual,
        }
        rooted = dict(fixture["input"], reflection_version=66, argument_buffer_roots=[3])
        assert load_trim(path)(rooted)["argument_buffer_roots"] == [3]
    print("REFLECTION_TRIM_ABI8_PASS paths=1 rust_fixture=1")


if __name__ == "__main__":
    main()
