#!/usr/bin/env python3
"""Expand private device modules for production-source extraction and diff guards."""

import argparse
from pathlib import Path
import re
import sys


def assemble(source: Path, include_dir: Path) -> str:
    # Leave ordinary includes intact, just as in the historical monolithic source.
    # Honor include-once semantics for garnet_*_internal.h across modular units.
    seen_headers: set[Path] = set()
    def expand(path: Path, parents: tuple[Path, ...]) -> str:
        path = path.resolve()
        if path in parents:
            raise ValueError(f"cyclic device module include: {path.name}")
        if path.name.endswith("_internal.h"):
            if path in seen_headers:
                return ""
            seen_headers.add(path)
        return re.sub(
            r'^#include "(garnet_[a-z0-9_]*_internal\.h|garnet_[a-z0-9_]+\.m|rdna4_[a-z0-9_]+_impl\.inc)"\n',
            lambda match: expand(include_dir / match[1], (*parents, path)),
            path.read_text(),
            flags=re.MULTILINE,
        )
    return expand(source, ())


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("--include-dir", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    text = assemble(args.source, args.include_dir or args.source.parent)
    if args.output:
        args.output.write_text(text)
    else:
        sys.stdout.write(text)


if __name__ == "__main__":
    main()
