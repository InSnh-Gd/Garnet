import pathlib
import re
import sys

source = pathlib.Path(sys.argv[1]).read_text()
methods = (
    "onVblTimer",
    "registerForInterruptType",
    "unregisterInterrupt",
    "setInterruptState",
)
definitions = []
for method in methods:
    pattern = rf"^(?:void|IOReturn) AMDGarnetPrimaryFramebuffer::{method}\([^{{}}]*\) \{{.*?^\}}"
    matches = list(re.finditer(pattern, source, re.MULTILINE | re.DOTALL))
    if len(matches) != 1:
        raise SystemExit(f"Expected one production definition for {method}, got {len(matches)}")
    definitions.append(matches[0].group())
pathlib.Path(sys.argv[2]).write_text("\n\n".join(definitions) + "\n")
