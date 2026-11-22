# ioaccel-layout

Reconstructs the C++ vtable layout and instance sizes of Apple's `IOAcceleratorFamily2` (macOS Tahoe, x86_64) from
**your own** kernel collections, and emits a header that lets a `-fapple-kext` subclass reproduce that layout without
Apple's private headers.

This directory contains only the scripts. It contains no Apple binaries, symbol tables, vtable dumps or generated headers,
and you must not redistribute what the scripts write. The output is derived from Apple software on your machine and
stays on your machine.

## Pipeline

| script | job |
|---|---|
| `extract_fileset.py` | Creates an analysis image for one x86_64 KC fileset member, retaining shared LINKEDIT data and original file offsets. |
| `kcmod.py` | Mach-O / kernel-collection reader (segments, `nm` symbols, chained-fixup vtable slots). Shared by the others. |
| `model.py` | Builds the class model: superclass chain, instance size, per-slot target and kind (new / override / inherit / pure). |
| `dump_vtables.py` | Runs the model over every class in the two kexts; writes `model.json`, `vtables/*.tsv`, `summary.md`, `paravirt_overrides.tsv`. |
| `exports.py` | Which symbols the family, boot KC and system KC export (a slot may only be referenced by name if it is exported). |
| `gen_header.py` | Writes `IOAccelFamily2_decl.h`, `slots.json`, `forwarded.json`, `downgraded.txt`. |
| `objvt.py` | Reads a vtable out of an x86_64 `MH_OBJECT` (no linking). |
| `check_layout.py` | Compiles a test subclass of every generated class and checks slot count, order and symbols against the KC. Prints `MATCH` or the first mismatch. |

## What you need

- macOS with Xcode Command Line Tools (`nm`, `c++filt`, `clang++`), Python 3.
- A MacKernelSDK checkout (`Headers/`), for `check_layout.py` only: `export C64_MKSDK=/path/to/MacKernelSDK`.
- A work directory with, from your own Mac:

```
$C64_WORK/kc/BootKernelExtensions.kc
$C64_WORK/kc/SystemKernelExtensions.kc
$C64_WORK/kexts/com.apple.iokit.IOAcceleratorFamily2     # standalone Mach-O carved from the KC
$C64_WORK/kexts/com.apple.driver.AppleParavirtGPU        # standalone Mach-O carved from the KC
```

The two kernel collections come from `/System/Library/KernelCollections/` (the paths differ per OS build; use the ones
that match the OS you will load on). For an x86_64 `MH_FILESET` system collection, create the two analysis images with:

```sh
mkdir -p "$C64_WORK/kexts"
python3 extract_fileset.py "$C64_WORK/kc/SystemKernelExtensions.kc" \
  com.apple.iokit.IOAcceleratorFamily2 "$C64_WORK/kexts/com.apple.iokit.IOAcceleratorFamily2"
python3 extract_fileset.py "$C64_WORK/kc/SystemKernelExtensions.kc" \
  com.apple.driver.AppleParavirtGPU "$C64_WORK/kexts/com.apple.driver.AppleParavirtGPU"
python3 test_extract_fileset.py
```

Each output has the full collection's file size. The extractor overlays the selected member's Mach-O header at offset
zero and preserves its segments, symbol-table offsets and chained pointers. These are local analysis inputs for `nm`
and the layout scripts; do not install them as kernel extensions. Existing output files are never overwritten.

## Run

```sh
export C64_WORK=$HOME/c64-work        # input directory above
export C64_LAYOUT_OUT=$PWD/layout-out # output directory (default ./layout-out)
export C64_MKSDK=$HOME/MacKernelSDK
python3 dump_vtables.py
python3 gen_header.py
python3 check_layout.py               # expect MATCH
```

The accelerator build and its gates use the same `C64_LAYOUT_OUT` directory.
Keep that export when running `make -C ../../src/garnet-accel` from this
directory. Without an override, that build looks in this directory's
`layout-out/`. `C64_WORK` must still point to the matching target system's
kernel collections and carved kexts; generating a header alone does not
establish binary compatibility.

Memory: `kcmod.Image` reads each input file into memory (the system KC is about 360 MB). Close other heavy jobs on a
16 GB machine.

## Notes

- Return types are unknown (C++ mangling omits them). Every recovered signature returns `GARNET_R_<Class>_<slot>`, which
  defaults to `uint64_t`; `#define` it before including the header to change it. The `GARNET_` prefix is the project's.
- Slot indices of the SDK base classes (`OSObject`, `IORegistryEntry`, `IOService`, `IOUserClient`) are looked up by method
  signature in your kernel's own `IOUserClient` vtable at run time, not stored.
- `kcmod.KERNEL_BASE` (`0xffffff8000100000`) is the level-0 chained-fixup base. It was verified against
  `___cxa_pure_virtual` on macOS 26.x. If `check_layout.py` reports nonsense on another OS build, check this first.
- Tested only on macOS 26.6.2 (25G83) x86_64. Other builds have not been tried.
