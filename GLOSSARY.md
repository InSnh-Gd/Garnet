# Garnet

Garnet brings a shared macOS RDNA4 GPU driver stack up on one card, the Radeon
RX 9060 XT, on one PC.

## Hardware

**RDNA4**:
The GPU family this stack implements. Shared code, interfaces, boot-args, bundle
identifiers, and file names all say RDNA4.

**Navi 44**:
The GPU die in the RX 9060 XT (PCI 1002:7590, gfx1200, GC 12.0.0). The only target of this project.
The name appears only where the chip genuinely differs: the per-chip table
(`src/garnet/src/amd/amdgpu_asic.*`), firmware selection (`CHIPS=navi44`), and the
canned `gfx1200` Mesa device entry.

**Navi 48**:
The GPU die in the RX 9070 XT and R9700 (gfx1201, GC 12.0.1) that upstream Cobalt64 was built
and tested on. The driver retains its per-chip selection and firmware path, plus the canned
`gfx1201` Mesa entry. This project's current physical target remains the RX 9060 XT;
retaining the Navi 48 path does not establish physical validation here. Shared code uses RDNA4.

**Test bench**:
The separated two-machine test setup: development runs on a Linux workstation (plus an optional offline local macOS VM), and physical hardware validation runs remotely over SSH, Screen Sharing (VNC), and an attached physical monitor against a dedicated RX 9060 XT 16 GB (Navi 44) test machine booting macOS 26.7.2 (`25G332`) by default via OpenCore. Upstream Cobalt64 uses a separate Navi 48 machine over SSH.
_Avoid_: test PC, rig

## Driver stack

**Upstream**:
The Cobalt64 project (`github.com/Almosst-DEV/Cobalt64`) this repo is based on.

**Garnet kext**:
The standalone primary kernel extension built from `src/garnet/` (`Garnet.kext`,
`org.garnet.driver`, OpenCore ESP `Kernel.Add`). It takes the GPU from power-on to a usable
device: PCIe setup, IP discovery, firmware load and engine initialisation, built-in display
mode-setting and primary framebuffer (`AMDGarnetPrimaryFramebuffer`, `AMD*` prefix for CoreDisplay vendor classification; `GarnetDisplayNub`), the
`'GRNT'` native user client (`IOAccelGarnetNativeClient`), and `GarnetMetalNub`. Has no `Lilu.kext` or external display plugin
dependency.
_Avoid_: driver (ambiguous)

**Accelerator kext**:
The Auxiliary Kernel Collection extension built from `src/garnet-accel/` (`GarnetAccel.kext`,
`org.garnet.accel`), publishing `GarnetFramebuffer` and `GarnetAccelerator` over
`GarnetMetalNub`.

**Display peer**:
The runtime coordination state (`Garnet,DisplayPeer*` and `Garnet,DisplayPrimary*` via
`GarnetDisplayGate` / `GarnetDisplayNub`) keeping primary framebuffer ownership and scanout
handoff serialized. Historical `AMDRadeonRDNA4.kext` (`Lilu` plugin) and `RDNA4FB` are retired.

**Metal bundle**:
The user-space Metal driver bundle (`GarnetMetal.bundle`, `org.garnet.metal`) built from
`src/garnet-metal/`, subclassing `MTLIOAccelDevice` over `libvulkan_radeon.dylib` and
`libgarnetxlate.dylib`.

**Native route**:
The path where Metal runs on Mesa's RADV Vulkan driver, ported to macOS, on top of the bring-up kext.

**Substitution route**:
Upstream's older path that swaps shaders inside Apple's own AMD accelerator kext using Apple-derived shader files. Out of scope.

**Compute test**:
Upstream's first end-to-end GPU check: a shader dispatched through the command processor writes a known value to memory that the CPU reads back.

**GPU-composited desktop**:
The macOS desktop with WindowServer's Metal compositor running on this driver stack instead of the CPU.
_Avoid_: acceleration, QE/CI

**Sampler configuration**:
The filter, address, comparison, coordinate, LOD and anisotropy settings of a
Metal sampler. Equivalent configurations share backend handles and one public
argument-sampler capacity reservation, regardless of the number of sampler objects.

**Sampler binding**:
A sampler configuration used by an encoder, optionally with effective LOD clamps.
A clamp override changes the binding without changing the public sampler token.
