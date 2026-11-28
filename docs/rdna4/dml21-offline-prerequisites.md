# DCN 4.01 DML2.1: host solver and kernel boundary

The Garnet host suite runs the 15 DCN 4.01 translation units from Linux `9437ee8b6cf607e883b36e44bd755968daaebd48`. `Garnet.kext` does **not** contain or call the solver. The [import notes](../../src/garnet/src/dcn/dml21/README.md) record provenance and differences from Linux.

## Run the comparison

`make -C src/garnet/tests all` runs the port under ASan and UBSan. It checks 4K60, 4K160 DSC, and simultaneous DP + HDMI, each with a 32 MiB or 64 MiB MALL profile. The expected values come from compiling the **unmodified** Linux solver separately. The [reference recipe](../../src/garnet/tests/dml21-oracle/README.md) gives the exact inputs, commands, and sanitizer caveats for upstream. All reported watermarks, DET/COMPBUF sizes, P-state methods, MALL byte counts, and pre-routing FAMS2 bytes are compared; these are not hardware traces.

The profiles model unscaled linear ARGB8888 surfaces and fixed clocks and latencies. They do not validate an actual link, measure power, identify a physical HUBP/OTG, or authorize a UCLK change. A real modeset needs a consistent snapshot of active planes, links, clock tables, ATOM memory geometry, and latencies. An idle clock reading or a chip's physical MALL capacity cannot replace that snapshot.

## Check kernel compilation without enabling execution

With the cross-toolchain configured, run `make -C src/garnet check-dml21-kernel check-kernel-fp`. The first target compiles the solver and adapter in isolation with SSE2, no AVX, and a 3072-byte per-frame limit. The second checks every object actually linked into the kext for FP/SIMD instructions. The host entry allocates the large contexts on the heap; it is excluded from the kext.

On the examined Darwin 25.6 x86_64 kernel, XNU manages extended processor state during context switches and `#NM` handling. Its kext interfaces expose interrupt controls, but no verified way for this driver to acquire FPU ownership. A driver lock, disabling interrupts, or saving with `fxsave64` does not establish ownership. Do not call the solver from the kernel on that basis.
