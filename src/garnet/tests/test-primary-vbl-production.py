import pathlib
import re
import sys

source = pathlib.Path(sys.argv[1]).read_text()
methods = (
    "vblInitialTimeoutUs",
    "onHardwareVbl",
    "onVblTimer",
    "registerForInterruptType",
    "unregisterInterrupt",
    "setInterruptState",
    "acquireUclkWatchLeaseLocked",
    "pinUclkForModesetLocked",
    "updateUclkPolicyLocked",
    "onUclkWatchdog",
    "scheduleUclkWatchdogLocked",
    "onSystemWake",
    "drainSystemTransitions",
)
helpers = re.search(r"^struct UclkDisplayLease \{.*?^constexpr uint8_t kUclkRecoveryLimit = .*?;", source,
                    re.MULTILINE | re.DOTALL)
if not helpers:
    raise SystemExit("Missing production UCLK helpers")
definitions = ["namespace {\n" + helpers.group() + "\n}"]
admission = re.search(r"^bool gSystemDisplayTransitionsBlocked = .*?^\}", source, re.MULTILINE | re.DOTALL)
if not admission:
    raise SystemExit("Missing production display transition admission")
definitions.append("namespace {\n" + admission.group() + "\n}")
for function in ("block_system_transitions", "resume_system_transitions", "drain_system_transitions", "notify_wake"):
    match = re.search(rf"^void garnet_primary_fb_{function}\([^{{}}]*\) \{{.*?^\}}", source,
                      re.MULTILINE | re.DOTALL)
    if not match:
        raise SystemExit(f"Missing production display transition {function}")
    definitions.append(match.group())
for method in methods:
    pattern = rf"^(?:bool|void|IOReturn|uint32_t|uint64_t) AMDGarnetPrimaryFramebuffer::{method}\([^{{}}]*\)(?: const)? \{{.*?^\}}"
    matches = list(re.finditer(pattern, source, re.MULTILINE | re.DOTALL))
    if len(matches) != 1:
        raise SystemExit(f"Expected one production definition for {method}, got {len(matches)}")
    definitions.append(matches[0].group())
driver = pathlib.Path(sys.argv[2]).read_text()
callback = re.search(r"^IOReturn GarnetDriver::sleepWakeCallback\([^{}]*\) \{.*?^\}", driver,
                     re.MULTILINE | re.DOTALL)
if not callback:
    raise SystemExit("Missing production power notification callback")
definitions.append(callback.group())
pathlib.Path(sys.argv[3]).write_text("\n\n".join(definitions) + "\n")
