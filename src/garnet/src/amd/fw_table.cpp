// Firmware tables for the explicitly selected ASIC (see ../fw/fw_table.h).
// Generated $(BUILD)/firmware/fw_<blob>.c files provide each payload and size;
// absent blobs are zero-length stubs. Unknown or unselected chips have no table.
// The tables are filled lazily because external size values are not constant
// expressions and a kext must not depend on C++ global-constructor ordering.
#include "../fw/fw_table.h"
#include "amdgpu_asic.h"

namespace amdgpu {

// ---- symbols from generated $(BUILD)/firmware/fw_*.c files ----
//
// Names/format match tools/embed-firmware.py's output and the existing
// historical hand-written firmware source: `const uint8_t <sym>[]` (64-byte
// aligned) + `const size_t <sym>_size`.
#define FW_DECL(sym) extern const uint8_t fw_##sym[]; extern const size_t fw_##sym##_size;
extern "C" {
// Navi 48 (GC 12.0.1, MP0 14.0.3)
FW_DECL(psp_14_0_3_sos) FW_DECL(psp_14_0_3_ta)
FW_DECL(gc_12_0_1_imu) FW_DECL(gc_12_0_1_pfp) FW_DECL(gc_12_0_1_me) FW_DECL(gc_12_0_1_mec)
FW_DECL(gc_12_0_1_uni_mes) FW_DECL(gc_12_0_1_rlc)
FW_DECL(smu_14_0_3) FW_DECL(sdma_7_0_1)
// Navi 44 (GC 12.0.0, MP0 14.0.2)
FW_DECL(psp_14_0_2_sos) FW_DECL(psp_14_0_2_ta)
FW_DECL(gc_12_0_0_imu) FW_DECL(gc_12_0_0_pfp) FW_DECL(gc_12_0_0_me) FW_DECL(gc_12_0_0_mec)
FW_DECL(gc_12_0_0_uni_mes) FW_DECL(gc_12_0_0_rlc)
FW_DECL(smu_14_0_2) FW_DECL(sdma_7_0_0)
// Shared DCN 4.01 DAL DMUB
FW_DECL(dcn_4_0_1_dmcub)
} // extern "C"
#undef FW_DECL

namespace {

constexpr uint32_t kCount = static_cast<uint32_t>(FwId::Count);
constexpr uint32_t kChips = 2;   // 0 = Navi 48, 1 = Navi 44

// Plain zero-initialized statics — no constructor, lands in .bss.
FwBlob g_table[kChips][kCount];
bool   g_built = false;

// Explicit chip slot: Navi 48 -> 0, Navi 44 -> 1. Any other id (including
// Unknown and nullptr) has no slot and fails instead of aliasing a chip.
bool chip_index_for(const AsicInfo *a, uint32_t &out) {
    if (!a) return false;
    if (a->id == AsicId::Navi48) { out = 0u; return true; }
    if (a->id == AsicId::Navi44) { out = 1u; return true; }
    return false;
}

void set(uint32_t chip, FwId id, const uint8_t *data, size_t size, const char *name) {
    g_table[chip][static_cast<uint32_t>(id)] = FwBlob{ data, size, name, id };
}

// Fills g_table[][] with ordinary runtime assignments (reads each extern
// fw_<blob>_size's actual value — valid the moment this kext's segments
// are mapped, well before any bring-up code could call fw_get()). Safe
// to call repeatedly; only the first call does anything. Bring-up is
// single-threaded through this point, so no guard/lock is used — see
// the file header.
//
// GC_MES, GC_MES1 and GC_TOC have no embedded blob on either chip's
// non-kicker bring-up ladder (uni_mes supersedes the legacy split MES
// pair; TOC is only loaded for psp_14_0_5), so those rows keep
// data=nullptr, size=0 — see fw_table.h.
void build_table() {
    if (g_built) return;
    const AsicInfo *chips[kChips] = { asic_by_id(AsicId::Navi48), asic_by_id(AsicId::Navi44) };
    const uint8_t *data[kChips][kCount] = {
        { fw_psp_14_0_3_sos, fw_psp_14_0_3_ta, fw_gc_12_0_1_imu, fw_gc_12_0_1_pfp, fw_gc_12_0_1_me,
          fw_gc_12_0_1_mec, nullptr, nullptr, fw_gc_12_0_1_uni_mes, fw_gc_12_0_1_rlc, nullptr,
          fw_smu_14_0_3, fw_sdma_7_0_1, fw_dcn_4_0_1_dmcub },
        { fw_psp_14_0_2_sos, fw_psp_14_0_2_ta, fw_gc_12_0_0_imu, fw_gc_12_0_0_pfp, fw_gc_12_0_0_me,
          fw_gc_12_0_0_mec, nullptr, nullptr, fw_gc_12_0_0_uni_mes, fw_gc_12_0_0_rlc, nullptr,
          fw_smu_14_0_2, fw_sdma_7_0_0, fw_dcn_4_0_1_dmcub },
    };
    const size_t size[kChips][kCount] = {
        { fw_psp_14_0_3_sos_size, fw_psp_14_0_3_ta_size, fw_gc_12_0_1_imu_size, fw_gc_12_0_1_pfp_size,
          fw_gc_12_0_1_me_size, fw_gc_12_0_1_mec_size, 0, 0, fw_gc_12_0_1_uni_mes_size,
          fw_gc_12_0_1_rlc_size, 0, fw_smu_14_0_3_size, fw_sdma_7_0_1_size, fw_dcn_4_0_1_dmcub_size },
        { fw_psp_14_0_2_sos_size, fw_psp_14_0_2_ta_size, fw_gc_12_0_0_imu_size, fw_gc_12_0_0_pfp_size,
          fw_gc_12_0_0_me_size, fw_gc_12_0_0_mec_size, 0, 0, fw_gc_12_0_0_uni_mes_size,
          fw_gc_12_0_0_rlc_size, 0, fw_smu_14_0_2_size, fw_sdma_7_0_0_size, fw_dcn_4_0_1_dmcub_size },
    };
    for (uint32_t c = 0; c < kChips; c++) {
        const AsicFwNames &n = chips[c]->fw;
        const char *names[kCount] = { n.psp_sos, n.psp_ta, n.gc_imu, n.gc_pfp, n.gc_me, n.gc_mec, n.gc_mes,
                                      n.gc_mes1, n.gc_uni_mes, n.gc_rlc, n.gc_toc, n.smu, n.sdma, n.dcn_dmcub };
        for (uint32_t i = 0; i < kCount; i++)
            set(c, static_cast<FwId>(i), size[c][i] ? data[c][i] : nullptr, size[c][i], names[i]);
    }
    g_built = true;
}

// Manual equality check — deliberately not strcmp/strncmp. PORTING.md /
// the task brief only guarantee memcpy/memset/strlen/strncmp out of
// <string.h> in this kernel; strcmp isn't in that guaranteed set, and a
// bounded strncmp would need an arbitrary length cap for what is really
// an unbounded exact-match. Blob names are short, static, NUL-terminated
// C strings on both sides, so a plain loop is simplest and dependency-free.
bool str_eq(const char *a, const char *b) {
    while (*a != '\0' && *b != '\0') {
        if (*a != *b) return false;
        ++a;
        ++b;
    }
    return *a == *b; // both must be at NUL simultaneously
}

bool is_optional(uint32_t i) {
    const FwId id = static_cast<FwId>(i);
    return id == FwId::GC_MES || id == FwId::GC_MES1 || id == FwId::GC_TOC || id == FwId::DCN_DMUB;
}

} // anonymous namespace

const FwBlob *fw_get(FwId id) {
    build_table();
    uint32_t idx = static_cast<uint32_t>(id);
    if (idx >= kCount) return nullptr;
    uint32_t c = 0;
    if (!chip_index_for(asic_current(), c)) return nullptr;
    return &g_table[c][idx];
}

const FwBlob *fw_find_by_name(const char *name) {
    if (name == nullptr) return nullptr;
    build_table();
    uint32_t c = 0;
    if (!chip_index_for(asic_current(), c)) return nullptr;
    for (uint32_t i = 0; i < kCount; ++i) {
        if (str_eq(g_table[c][i].name, name)) return &g_table[c][i];
    }
    return nullptr;
}

bool fw_chip_complete(const AsicInfo *asic) {
    if (asic == nullptr) return false;
    build_table();
    uint32_t c = 0;
    if (!chip_index_for(asic, c)) return false;
    for (uint32_t i = 0; i < kCount; ++i)
        if (!is_optional(i) && g_table[c][i].size == 0) return false;
    return true;
}

} // namespace amdgpu
