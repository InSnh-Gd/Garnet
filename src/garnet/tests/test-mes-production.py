#!/usr/bin/env python3
"""Extract the production MES queue ownership configuration."""
from pathlib import Path
import sys

source = Path(sys.argv[2]).read_text()
begin = source.index("    MESSetHwResourcesInput in{};", source.index("mes_init_full(DeviceContext"))
end = source.index("    for (uint32_t i = 0; i < kMES_PriorityLevels;", begin)
Path(sys.argv[1]).write_text(
    "MESSetHwResourcesInput queue_resources() {\n" + source[begin:end] +
    "    return in;\n}\n")
