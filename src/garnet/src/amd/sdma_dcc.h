// sdma_dcc.h — the SDMA0_DCC_CNTL no-PTE compression arithmetic the `accel sdmadcc` verb and its host
// test drive (SDMA DCC control). No kernel headers, no hardware:
// the kext (GarnetDriver.cpp) and the host test (tests/sdma_dcc_test.cpp) compile this same file, so
// there is no second implementation of the mask or the argument rules that could drift from the one
// that runs.
//
// WHY THIS EXISTS. Our SDMA0 QUEUE0 submits VMID-0, FB-aperture, no-PTE accesses. On this silicon
// SDMA0_DCC_CNTL's set-0 no-PTE read/write COMPRESSION bits are enabled, so every such read is
// DECOMPRESSED and every such write is COMPRESSED (: a uniform block returns its clear
// constant, not its bytes). Clearing only the eight *_COMP_EN_n bits leaves the *_OVERRIDE_n bits and
// DCC_FORCE_BYPASS exactly as they are, so no-PTE accesses go raw and Apple's PTE-translated work is
// untouched. The spec calls this `0xaabe` -> `0xaaaa`.
//
// REGISTER AND FIELDS, from the vendored headers (documentation only, never included by the kext):
//   regSDMA0_DCC_CNTL = 0x0034, regSDMA0_DCC_CNTL_BASE_IDX = 0   gc_12_0_0_offset.h:88-89
//   regSDMA1_DCC_CNTL = 0x0634, regSDMA1_DCC_CNTL_BASE_IDX = 0   gc_12_0_0_offset.h:1070-1071
//   fields: DCC_FORCE_BYPASS bit 0; for n=0..3, RD_NOPTE_OVERRIDE_n bit 1+4n,
//   RD_NOPTE_COMP_EN_n bit 2+4n, WR_NOPTE_OVERRIDE_n bit 3+4n, WR_NOPTE_COMP_EN_n bit 4+4n
//   gc_12_0_0_sh_mask.h:334-367 (SDMA0), :3243-3276 (SDMA1, identical layout).
// The eight COMP_EN masks sum to 0x00015554 exactly; the eight OVERRIDE masks sum to 0x0000AAAA and
// bit 0 is the bypass, so the compression mask shares no bit with either (asserted by the host test).
#ifndef RDNA4_SDMA_DCC_H
#define RDNA4_SDMA_DCC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The two registers, at GC BASE_IDX 0 (same GC base mechanism the rest of the kext uses).
#define RDNA4_SDMA_DCC_CNTL_OFF   0x0034u
#define RDNA4_SDMA1_DCC_CNTL_OFF  0x0634u

// The fields the report decodes, and the one mask the verb clears. Every value below is the OR of the
// named field masks in gc_12_0_0_sh_mask.h; the host test re-derives them.
#define RDNA4_DCC_FORCE_BYPASS_MASK      0x00000001u
#define RDNA4_DCC_NOPTE_OVERRIDE_MASK    0x0000AAAAu   // bits 1,3,5,7,9,11,13,15
#define RDNA4_DCC_NOPTE_COMP_EN_MASK     0x00015554u   // bits 2,4,6,8,10,12,14,16 -- THE MASK THE VERB CLEARS
// Every bit the layout defines (bypass + all overrides + all comp enables). Asserted == 0x1FFFF.
#define RDNA4_DCC_DEFINED_MASK \
    (RDNA4_DCC_FORCE_BYPASS_MASK | RDNA4_DCC_NOPTE_OVERRIDE_MASK | RDNA4_DCC_NOPTE_COMP_EN_MASK)

// ---------------------------------------------------------------------------------------------------
// D1's arithmetic. `cleared` is the value `sdmadcc 1` writes; `restore` is the captured original.
// ---------------------------------------------------------------------------------------------------
static inline uint32_t rdna4_sdma_dcc_cleared(uint32_t v)
{
    return v & ~RDNA4_DCC_NOPTE_COMP_EN_MASK;
}

// Per-set field accessors for the report (set 0..3). Out of range reads as 0 rather than shifting UB.
static inline uint32_t rdna4_sdma_dcc_rd_override(uint32_t v, unsigned set)
{
    return set < 4u ? ((v >> (1u + 4u * set)) & 1u) : 0u;
}
static inline uint32_t rdna4_sdma_dcc_rd_comp(uint32_t v, unsigned set)
{
    return set < 4u ? ((v >> (2u + 4u * set)) & 1u) : 0u;
}
static inline uint32_t rdna4_sdma_dcc_wr_override(uint32_t v, unsigned set)
{
    return set < 4u ? ((v >> (3u + 4u * set)) & 1u) : 0u;
}
static inline uint32_t rdna4_sdma_dcc_wr_comp(uint32_t v, unsigned set)
{
    return set < 4u ? ((v >> (4u + 4u * set)) & 1u) : 0u;
}
static inline uint32_t rdna4_sdma_dcc_force_bypass(uint32_t v)
{
    return v & RDNA4_DCC_FORCE_BYPASS_MASK;
}

// ---------------------------------------------------------------------------------------------------
// D1's argument rules. `0` reads both registers, `1` captures-then-clears SDMA0, `2` restores the
// captured value. Anything else is REFUSED, and `2` before any capture is REFUSED (there is nothing
// to restore). Pure so tests/sdma_dcc_test.cpp drives the identical rule the kext dispatches on.
// ---------------------------------------------------------------------------------------------------
enum {
    kRDNA4DccOpRefused = 0,
    kRDNA4DccOpRead    = 1,
    kRDNA4DccOpSet     = 2,
    kRDNA4DccOpRestore = 3,
};

static inline uint32_t rdna4_sdma_dcc_op(uint64_t arg, int captured)
{
    if (arg == 0u) return kRDNA4DccOpRead;
    if (arg == 1u) return kRDNA4DccOpSet;
    if (arg == 2u) return captured ? kRDNA4DccOpRestore : kRDNA4DccOpRefused;
    return kRDNA4DccOpRefused;
}

// ---------------------------------------------------------------------------------------------------
// E1 (0.0.418, driver design) — THE CLEAR IS THE DEFAULT. `rdna4-sdmadcc` is parsed with
// PE_parse_boot_argn exactly as this project parses its other boot-args: absent => the default (ON), an
// explicit 0 => skipped, any other value => ON. Pure so tests/sdma_dcc_test.cpp drives the same rule the
// kext dispatches on. The value the default writes is `rdna4_sdma_dcc_cleared(captured)`, the SAME arithmetic
// `sdmadcc 1` writes; only the site and the log line differ.
// ---------------------------------------------------------------------------------------------------
static inline uint32_t rdna4_sdma_dcc_default_on(uint32_t boot_present, uint32_t boot_value)
{
    if (!boot_present) return 1u;      // no boot-arg: the scanout fix is on
    return boot_value != 0u ? 1u : 0u; // `rdna4-sdmadcc=0` skips it; anything else runs it
}

// The verb's status, returned to the caller and mirrored into its out-scalars.
enum {
    kRDNA4DccStOk        = 0,
    kRDNA4DccStBadArg    = 1,   // argument other than 0/1/2, or `2` before a capture
    kRDNA4DccStNoContext = 2,   // no bring-up DeviceContext
    kRDNA4DccStNoGc      = 3,   // GC BASE_IDX 0 did not resolve
    kRDNA4DccStMismatch  = 4,   // the read-back after a write did not equal what was written
};

#ifdef __cplusplus
}
#endif

#endif // RDNA4_SDMA_DCC_H
