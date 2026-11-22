#!/usr/bin/env python3
"""Compile the production setup sequence with a deterministic PMFW transport."""
from pathlib import Path
import sys

source = Path(sys.argv[2]).read_text()
begin = source.index("kern_return_t\nsmu_smc_hw_setup(")
end = source.index("\n//============================================================", begin)
Path(sys.argv[1]).write_text(source[begin:end])
