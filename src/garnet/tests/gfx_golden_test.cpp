#include "amdgpu_gfx.h"
#include "amdgpu_asic.h"
#include <cstdio>
#include <map>
#include <vector>
#include <cstdarg>
#include <string>

namespace {
struct Access { char op; uint32_t reg, value; };
std::map<uint32_t, uint32_t> registers;
std::vector<Access> trace;
unsigned failures;
std::vector<std::string> messages;

uint32_t trace_read(const amdgpu::DeviceContext &, uint32_t reg) {
    const uint32_t value = registers[reg];
    trace.push_back({'R', reg, value});
    return value;
}
void trace_write(const amdgpu::DeviceContext &, uint32_t reg, uint32_t value) {
    registers[reg] = value;
    trace.push_back({'W', reg, value});
}
uint32_t trace_smn_read(const amdgpu::DeviceContext &, uint32_t reg) {
    const uint32_t value = registers[reg];
    trace.push_back({'S', reg, value});
    return value;
}
void expect(bool condition, const char *message) {
    if (!condition) { ++failures; std::printf("FAIL: %s\n", message); }
}
}

// Replace only the hardware boundary while exercising the real GFXInit callee.
#define RREG32 trace_read
#define WREG32 trace_write
#define SMN_RREG32 trace_smn_read
#include "../src/amd/gfx_v12_0.cpp"
#undef SMN_RREG32
#undef WREG32
#undef RREG32

namespace amdgpu {
void rdna4_logf(const char *fmt, ...) {
    char text[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    messages.emplace_back(text);
}
}

namespace {
amdgpu::DeviceContext fixture(uint32_t strap) {
    registers.clear(); trace.clear();
    amdgpu::DeviceContext dev;
    dev.ip.setVersion(amdgpu::IPBlock::GC, {12, 0, 0});
    dev.ip.setVersion(amdgpu::IPBlock::NBIO, {6, 3, 1});
    dev.ip.setBase(amdgpu::IPBlock::GC, 0, 0x1000);
    dev.ip.setBase(amdgpu::IPBlock::GC, 1, 0x5000);
    dev.ip.setBase(amdgpu::IPBlock::NBIO, 2, 0x10000);
    static uint32_t mmio;
    dev.rmmio = &mmio;
    dev.rmmioSize = 0x80000;
    dev.gc = {true, false, 0, 0, 2, 2, 4, 8, 16};
    registers[0x7200] = 0xe0000000;
    registers[0x23de] = 0x08200545;
    registers[0x1001c] = strap;
    registers[0x23d2] = 0xa5a50050;
    registers[0x2425] = 0x5c123456;
    registers[0x7e19] = 0xcafeffdf;
    return dev;
}

std::vector<Access> golden_writes() {
    std::vector<Access> writes;
    for (const auto &a : trace)
        if (a.op == 'W' && (a.reg == 0x23d2 || a.reg == 0x2425 || a.reg == 0x7e19))
            writes.push_back(a);
    return writes;
}

void revision_zero() {
    // High PCI-style revision bits are deliberately nonzero; silicon revision is zero.
    auto dev = fixture(0xf0123456);
    amdgpu::GFXConfig cfg {};
    expect(amdgpu::gfx_constants_init(dev, cfg) == kIOReturnSuccess && cfg.inited,
           "GFXInit succeeds for GC12.0.0 silicon revision zero");
    const auto writes = golden_writes();
    const Access expected[] = {
        {'W', 0x23d2, 0xa5a58050},
        {'W', 0x23d2, 0xa5a5805f},
        {'W', 0x2425, 0x5f123456},
        {'W', 0x7e19, 0xcafeffaf},
    };
    expect(writes.size() == 4, "revision zero emits exactly four golden writes (previously missing)");
    for (size_t i = 0; i < writes.size() && i < 4; ++i) {
        std::printf("GOLDEN rev0 write[%zu] reg=%#x value=%#010x\n", i, writes[i].reg, writes[i].value);
        expect(writes[i].reg == expected[i].reg && writes[i].value == expected[i].value,
               "golden write matches Linux v6.17 address, ordering and masked value");
    }
    const uint32_t before[] = {0xa5a50050, 0xa5a58050, 0x5c123456, 0xcafeffdf};
    size_t next = 0;
    for (size_t i = 0; i < trace.size() && next < 4; ++i) {
        if (trace[i].op != 'W') continue;
        expect(trace[i].reg == expected[next].reg && trace[i].value == expected[next].value,
               "goldens are the first GFXInit writes, before existing constants setup");
        expect(i > 0 && trace[i - 1].op == 'R' && trace[i - 1].reg == expected[next].reg &&
               trace[i - 1].value == before[next], "each masked write reads the current value of the same register");
        ++next;
    }
    expect(next == 4, "complete read/modify/write trace observed");
}

void later_revisions() {
    for (uint32_t strap : {0x01012345u, 0x0f012345u}) {
        auto dev = fixture(strap);
        amdgpu::GFXConfig cfg {};
        expect(amdgpu::gfx_constants_init(dev, cfg) == kIOReturnSuccess && cfg.inited,
               "GFXInit succeeds for later silicon revisions");
        const auto writes = golden_writes();
        expect(writes.size() == 1 && writes[0].reg == 0x23d2 && writes[0].value == 0xa5a58050,
               "later revisions receive only the common DB_MEM_CONFIG write");
        expect(registers[0x2425] == 0x5c123456 && registers[0x7e19] == 0xcafeffdf,
               "revision-zero CB and GL2 fields remain untouched on later silicon");
        std::printf("GOLDEN strap=%#010x common writes=%zu\n", strap, writes.size());
    }
}

void smn_revision() {
    auto dev = fixture(0);
    dev.ip.setBase(amdgpu::IPBlock::NBIO, 2, 0x18000000);
    registers[0x1800001c] = 0xf0000000;
    amdgpu::GFXConfig cfg {};
    expect(amdgpu::gfx_constants_init(dev, cfg) == kIOReturnSuccess && cfg.inited,
           "revision strap outside BAR5 uses existing SMN access");
    unsigned smnReads = 0;
    for (const auto &a : trace) if (a.op == 'S') {
        ++smnReads;
        expect(a.reg == 0x1800001c && a.value == 0xf0000000, "SMN read uses discovered NBIF segment 2");
    }
    expect(smnReads == 1 && golden_writes().size() == 4, "SMN revision zero gets the same golden sequence");
    std::printf("GOLDEN indirect strap reads=%u writes=%zu\n", smnReads, golden_writes().size());
}

void guards() {
    for (unsigned scenario = 0; scenario < 7; ++scenario) {
        auto dev = fixture(0);
        kern_return_t expected = kIOReturnNotReady;
        switch (scenario) {
        case 0: registers[0x1001c] = 0xffffffff; expected = kIOReturnNoDevice; break;
        case 1: dev.ip.setBase(amdgpu::IPBlock::NBIO, 2, 0xffffffff); break;
        case 2: dev.ip.setBase(amdgpu::IPBlock::GC, 1, 0xffffffff); break;
        case 3: dev.rmmio = nullptr; break;
        case 4: dev.rmmioSize = 0x1f864; break;
        case 5: dev.ip.setVersion(amdgpu::IPBlock::NBIO, {7, 11, 0}); expected = kIOReturnUnsupported; break;
        case 6: registers[0x7200] = 0xffffffff; expected = kIOReturnNoDevice; break;
        }
        amdgpu::GFXConfig cfg {};
        expect(amdgpu::gfx_constants_init(dev, cfg) == expected && !cfg.inited,
               "unusable revision, mapping or GC probe refuses GFXInit");
        unsigned writes = 0;
        for (const auto &a : trace) if (a.op == 'W') ++writes;
        expect(writes == 0, "failed preflight issues no register writes");
        std::printf("GOLDEN guard=%u writes=%u\n", scenario, writes);
    }
}

void other_gc_version() {
    auto dev = fixture(0);
    dev.ip.setVersion(amdgpu::IPBlock::GC, {12, 0, 1});
    amdgpu::GFXConfig cfg {};
    expect(amdgpu::gfx_constants_init(dev, cfg) == kIOReturnSuccess && cfg.inited,
           "existing GC12.0.1 constants path is preserved");
    expect(golden_writes().empty(), "Navi44 golden port does not apply a blanket Navi48 table");
    std::printf("GOLDEN GC12.0.1 writes=%zu\n", golden_writes().size());
}
}

void chip_fallback_geometry() {
    // Missing discovery (dev.gc invalid): selected chip's fallback row decides.
    auto dev = fixture(0);
    dev.gc = {};
    amdgpu::asic_select(amdgpu::asic_by_id(amdgpu::AsicId::Navi44));
    amdgpu::GFXConfig cfg {};
    expect(amdgpu::gfx_gpu_early_init(dev, cfg) == kIOReturnSuccess, "Navi44 fallback geometry initializes");
    expect(cfg.max_shader_engines == 2 && cfg.max_sh_per_se == 2 && cfg.max_cu_per_sh == 8 &&
           cfg.max_backends_per_se == 4, "Navi44 fallback is 2 SE x 2 SA x 8 CU, 4 RB/SE");
    amdgpu::asic_select(amdgpu::asic_by_id(amdgpu::AsicId::Navi48));
    expect(amdgpu::gfx_gpu_early_init(dev, cfg) == kIOReturnSuccess, "Navi48 fallback geometry initializes");
    expect(cfg.max_shader_engines == 4 && cfg.max_sh_per_se == 2 && cfg.max_cu_per_sh == 8 &&
           cfg.max_backends_per_se == 4, "Navi48 fallback is 4 SE x 2 SA x 8 CU, 4 RB/SE");
    // Discovery present still wins over the fallback.
    auto dev2 = fixture(0);
    amdgpu::GFXConfig cfg2 {};
    expect(amdgpu::gfx_gpu_early_init(dev2, cfg2) == kIOReturnSuccess, "discovery geometry initializes");
    expect(cfg2.max_shader_engines == 2, "discovery gc_info wins over chip fallback");
    std::printf("GOLDEN fallback se=%u/%u\n", cfg.max_shader_engines, cfg2.max_shader_engines);
}

void no_chip_no_geometry() {
    // Neither discovery geometry nor a selected chip: fail, never guess.
    auto dev = fixture(0);
    dev.gc = {};
    amdgpu::asic_select(nullptr);
    amdgpu::GFXConfig cfg {};
    expect(amdgpu::gfx_gpu_early_init(dev, cfg) != kIOReturnSuccess, "no chip, no geometry refuses to guess");
    expect(cfg.max_shader_engines == 0 && cfg.max_sh_per_se == 0, "refusal leaves caps zeroed");
    // Restore a chip so later tests (and any re-run) see explicit state.
    amdgpu::asic_select(amdgpu::asic_by_id(amdgpu::AsicId::Navi44));
}

void address_layout_is_not_physical_geometry() {
    auto dev = fixture(0x01012345);
    amdgpu::GFXConfig cfg {};
    messages.clear();
    expect(amdgpu::gfx_constants_init(dev, cfg) == kIOReturnSuccess,
           "physical log GB_ADDR_CONFIG initializes");
    expect(cfg.gb_addr_config_raw == 0x08200545 && cfg.num_shader_engines == 16,
           "address-layout encoding is preserved for tiling");
    expect(cfg.max_shader_engines == 2 && cfg.num_active_cus == 32,
           "physical geometry comes from discovery independently of address layout");
    for (const auto &message : messages)
        expect(message.find("WARNING GB_ADDR_CONFIG") == std::string::npos,
               "address layout is not incorrectly compared with physical geometry");
}

int main() {
    revision_zero();
    later_revisions();
    smn_revision();
    guards();
    other_gc_version();
    chip_fallback_geometry();
    no_chip_no_geometry();
    address_layout_is_not_physical_geometry();
    std::printf("gfx golden: %u failed; simulated MMIO only\n", failures);
    return failures ? 1 : 0;
}
