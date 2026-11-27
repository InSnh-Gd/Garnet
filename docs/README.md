# Documentation index

Documentation for the standalone Garnet RDNA 4 driver stack on x86_64 macOS. Canonical names and component boundaries are defined in [GLOSSARY.md](../GLOSSARY.md).

## 1. Overview, glossary, and build

| Document | Purpose |
| --- | --- |
| [README.md](../README.md) | Stack overview, supported hardware, installation, prerequisites, and credits |
| [README.zh-CN.md](../README.zh-CN.md) | Simplified Chinese project README |
| [GLOSSARY.md](../GLOSSARY.md) | Canonical names (`Garnet`, `RDNA4`, `Navi 44`, `Navi 48`) and stack terms |
| [BUILDING.md](../BUILDING.md) | Prerequisites, macOS/Linux builds, firmware handling, and `build-all.sh` |
| [CHANGES.md](../CHANGES.md) | Release changelog and historical Cobalt64 baseline provenance |

## 2. Subsystem specifications and tooling docs

| Document | Purpose |
| --- | --- |
| [tools/rdna4/README.md](../tools/rdna4/README.md) | Payload build, verification, installation, activation, and removal scripts |
| [tools/ioaccel-layout/README.md](../tools/ioaccel-layout/README.md) | Kernel-collection vtable extraction, header generation, and layout verification |
| [full-bar-aperture.md](rdna4/full-bar-aperture.md) | Pre-PCI Full-BAR compatibility (`GarnetPCIBarCompat.kext`), VRAM ownership, and 8 GB checklist |
| [memory-reports.md](rdna4/memory-reports.md) | Native VRAM/GART reporting, large-memory budgets, and `RDNA4,MemoryCaps` |
| [large-buffer-backing.md](rdna4/large-buffer-backing.md) | Multi-gigabyte buffers, composite backing, and 64-bit BDA addressing |
| [automatic-heaps.md](rdna4/automatic-heaps.md) | `MTLHeapTypeAutomatic` and `MTLHeapTypePlacement` backing and aliasing rules |
| [command-events.md](rdna4/command-events.md) | `MTLEvent`, `MTLSharedEvent`, and `MTLFence` command-buffer segmentation |
| [metal2-baseline.md](rdna4/metal2-baseline.md) | Metal 2 runtime baseline, capability contracts, and null-descriptor fallback |
| [metal2-matrix.md](rdna4/metal2-matrix.md) | Metal 2 (`Mac2` / MSL 2.0 to 2.1) feature, limit, pixel-format, and large-memory matrix |
| [metal3-matrix.md](rdna4/metal3-matrix.md) | Metal 3 (`AMD 6000-series` parity) feature, limit, sparse/PRT, mesh, and ray tracing matrix |
| [STRUCTURE.md](../src/garnet-metal/STRUCTURE.md) | Module layout of `src/garnet-metal/` |
| [TESTING.md](../src/garnet-metal/tests/TESTING.md) | Host regression suites, offline gates, and live verification modes for `GarnetMetal` |
| [argument-summary/README.md](../src/garnet-metal/argument-summary/README.md) | Compute-assisted argument-buffer pointer summary shader and bridge API |
| [mesa-patches/README.txt](../mesa-patches/README.txt) | 33-patch Mesa RADV Darwin backend stack (ABI 1.14) and host test instructions |
| [third-party/metal2vulkan-rdna4/README.txt](../third-party/metal2vulkan-rdna4/README.txt) | 90-patch `metal2vulkan` stack and `garnetxlate` C-ABI translator notes |
