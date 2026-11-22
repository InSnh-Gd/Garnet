#!/usr/bin/env python3
"""Extract GMC allocation and default-address programming for host execution."""
from pathlib import Path
import sys

source = Path(sys.argv[2]).read_text()
begin = source.index("constexpr uint64_t kGMCGartPTVRAMOffset")
end = source.index("// PDE/PTE bit layout", begin)
parts = [source[begin:end]]
for marker in ("kern_return_t\ngmc_alloc_resources(",
               "void\ngmc_release_resources(",
               "static void\nhub_init_system_aperture_regs("):
    begin = source.index(marker)
    end = source.index("\n}\n", begin) + 3
    parts.append(source[begin:end])
Path(sys.argv[1]).write_text("\n".join(parts))
