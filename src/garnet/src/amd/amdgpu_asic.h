//
//  amdgpu_asic.h — per-chip facts the bring-up needs, selected at run time.
//
//  Supports RDNA 4 desktop GPUs (Navi 48: RX 9070 / 9070 XT 1002:7550,
//  AI PRO R9700 1002:7551, GC 12.0.1; and Navi 44: RX 9060 XT 1002:7590,
//  GC 12.0.0). Linux amdgpu runs the same IP blocks and the same sequence on both; what
//  differs is collected in this module:
//
//    * the linux-firmware blob names (gc_12_0_0 / psp_14_0_2 / smu_14_0_2 / sdma_7_0_0 on
//      Navi 44 vs gc_12_0_1 / psp_14_0_3 / smu_14_0_3 / sdma_7_0_1 on Navi 48),
//    * the PSP SOS version from which FB_FW_RESERV_ADDR is sent (amdgpu_psp.c
//      psp_update_fw_reservation: 0x3b0e0d on MP0 14.0.2, 0x3a0e14 on MP0 14.0.3),
//    * a FALLBACK die geometry, only used when the on-die discovery gc_info table cannot be
//      read (Linux takes the geometry from gc_info, never from code).
//
//  Everything else (register bases, IP versions, harvest masks) still comes from the card.
//  Pure C++: no IOKit, no allocation, so tests can compile it on any host.
//
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace amdgpu {

enum class AsicId : uint8_t { Unknown = 0, Navi48, Navi44 };

// The die geometry gfx_gpu_early_init needs (Linux adev->gfx.config.max_*).
struct GcGeometry {
    bool     valid { false };        // true when filled (from gc_info or from the fallback table)
    bool     fromDiscovery { false };// true when filled from the on-die gc_info table
    uint16_t gcInfoMajor { 0 }, gcInfoMinor { 0 };
    uint32_t max_shader_engines { 0 };
    uint32_t max_sh_per_se { 0 };       // shader arrays per SE
    uint32_t max_backends_per_se { 0 }; // RBs per SE
    uint32_t max_cu_per_sh { 0 };       // CUs per SA (= 2 x WGPs)
    uint32_t num_gl2c { 0 };            // informational (TCC channels); 0 if unknown
};

// Firmware blob base names, in FwId order minus Count (see ../fw/fw_table.h).
struct AsicFwNames {
    const char *psp_sos, *psp_ta, *gc_imu, *gc_pfp, *gc_me, *gc_mec, *gc_mes, *gc_mes1,
               *gc_uni_mes, *gc_rlc, *gc_toc, *smu, *sdma, *dcn_dmcub;
};

struct AsicInfo {
    AsicId      id;
    const char *family;        // "Navi 48"
    // GC / MP0 versions this entry is selected by (discovery) and expects.
    uint8_t     gcMajor, gcMinor, gcRev;
    uint8_t     mp0Major, mp0Minor, mp0Rev;
    // PCI device ids (vendor 0x1002) and their marketing names; 0-terminated.
    uint16_t    pciIds[4];
    const char *pciNames[4];
    // FB_FW_RESERV_ADDR is sent only when the running SOS is at least this version.
    uint32_t    pspFwReservMinSos;
    // Used only when the discovery gc_info table is unavailable.
    GcGeometry  fallbackGeometry;
    AsicFwNames fw;
};

// Table lookups. nullptr when nothing matches.
const AsicInfo *asic_from_pci(uint16_t vendor, uint16_t device);
const AsicInfo *asic_from_gc(uint8_t major, uint8_t minor, uint8_t rev);
const AsicInfo *asic_by_id(AsicId id);
const char     *asic_pci_name(const AsicInfo *a, uint16_t device);   // "RX 9060 XT", or the family name

// The chip this boot runs on. Selected once in GarnetDriver::probe/start (single-threaded),
// read by fw_get() and the IP modules. Explicit: nullptr until selected (or cleared with
// asic_select(nullptr)); unknown PCI/GC ids never fall back to a default chip.
void            asic_select(const AsicInfo *a);
const AsicInfo *asic_current();   // nullptr when no chip selected
bool            asic_is_selected();
// Selected chip's expected MP0 version, for use only when on-die discovery
// has not reported MP0. False when no chip is selected.
bool            asic_expected_mp0(uint8_t *major, uint8_t *minor, uint8_t *rev);

// Parse gc_info v1.x / v2.x from a discovery binary (Linux discovery.h; amdgpu_discovery.c
// amdgpu_discovery_get_gfx_info). `bin` points at binary_header (the 0x28211407 signature),
// `len` is the number of readable bytes from there. Returns false and leaves `out` invalid
// when the table is absent, out of bounds, has the wrong id or a bad checksum.
bool gc_info_parse(const uint8_t *bin, size_t len, GcGeometry &out);

} // namespace amdgpu
