# Documentation index

Documentation in this repository covers the standalone Garnet driver architecture, build steps, subsystem specifications, and hardware test procedures. Keep canonical terminology aligned with [GLOSSARY.md](../GLOSSARY.md).

## 1. Overview, glossary, and build

| Document | Purpose |
| --- | --- |
| [README.md](../README.md) | Stack overview, supported hardware, build prerequisites, and credits |
| [README.zh-CN.md](../README.zh-CN.md) | Simplified Chinese version of the project README |
| [GLOSSARY.md](../GLOSSARY.md) | Canonical vocabulary (hardware targets, `Garnet.kext`, `GarnetAccel.kext`, `GarnetMetal.bundle`) |
| [BUILDING.md](../BUILDING.md) | Kext prerequisites, Linux cross-build, firmware/SDK handling, and matched payload build (`build-all.sh`) |
| [CHANGES.md](../CHANGES.md) | Upstream Cobalt64 snapshot provenance plus local Navi 44, display kext, and Metal 2 additions |

## 2. Active hardware runbooks and subsystem specs

| Document | Purpose |
| --- | --- |
| [tools/rdna4/README.md](../tools/rdna4/README.md) | Build, install, activate, recover, and inspect scripts for the standalone Garnet stack |
| [memory-reports.md](rdna4/memory-reports.md) | Native VRAM/GART reporting, large-memory budget fields, and `RDNA4Caps` |
| [large-buffer-backing.md](rdna4/large-buffer-backing.md) | Dedicated heap allocations and 64-bit BDA (`buffer_address_table_version = 2`) |
| [automatic-heaps.md](rdna4/automatic-heaps.md) | Automatic-heap placement and fallback rules in `GarnetMetal` |
| [command-events.md](rdna4/command-events.md) | `MTLEvent` / `MTLSharedEvent` semaphore backing and command-buffer ordering |
| [metal2-baseline.md](rdna4/metal2-baseline.md) | Metal 2 capability advertisement and baseline probe contracts |
| [metal2-matrix.md](rdna4/metal2-matrix.md) | Tier 2 feature/capability matrix across kernel, translator, RADV, and Metal bundle |
| [STRUCTURE.md](../src/garnet-metal/STRUCTURE.md) | Module layout of `src/garnet-metal/` |
| [TESTING.md](../src/garnet-metal/TESTING.md) | Host regression suites, offline gates, and physical verification tiers for `GarnetMetal` |
| [argument-summary/README.md](../src/garnet-metal/argument-summary/README.md) | Compute-assisted argument-buffer pointer summary shader and runtime layout |
| [mesa-patches/README.txt](../mesa-patches/README.txt) | 26-patch Mesa RADV Darwin backend stack and host test instructions |
| [third-party/metal2vulkan-rdna4/README.txt](../third-party/metal2vulkan-rdna4/README.txt) | 65-patch `metal2vulkan` stack and `garnetxlate` C-ABI translator notes |
