//
//  amdgpu_asic.cpp — see amdgpu_asic.h.
//
#include "amdgpu_asic.h"

namespace amdgpu {

namespace {

// Navi 48 (GC 12.0.1, MP0/MP1 14.0.3, SDMA 7.0.1) and Navi 44 (GC 12.0.0, MP0/MP1 14.0.2,
// SDMA 7.0.0, 2 SE x 2 SA x 8 CU, 8 RB, 16 TCC) chip facts.
const AsicInfo kAsics[] = {
    {
        AsicId::Navi48, "Navi 48",
        12, 0, 1,   14, 0, 3,
        { 0x7550, 0x7551, 0, 0 },
        { "RX 9070/XT", "AI PRO R9700", nullptr, nullptr },
        0x003a0e14u,
        // 4 SE x 2 SA/SE x 4 WGP/SA x 2 CU/WGP = 64 CU, 4 RB/SE = 16 RB
        GcGeometry{ true, false, 0, 0, 4, 2, 4, 8, 32 },
        { "psp_14_0_3_sos", "psp_14_0_3_ta", "gc_12_0_1_imu", "gc_12_0_1_pfp", "gc_12_0_1_me",
          "gc_12_0_1_mec", "gc_12_0_1_mes", "gc_12_0_1_mes1", "gc_12_0_1_uni_mes", "gc_12_0_1_rlc",
          "gc_12_0_1_toc", "smu_14_0_3", "sdma_7_0_1" },
    },
    {
        AsicId::Navi44, "Navi 44",
        12, 0, 0,   14, 0, 2,
        { 0x7590, 0, 0, 0 },
        { "RX 9060 XT", nullptr, nullptr, nullptr },
        0x003b0e0du,
        // 2 SE x 2 SA/SE x 4 WGP/SA x 2 CU/WGP = 32 CU, 4 RB/SE = 8 RB
        GcGeometry{ true, false, 0, 0, 2, 2, 4, 8, 16 },
        { "psp_14_0_2_sos", "psp_14_0_2_ta", "gc_12_0_0_imu", "gc_12_0_0_pfp", "gc_12_0_0_me",
          "gc_12_0_0_mec", "gc_12_0_0_mes", "gc_12_0_0_mes1", "gc_12_0_0_uni_mes", "gc_12_0_0_rlc",
          "gc_12_0_0_toc", "smu_14_0_2", "sdma_7_0_0" },
    },
};
constexpr size_t kNumAsics = sizeof(kAsics) / sizeof(kAsics[0]);

const AsicInfo *g_current = nullptr;

inline uint16_t rd16(const uint8_t *p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
inline uint32_t rd32(const uint8_t *p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

} // namespace

const AsicInfo *asic_from_pci(uint16_t vendor, uint16_t device) {
    if (vendor != 0x1002) return nullptr;
    for (size_t i = 0; i < kNumAsics; i++)
        for (int j = 0; j < 4 && kAsics[i].pciIds[j]; j++)
            if (kAsics[i].pciIds[j] == device) return &kAsics[i];
    return nullptr;
}

const AsicInfo *asic_from_gc(uint8_t major, uint8_t minor, uint8_t rev) {
    for (size_t i = 0; i < kNumAsics; i++)
        if (kAsics[i].gcMajor == major && kAsics[i].gcMinor == minor && kAsics[i].gcRev == rev)
            return &kAsics[i];
    return nullptr;
}

const AsicInfo *asic_by_id(AsicId id) {
    for (size_t i = 0; i < kNumAsics; i++)
        if (kAsics[i].id == id) return &kAsics[i];
    return nullptr;
}

const char *asic_pci_name(const AsicInfo *a, uint16_t device) {
    if (!a) return "unknown";
    for (int j = 0; j < 4 && a->pciIds[j]; j++)
        if (a->pciIds[j] == device && a->pciNames[j]) return a->pciNames[j];
    return a->family;
}

void asic_select(const AsicInfo *a) { g_current = a; }

const AsicInfo *asic_current() { return g_current; }

bool asic_is_selected() { return g_current != nullptr; }

bool asic_expected_mp0(uint8_t *major, uint8_t *minor, uint8_t *rev) {
    if (!g_current) return false;
    if (major) *major = g_current->mp0Major;
    if (minor) *minor = g_current->mp0Minor;
    if (rev) *rev = g_current->mp0Rev;
    return true;
}

// binary_header (v1): sig u32, vmaj u16, vmin u16, checksum u16, size u16, table_info[7] @ +12.
// binary_header_v2: ..., num_tables u16, pad u16, table_info[] @ +16.
// table_info: offset u16 (from binary start), checksum u16 (byte sum), size u16, pad u16.
// GC is table index 1. gpu_info_header: table_id u32 (0x4347), vmaj u16, vmin u16, size u32.
bool gc_info_parse(const uint8_t *bin, size_t len, GcGeometry &out) {
    out = GcGeometry();
    if (!bin || len < 28 || rd32(bin) != 0x28211407u) return false;
    const uint16_t vmajor = rd16(bin + 4);
    const size_t binarySize = rd16(bin + 10);
    if ((vmajor != 1 && vmajor != 2) || binarySize < 28 || binarySize > len) return false;
    size_t list = 12;
    size_t directoryEnd = list + 16;
    if (vmajor == 2) {
        const size_t tables = rd16(bin + 12);
        if (tables < 2 || tables > (binarySize - 16) / 8) return false;
        list = 16;
        directoryEnd = list + tables * 8;
    }
    const size_t ti = list + 8;                  // table_list[GC]
    if (directoryEnd > binarySize) return false;
    uint16_t sum = 0;
    for (size_t i = 10; i < binarySize; ++i) sum = static_cast<uint16_t>(sum + bin[i]);
    if (sum != rd16(bin + 8)) return false;
    const uint16_t off = rd16(bin + ti), csum = rd16(bin + ti + 2), tableSize = rd16(bin + ti + 4);
    if (off < directoryEnd || off > binarySize || tableSize < 12 || tableSize > binarySize - off) return false;
    const uint8_t *g = bin + off;
    if (rd32(g) != 0x4347u) return false;        // GC_TABLE_ID
    const uint16_t gmaj = rd16(g + 4), gmin = rd16(g + 6);
    const uint32_t gsize = rd32(g + 8);
    if (gsize < 12 + 4 * 17 || gsize > tableSize) return false;
    sum = 0;
    for (uint32_t i = 0; i < tableSize; i++) sum = static_cast<uint16_t>(sum + g[i]);
    if (sum != csum) return false;

    const uint8_t *d = g + 12;
    GcGeometry r;
    r.gcInfoMajor = gmaj; r.gcInfoMinor = gmin;
    if (gmaj == 1) {
        // gc_info_v1_x: se, wgp0/sa, wgp1/sa, rb/se, gl2c, ... sa_per_se at +64 (field 16).
        if (gsize < 12 + 4 * 17) return false;
        r.max_shader_engines  = rd32(d + 0);
        const uint64_t wgps = static_cast<uint64_t>(rd32(d + 4)) + rd32(d + 8);
        if (wgps > 16) return false;
        r.max_cu_per_sh       = static_cast<uint32_t>(2 * wgps);
        r.max_backends_per_se = rd32(d + 12);
        r.num_gl2c            = rd32(d + 16);
        r.max_sh_per_se       = rd32(d + 64);
    } else if (gmaj == 2) {
        // gc_info_v2_x: se, cu_per_sh, sh_per_se, rb_per_se, tccs, ...
        r.max_shader_engines  = rd32(d + 0);
        r.max_cu_per_sh       = rd32(d + 4);
        r.max_sh_per_se       = rd32(d + 8);
        r.max_backends_per_se = rd32(d + 12);
        r.num_gl2c            = rd32(d + 16);
    } else {
        return false;
    }
    // Sanity: GFX12 parts have 1..8 SEs, 1..4 SAs/SE, 1..8 RBs/SE, 2..32 CUs/SA.
    if (r.max_shader_engines < 1 || r.max_shader_engines > 8 || r.max_sh_per_se < 1 ||
        r.max_sh_per_se > 4 || r.max_backends_per_se < 1 || r.max_backends_per_se > 8 ||
        r.max_cu_per_sh < 2 || r.max_cu_per_sh > 32)
        return false;
    r.valid = true;
    r.fromDiscovery = true;
    out = r;
    return true;
}

} // namespace amdgpu
