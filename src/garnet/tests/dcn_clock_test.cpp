#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cerrno>
#include "liveraster.h"
#include "dcn41_otg_timing.h"
#include "scanout_pure.h"
#include "GarnetNativeABI.h"
#include "amd/native_disp_pure.h"

static unsigned checks, failures, writes, reads;
static uint32_t regs[0x10000];
static uint32_t changedAddress = UINT32_MAX, changedValue, changedAfter;
static uint64_t nowNs = 1000000000ull;
static uint32_t readReg(void *, uint32_t a) {
    ++reads;
    if (a == changedAddress && reads >= changedAfter) return changedValue;
    return a < sizeof(regs) / sizeof(regs[0]) ? regs[a] : UINT32_MAX;
}
static void writeReg(void *, uint32_t, uint32_t) { ++writes; }
static void delay(void *, uint32_t) { ++writes; }
static void expect(bool ok, const char *label) {
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL: %s\n", label); }
}
static rdna4lr_ro gLr;
static struct { dcn41_dev d; bool armed; uint32_t seg[5]; } gDcn;
using IOLock = int;
static IOLock lock;
static struct {
    bool acquired, haveConsole;
    uint64_t consoleMc;
    rdna4scan::FcExt fc;
} gScan;
struct ScanGeom { uint32_t w, h, pitchPx, fmt, sw; bool dcc; } geometry;
static IOLock *scan_lock() { return &lock; }
static void IOLockLock(IOLock *) {}
static void IOLockUnlock(IOLock *) {}
static int lit_otg() {
    for (unsigned i = 0; i < 4; ++i) if (regs[0x34c0 + 0x1b43 + i * 0x80] & 1u) return i;
    return -1;
}
static void scan_read_geom(ScanGeom *g) { *g = geometry; }
static bool rdna4m6_latched_on() { return false; }
static bool rdna4m6flip_latched_on() { return false; }
static bool rdna4m6flip1_latched_on() { return false; }
namespace rdna4native { constexpr uint32_t kGateOn = 1; }
namespace amdgpu {
struct NativeState { uint32_t gate; };
static NativeState native_s1b_state() { return {rdna4native::kGateOn}; }
}
[[maybe_unused]] static uint32_t dcn_rreg(void *, uint32_t address) { return readReg(nullptr, address); }
static uint32_t liveRaster(uint32_t *ha, uint32_t *va, uint32_t *ht, uint32_t *vt,
                          uint32_t *hf, uint32_t *hs, uint32_t *vf, uint32_t *vs, uint64_t *clock) {
    return rdna4lr_live(&gLr, ha, va, ht, vt, hf, hs, vf, vs, clock);
}
static uint64_t now_ns() { return nowNs; }
static uint64_t mach_absolute_time() { return nowNs; }
static bool modeTrialBusy() { return false; }
#include "dcn_clock_actual.inc"

static void setup(unsigned otg, unsigned w, unsigned h, unsigned ht, unsigned vt,
                  unsigned hf, unsigned hs, unsigned vf, unsigned vs,
                  uint32_t integer, uint32_t phase, uint32_t modulo) {
    std::memset(regs, 0, sizeof(regs));
    std::memset(&gLr, 0, sizeof(gLr));
    std::memset(&gDcn, 0, sizeof(gDcn));
    std::memset(&gScan, 0, sizeof(gScan));
    gVbl = {};
    const uint32_t bases[5] = {0x12, 0xc0, 0x34c0, 0x9000, 0x2403c00};
    expect(rdna4lr_ro_build(&gLr, nullptr, readReg, bases, 0x10000) == 1, "build read-only device");
    expect(dcn41_dev_init(&gDcn.d, nullptr, readReg, writeReg, delay, bases, 0x10000, 0) == DCN41_OK, "build query device");
    gDcn.armed = true;
    std::memcpy(gDcn.seg, bases, sizeof(bases));
    const unsigned base = 0x34c0 + otg * 0x80;
    regs[base + 0x1b43] = 1;
    regs[base + 0x1b2a] = ht - 1;
    regs[base + 0x1b2f] = vt - 1;
    regs[base + 0x1b2b] = ((ht - hf - w) << 16) | (ht - hf);
    regs[base + 0x1b38] = ((vt - vf - h) << 16) | (vt - vf);
    regs[base + 0x1b2c] = hs << 16;
    regs[base + 0x1b39] = vs << 16;
    regs[0xc0 + 0x80 + otg * 4] = 0x1090;
    regs[0xc0 + 0x81 + otg * 4] = phase;
    regs[0xc0 + 0x82 + otg * 4] = modulo;
    regs[0xc0 + 0x6f] = integer << (1 + otg * 5);
    geometry = {w, h, w, 8, 0, false};
    reads = writes = 0;
    changedAddress = UINT32_MAX;
}
static void valid(unsigned otg, unsigned w, unsigned h, unsigned ht, unsigned vt,
                  unsigned hf, unsigned hs, unsigned vf, unsigned vs,
                  uint32_t integer, uint32_t phase, uint32_t modulo) {
    setup(otg, w, h, ht, vt, hf, hs, vf, vs, integer, phase, modulo);
    const uint64_t clock = static_cast<uint64_t>(integer) * modulo + phase;
    const uint64_t expectedPeriod = static_cast<uint64_t>(ht) * vt * 1000000000ull / clock;
    uint64_t period = UINT64_MAX, wait = UINT64_MAX, stamp = 0;
    expect(vblSample(&period, &wait, &stamp), "VBL accepts real DTO clock without a static timing row");
    expect(period == expectedPeriod, "VBL uses complete DTO rate");
    expect(stamp == nowNs && wait < expectedPeriod, "VBL timestamp and next blank are coherent");
    uint32_t ha = 0, va = 0, htotal = 0, vtotal = 0, hfront = 0, hsync = 0, vfront = 0, vsync = 0;
    uint64_t pixel = 0;
    expect(liveRaster(&ha, &va, &htotal, &vtotal, &hfront, &hsync, &vfront, &vsync, &pixel) == 1, "link reply supports a live raster absent from the compiled table");
    expect(ha == w && va == h && htotal == ht && vtotal == vt, "link raster matches hardware dimensions");
    expect(hfront == hf && hsync == hs && vfront == vf && vsync == vs && pixel == clock, "link reply uses live porches and full clock");
    rdna4n_scan_query query{};
    expect(scanQuery(&query) == 0, "query succeeds");
    expect(query.pix_clk_khz == clock / 1000 && query.refresh_mhz == rdna4scan::refresh_mhz(clock, ht, vt), "query uses the same complete clock as VBL");
    expect(query.flags & RDNA4N_SCANQ_DTO_VALID, "query DTO flag means all clock terms were read");
    expect(writes == 0, "clock and raster paths do not write or wait");
    std::printf("OTG%u %ux%u totals=%ux%u programmed_clock=%llu period_ns=%llu query_mHz=%u\n",
                otg, w, h, ht, vt, static_cast<unsigned long long>(clock),
                static_cast<unsigned long long>(period), query.refresh_mhz);
}
static bool rasterChanged;
static uint32_t transitionReg(void *cookie, uint32_t address) {
    if (!rasterChanged && address == 0xc0 + 0x80) {
        rasterChanged = true;
        regs[0x34c0 + 0x1b2a] = 2719;
        regs[0x34c0 + 0x1b2f] = 1524;
        regs[0x34c0 + 0x1b2b] = (112u << 16) | 2672u;
        regs[0x34c0 + 0x1b38] = (82u << 16) | 1522u;
        regs[0xc0 + 0x6f] = 4u << 1;
        regs[0xc0 + 0x81] = 97750000;
        geometry = {2560, 1440, 2560, 8, 0, false};
    }
    return readReg(cookie, address);
}
static void transitionSetup() {
    setup(0, 3840, 2160, 4000, 2222, 48, 32, 3, 5, 5, 33250000, 100000000);
    rasterChanged = false;
    gLr.d.rreg = transitionReg;
    gDcn.d.rreg = transitionReg;
}
int main() {
    // Mi Monitor preferred and 120 Hz DTDs, plus an integer-only rate.
    valid(0, 3840, 2160, 4000, 2222, 48, 32, 3, 5, 5, 33250000, 100000000);
    valid(0, 2560, 1440, 2720, 1525, 48, 32, 3, 5, 4, 97750000, 100000000);
    valid(0, 3840, 2160, 4400, 2250, 176, 88, 8, 10, 6, 0, 99000000);
    valid(0, 3840, 2160, 4400, 2250, 176, 88, 8, 10, 6, 0, 100000000);
    for (unsigned otg = 0; otg < 4; ++otg)
        valid(otg, 3000, 1700, 3200, 1750, 48, 32, 3, 5, 3, 36000000, 100000000);
    for (unsigned kind = 0; kind < 7; ++kind) {
        setup(0, 3840, 2160, 4000, 2222, 48, 32, 3, 5, 5, 33250000, 100000000);
        switch (kind) {
            case 0: regs[0xc0 + 0x80] = 0; break;
            case 1: regs[0xc0 + 0x82] = 0; break;
            case 2: regs[0xc0 + 0x81] = 100000000; break;
            case 3: regs[0xc0 + 0x6f] = UINT32_MAX; break;
            case 4: regs[0xc0 + 0x82] = UINT32_MAX; break;
            case 5: regs[0xc0 + 0x81] = UINT32_MAX; break;
            case 6: regs[0xc0 + 0x80] = UINT32_MAX; break;
        }
        uint64_t period = 123, wait = 456, stamp = 789;
        expect(!vblSample(&period, &wait, &stamp), "invalid or unavailable DTO refuses VBL");
        expect(period == 123 && wait == 456 && stamp == 789, "failed VBL preserves outputs");
        rdna4n_scan_query query{};
        expect(scanQuery(&query) == 0 && !(query.flags & RDNA4N_SCANQ_DTO_VALID), "invalid DTO is never marked valid in query");
        expect(writes == 0, "invalid DTO remains read-only");
    }
    setup(0, 3840, 2160, 4000, 2222, 48, 32, 3, 5, 5, 33250000, 100000000);
    gLr.d.mmio_dwords = 0x140;
    uint64_t period = 0, wait = 0, stamp = 0;
    expect(!vblSample(&period, &wait, &stamp), "out-of-window clock/raster refuses access");
    expect(writes == 0, "all paths remain read-only");
    for (unsigned field = 0; field < 4; ++field) {
        setup(0, 3840, 2160, 4000, 2222, 48, 32, 3, 5, 5, 33250000, 100000000);
        const uint32_t address[4] = {0xc0 + 0x80, 0xc0 + 0x6f, 0xc0 + 0x81, 0xc0 + 0x82};
        changedAddress = address[field];
        changedValue = regs[changedAddress] ^ (field == 0 ? 0x1000u : field == 1 ? 2u : 1u);
        changedAfter = 5;
        uint64_t clock = 123;
        expect(!rdna4lr_dp_clock(&gLr, 0, &clock) && clock == 123, "changed DTO snapshot refuses without output");
    }
    setup(0, 3840, 2160, 4000, 2222, 48, 32, 3, 5, 5, 33250000, 100000000);
    changedAddress = 0xc0 + 0x80;
    changedValue = regs[changedAddress] | 0x10000u;
    changedAfter = 5;
    uint64_t clock = 0;
    expect(rdna4lr_dp_clock(&gLr, 0, &clock) && clock == 533250000, "changing FIFO counters do not invalidate the clock");
    expect(!rdna4lr_dp_clock(&gLr, 4, &clock) && !rdna4lr_dp_clock(nullptr, 0, &clock) &&
           !rdna4lr_dp_clock(&gLr, 0, nullptr), "clock rejects invalid instance and outputs");
    transitionSetup();
    period = 123; wait = 456; stamp = 789;
    expect(!vblSample(&period, &wait, &stamp) && rasterChanged, "VBL refuses a mode change between totals and DTO");
    expect(period == 123 && wait == 456 && stamp == 789, "mixed-mode VBL preserves outputs");
    expect(vblSample(&period, &wait, &stamp) && period == 8333500, "VBL recovers on the next stable 120 Hz raster");
    transitionSetup();
    rdna4n_scan_query query{};
    expect(scanQuery(&query) == rdna4scan::kNotReady && rasterChanged, "query refuses mixed old totals and new DTO");
    expect(!(query.flags & RDNA4N_SCANQ_DTO_VALID) && !query.refresh_mhz, "mixed-mode query exposes no valid clock or refresh");
    expect(scanQuery(&query) == 0 && query.h_total == 2720 && query.v_total == 1525 &&
           query.pix_clk_khz == 497750 && query.refresh_mhz == 119997, "query recovers with coherent 120 Hz values");
    std::printf("dcn_clock: %u checks, %u failed\n", checks, failures);
    return failures ? 1 : 0;
}
