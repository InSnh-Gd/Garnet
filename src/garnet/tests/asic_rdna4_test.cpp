// asic_rdna4_test.cpp — the per-chip table (src/amd/amdgpu_asic.{h,cpp}), the discovery gc_info parser and the
// firmware table's per-chip selection (src/amd/fw_table.cpp), offline. The firmware blobs are stood in by tiny arrays
// defined below, so no linux-firmware checkout is needed.
//
//     clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//         -I src/garnet/src -I src/garnet/src/amd \
//         src/garnet/tests/asic_rdna4_test.cpp src/garnet/src/amd/amdgpu_asic.cpp \
//         src/garnet/src/amd/fw_table.cpp -o /tmp/asictest && /tmp/asictest
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include "amdgpu_asic.h"
#include "fw/fw_table.h"

using namespace amdgpu;

static int gFail = 0, gRun = 0;
static void expect(bool ok, const char *what) {
    gRun++;
    if (!ok) { gFail++; std::printf("FAIL  %s\n", what); } else std::printf("ok    %s\n", what);
}

// ---- stand-ins for the generated $(BUILD)/firmware/fw_<blob>.c files ----------------------------------------------------
#define BLOB(sym, n) extern "C" { extern const uint8_t fw_##sym[]; extern const size_t fw_##sym##_size; \
    const uint8_t fw_##sym[64] = { 1 }; const size_t fw_##sym##_size = (n); }
BLOB(psp_14_0_3_sos, 11) BLOB(psp_14_0_3_ta, 12) BLOB(gc_12_0_1_imu, 13) BLOB(gc_12_0_1_pfp, 14)
BLOB(gc_12_0_1_me, 15) BLOB(gc_12_0_1_mec, 16) BLOB(gc_12_0_1_uni_mes, 17) BLOB(gc_12_0_1_rlc, 18)
BLOB(smu_14_0_3, 19) BLOB(sdma_7_0_1, 20)
// Navi 44 built in, except the TA (a stub of size 0, as the Makefile emits for an absent blob).
BLOB(psp_14_0_2_sos, 21) BLOB(psp_14_0_2_ta, 0) BLOB(gc_12_0_0_imu, 23) BLOB(gc_12_0_0_pfp, 24)
BLOB(gc_12_0_0_me, 25) BLOB(gc_12_0_0_mec, 26) BLOB(gc_12_0_0_uni_mes, 27) BLOB(gc_12_0_0_rlc, 28)
BLOB(smu_14_0_2, 29) BLOB(sdma_7_0_0, 30)
BLOB(dcn_4_0_1_dmcub, 31)

// ---- a synthetic discovery binary carrying a gc_info table ---------------------------------------------------
static void put16(std::vector<uint8_t> &b, size_t o, uint16_t v) { b[o] = v & 0xff; b[o + 1] = v >> 8; }
static void put32(std::vector<uint8_t> &b, size_t o, uint32_t v) { for (int i = 0; i < 4; i++) b[o + i] = (v >> (8 * i)) & 0xff; }
static void seal_binary(std::vector<uint8_t> &b) {
    const size_t size = b[10] | (b[11] << 8);
    uint16_t sum = 0;
    for (size_t i = 10; i < size; ++i) sum = static_cast<uint16_t>(sum + b[i]);
    put16(b, 8, sum);
}

// hdrMajor: binary_header version (1: list @ +12, 2: num_tables + list @ +16). gcMajor: gc_info 1 or 2.
static std::vector<uint8_t> make_bin(uint16_t hdrMajor, uint16_t gcMajor, const uint32_t *fields, size_t nfields,
                                     bool breakChecksum = false, uint32_t tableId = 0x4347) {
    std::vector<uint8_t> b(1024, 0);
    put32(b, 0, 0x28211407u);
    put16(b, 4, hdrMajor);
    put16(b, 10, static_cast<uint16_t>(b.size()));
    size_t list = 12;
    if (hdrMajor >= 2) { put16(b, 12, 7); list = 16; }
    const uint16_t off = 0x100;
    const uint32_t gsize = 12 + 4 * (uint32_t)nfields;
    put32(b, off, tableId);
    put16(b, off + 4, gcMajor);
    put16(b, off + 6, 0);
    put32(b, off + 8, gsize);
    for (size_t i = 0; i < nfields; i++) put32(b, off + 12 + 4 * i, fields[i]);
    uint16_t sum = 0;
    for (uint32_t i = 0; i < gsize; i++) sum = (uint16_t)(sum + b[off + i]);
    if (breakChecksum) sum ^= 1;
    put16(b, list + 8, off);      // table_list[GC].offset
    put16(b, list + 10, sum);     // .checksum
    put16(b, list + 12, (uint16_t)gsize);
    seal_binary(b);
    return b;
}

int main() {
    // ---- explicit selection: no default chip ----
    asic_select(nullptr);
    expect(asic_current() == nullptr, "unselected: no default chip");
    expect(!asic_is_selected(), "unselected: is_selected is false");
    {
        uint8_t mj = 0, mn = 0, rv = 0;
        expect(!asic_expected_mp0(&mj, &mn, &rv), "unselected: no expected MP0");
    }
    expect(fw_get(FwId::PSP_SOS) == nullptr, "unselected: fw_get fails instead of aliasing a chip");
    expect(fw_find_by_name("smu_14_0_3") == nullptr, "unselected: fw_find_by_name fails");
    expect(!fw_chip_complete(nullptr), "unselected: chip_complete is false");
    {
        AsicInfo unknown {};
        unknown.id = AsicId::Unknown;
        expect(!fw_chip_complete(&unknown), "unknown chip id: chip_complete is false");
        expect(asic_by_id(AsicId::Unknown) == nullptr, "unknown chip id: no table entry");
    }

    // ---- table lookups ----
    const AsicInfo *n48 = asic_from_pci(0x1002, 0x7550), *n48p = asic_from_pci(0x1002, 0x7551);
    const AsicInfo *n44 = asic_from_pci(0x1002, 0x7590);
    expect(n48 && n48->id == AsicId::Navi48 && n48p == n48, "7550 / 7551 -> Navi 48");
    expect(n44 && n44->id == AsicId::Navi44, "7590 -> Navi 44");
    expect(!asic_from_pci(0x1002, 0x7480) && !asic_from_pci(0x10de, 0x7590), "other ids / vendors -> none");
    expect(asic_from_gc(12, 0, 1) == n48 && asic_from_gc(12, 0, 0) == n44 && !asic_from_gc(11, 0, 0), "GC 12.0.1 / 12.0.0 / 11.0.0");
    expect(n44->mp0Major == 14 && n44->mp0Minor == 0 && n44->mp0Rev == 2 && n48->mp0Rev == 3, "MP0 14.0.2 / 14.0.3");
    expect(n44->pspFwReservMinSos == 0x3b0e0d && n48->pspFwReservMinSos == 0x3a0e14, "FB_FW_RESERV_ADDR SOS thresholds (amdgpu_psp.c)");
    expect(std::strcmp(asic_pci_name(n44, 0x7590), "RX 9060 XT") == 0, "7590 is called RX 9060 XT");
    expect(std::strcmp(asic_pci_name(nullptr, 0x7590), "unknown") == 0, "null chip has no PCI name");
    expect(n44->fallbackGeometry.max_shader_engines == 2 && n48->fallbackGeometry.max_shader_engines == 4, "fallback SE count 2 / 4");
    expect(n44->fallbackGeometry.valid && n48->fallbackGeometry.valid, "both fallback geometries valid");

    // ---- firmware selection: Navi 48 ----
    asic_select(n48);
    expect(asic_current() == n48 && asic_is_selected(), "select Navi 48");
    {
        uint8_t mj = 0, mn = 0, rv = 0;
        expect(asic_expected_mp0(&mj, &mn, &rv) && mj == 14 && mn == 0 && rv == 3, "Navi 48: expected MP0 14.0.3");
    }
    expect(std::strcmp(fw_get(FwId::PSP_SOS)->name, "psp_14_0_3_sos") == 0 && fw_get(FwId::PSP_SOS)->size == 11, "Navi 48: PSP_SOS = psp_14_0_3_sos");
    expect(std::strcmp(fw_get(FwId::DCN_DMUB)->name, "dcn_4_0_1_dmcub") == 0 && fw_get(FwId::DCN_DMUB)->size == 31, "Navi 48: DCN_DMUB = dcn_4_0_1_dmcub");
    // ---- firmware selection: Navi 44 ----
    asic_select(n44);
    expect(asic_current() == n44, "select Navi 44");
    {
        uint8_t mj = 0, mn = 0, rv = 0;
        expect(asic_expected_mp0(&mj, &mn, &rv) && mj == 14 && mn == 0 && rv == 2, "Navi 44: expected MP0 14.0.2");
    }
    expect(std::strcmp(fw_get(FwId::PSP_SOS)->name, "psp_14_0_2_sos") == 0 && fw_get(FwId::PSP_SOS)->size == 21, "Navi 44: PSP_SOS = psp_14_0_2_sos");
    expect(std::strcmp(fw_get(FwId::GC_RLC)->name, "gc_12_0_0_rlc") == 0 && fw_get(FwId::SDMA)->size == 30, "Navi 44: GC / SDMA names follow");
    expect(std::strcmp(fw_get(FwId::DCN_DMUB)->name, "dcn_4_0_1_dmcub") == 0 && fw_get(FwId::DCN_DMUB)->size == 31, "Navi 44: shared DCN_DMUB = dcn_4_0_1_dmcub");
    expect(fw_get(FwId::PSP_TA)->size == 0, "Navi 44: a stubbed blob reads as size 0");
    expect(!fw_chip_complete(n44), "Navi 44 with one stubbed required blob is NOT complete");
    expect(fw_chip_complete(n48), "Navi 48 with all blobs is complete");
    expect(fw_find_by_name("smu_14_0_2") && fw_find_by_name("smu_14_0_2")->size == 29, "find_by_name sees the selected chip");
    asic_select(n48);
    expect(fw_find_by_name("smu_14_0_2") == nullptr, "chip switch changes the visible firmware table");
    asic_select(n44);
    expect(fw_get(FwId::GC_MES)->data == nullptr && fw_get(FwId::GC_TOC)->data == nullptr, "MES / MES1 / TOC stay empty");
    asic_select(n48);
    expect(fw_get(FwId::SMU)->size == 19, "switch back to Navi 48");
    // ---- clearing the selection fails lookups again ----
    asic_select(nullptr);
    expect(fw_get(FwId::PSP_SOS) == nullptr, "cleared selection: fw_get fails");
    asic_select(n48);

    // ---- gc_info ----
    // gc_info_v1_x fields: se, wgp0/sa, wgp1/sa, rb/se, gl2c, gprs, gs_thds, gs_tbl, gsprim, param, dbl_offchip, wave,
    // waves/simd, scratch, lds, sc/se, sa/se, packer/sc, gl2a  (19 = v1_0)
    const uint32_t n44v1[19] = { 2, 4, 0, 4, 16, 1536, 32, 32, 64, 1024, 1, 32, 16, 32, 64, 2, 2, 4, 4 };
    GcGeometry g;
    std::vector<uint8_t> b = make_bin(1, 1, n44v1, 19);
    expect(gc_info_parse(b.data(), b.size(), g), "v1 header, gc_info 1.0 parses");
    expect(g.valid && g.fromDiscovery && g.max_shader_engines == 2 && g.max_sh_per_se == 2 && g.max_cu_per_sh == 8 &&
           g.max_backends_per_se == 4 && g.num_gl2c == 16, "  -> 2 SE x 2 SA x 8 CU, 4 RB/SE, 16 TCC (the 9060 XT of issue #6)");
    b = make_bin(2, 1, n44v1, 19);
    expect(gc_info_parse(b.data(), b.size(), g) && g.max_shader_engines == 2, "v2 header (list @ +16) parses");
    const uint32_t v2[17] = { 4, 8, 2, 4, 32, 1536, 32, 32, 64, 1024, 1, 32, 16, 32, 64, 2, 4 };
    b = make_bin(1, 2, v2, 17);
    expect(gc_info_parse(b.data(), b.size(), g) && g.max_shader_engines == 4 && g.max_cu_per_sh == 8 && g.max_sh_per_se == 2 &&
           g.max_backends_per_se == 4, "gc_info 2.0: se / cu_per_sh / sh_per_se / rb_per_se");
    b = make_bin(1, 1, n44v1, 19, true);
    expect(!gc_info_parse(b.data(), b.size(), g) && !g.valid, "bad checksum is refused");
    b = make_bin(1, 1, n44v1, 19, false, 0x1234);
    expect(!gc_info_parse(b.data(), b.size(), g), "wrong table id is refused");
    b = make_bin(1, 1, n44v1, 19);
    expect(!gc_info_parse(b.data(), 0x100 + 40, g), "truncated buffer is refused");
    const uint32_t bogus[19] = { 0, 4, 0, 4 };
    b = make_bin(1, 1, bogus, 19);
    expect(!gc_info_parse(b.data(), b.size(), g), "0 SEs is refused");
    expect(!gc_info_parse(nullptr, 0, g), "null buffer is refused");

    b = make_bin(1, 1, n44v1, 19);
    put16(b, 10, 0x100); seal_binary(b);
    expect(!gc_info_parse(b.data(), b.size(), g), "GC table beyond declared binary size is refused");
    b = make_bin(1, 1, n44v1, 19);
    put16(b, 24, 12); seal_binary(b);
    expect(!gc_info_parse(b.data(), b.size(), g), "GC payload larger than table directory extent is refused");
    b = make_bin(1, 1, n44v1, 19);
    put16(b, 24, 1000); seal_binary(b);
    expect(!gc_info_parse(b.data(), b.size(), g), "GC directory extent outside binary is refused");
    b = make_bin(3, 1, n44v1, 19);
    expect(!gc_info_parse(b.data(), b.size(), g), "unknown binary layout is refused");
    b = make_bin(2, 1, n44v1, 19);
    put16(b, 12, UINT16_MAX); seal_binary(b);
    expect(!gc_info_parse(b.data(), b.size(), g), "v2 table count exceeding binary is refused");
    b = make_bin(1, 1, n44v1, 19);
    b[100] ^= 1;
    expect(!gc_info_parse(b.data(), b.size(), g), "binary checksum is checked before geometry is trusted");
    uint32_t wrapped[19];
    std::memcpy(wrapped, n44v1, sizeof(wrapped));
    wrapped[1] = 0x80000004u;
    b = make_bin(1, 1, wrapped, 19);
    expect(!gc_info_parse(b.data(), b.size(), g), "WGP-to-CU multiplication cannot wrap to eight CUs");
    wrapped[1] = UINT32_MAX; wrapped[2] = 5;
    b = make_bin(1, 1, wrapped, 19);
    expect(!gc_info_parse(b.data(), b.size(), g), "WGP addition cannot wrap to four WGPs");

    std::printf("%d/%d checks passed\n", gRun - gFail, gRun);
    return gFail ? 1 : 0;
}
