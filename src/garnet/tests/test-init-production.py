#!/usr/bin/env python3
"""Extract the production GART helper and CPInit stage for an ordering test."""
from pathlib import Path
import sys

source = Path(sys.argv[2]).read_text()
begin = source.index("static kern_return_t ensure_gfxhub_gart(")
end = source.index("\n}\n", begin) + 3
helper = source[begin:end]
begin = source.index("    case BringupStage::CPInit: {")
end = source.index("\n    case BringupStage::MESInit:", begin)
stage = source[begin:end].replace(
    "    case BringupStage::CPInit: {",
    "kern_return_t run_cp_init(BringupContext &ctx) {\n"
    "    DeviceContext &dev = *ctx.dev;", 1)
Path(sys.argv[1]).write_text(helper + "\n" + stage)
