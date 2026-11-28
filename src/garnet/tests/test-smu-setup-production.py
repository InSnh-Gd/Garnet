#!/usr/bin/env python3
"""Compile the production setup sequence with a deterministic PMFW transport."""
from pathlib import Path
import sys

source = Path(sys.argv[2]).read_text()
begin = source.index("kern_return_t\nsmu_smc_hw_setup(")
end = source.index("\n//============================================================", begin)
floor_begin = source.index("kern_return_t\nsmu_set_uclk_hard_min(")
floor_end = source.index("\n}", source.index("kern_return_t\nsmu_pin_uclk_top(", floor_begin)) + 2
Path(sys.argv[1]).write_text(source[begin:end] + "\n" + source[floor_begin:floor_end])
