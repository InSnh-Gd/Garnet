#include "fw_loader.h"
#include "amdgpu_ucode_extract.h"
#include "amdgpu_log.h"
#include "../fw/fw_table.h"

namespace amdgpu {

kern_return_t fw_loader_prepare(DeviceContext &dev, PSPContext &psp, FwLoaderState &st) {
    st = {};
    const auto reject = [&st](kern_return_t error) {
        st = {};
        return error;
    };
    struct Plan { FwId id; uint64_t host; const char *what; uint32_t required[4]; };
    const Plan plans[] = {
        { FwId::SMU,        kHostFwIPBase + PSPGfxFwType::SMU, "smu",
          { PSPGfxFwType::SMU } },
        { FwId::GC_IMU,     kHostFwFile_IMU, "imu",
          { PSPGfxFwType::IMU_I, PSPGfxFwType::IMU_D } },
        // RLC subcomponents with zero sizes are legitimately absent; RLC_G is mandatory.
        { FwId::GC_RLC,     kHostFwFile_RLC, "rlc", { PSPGfxFwType::RLC_G } },
        { FwId::GC_PFP,     kHostFwFile_CP_PFP, "pfp",
          { PSPGfxFwType::RS64_PFP, PSPGfxFwType::RS64_PFP_P0 } },
        { FwId::GC_ME,      kHostFwFile_CP_ME, "me",
          { PSPGfxFwType::RS64_ME, PSPGfxFwType::RS64_ME_P0 } },
        { FwId::GC_MEC,     kHostFwFile_CP_MEC, "mec",
          { PSPGfxFwType::RS64_MEC, PSPGfxFwType::RS64_MEC_P0, PSPGfxFwType::RS64_MEC_P1 } },
        { FwId::GC_UNI_MES, kHostFwFile_MES_UNI, "uni_mes",
          { PSPGfxFwType::CP_MES, PSPGfxFwType::CP_MES_DATA,
            PSPGfxFwType::CP_MES_KIQ, PSPGfxFwType::MES_KIQ_STACK } },
        { FwId::SDMA,       kHostFwFile_SDMA, "sdma", { PSPGfxFwType::SDMA_UCODE_TH0 } },
    };
    for (const Plan &pl : plans) {
        const FwBlob *b = fw_get(pl.id);
        if (!b || !b->data || b->size == 0) {
            UCODE_LOG("loader: required blob %s missing", pl.what);
            return reject(kIOReturnNotFound);
        }
        UcodePayload out[kMaxUcodePayloadsPerFile];
        uint32_t n = amdgpu_ucode_extract(pl.host, b->data, b->size, out);
        if (n == 0 || n > kMaxUcodePayloadsPerFile || n > FwLoaderState::kMaxEntries - st.count) {
            UCODE_LOG("loader: %s (%s, %llu B): invalid payload count %u", pl.what, b->name,
                      (unsigned long long)b->size, n);
            return reject(kIOReturnBadArgument);
        }
        for (uint32_t i = 0; i < n; i++) {
            if (out[i].size_bytes == 0 || (uint64_t)out[i].offset_bytes + out[i].size_bytes > b->size) {
                UCODE_LOG("loader: %s payload type %u empty or out of bounds", pl.what, out[i].fw_type);
                return reject(kIOReturnBadArgument);
            }
            FwLoaderState::Entry &e = st.entries[st.count++];
            e.fw_type = out[i].fw_type; e.src = b->data + out[i].offset_bytes; e.size = out[i].size_bytes;
            e.mc = 0; e.staged = false; e.blob = b->name;
            UCODE_LOG("loader: %-8s type %3u  %7u bytes @+%u", pl.what, e.fw_type, e.size, out[i].offset_bytes);
        }
        for (uint32_t type : pl.required) {
            if (type && !fw_loader_find(st, type)) {
                UCODE_LOG("loader: %s missing required payload type %u", pl.what, type);
                return reject(kIOReturnBadArgument);
            }
        }
    }
    // Optional shared DCN4 DAL DMUB payload (used only when garnet-dmcub-fw=1).
    if (const FwBlob *dmub = fw_get(FwId::DCN_DMUB); dmub && dmub->data && dmub->size > 0) {
        UcodePayload out[kMaxUcodePayloadsPerFile];
        uint32_t n = amdgpu_ucode_extract(kHostFwFile_DCN_DMUB, dmub->data, dmub->size, out);
        if (n > 0 && st.count < FwLoaderState::kMaxEntries &&
            out[0].size_bytes > 0 &&
            (uint64_t)out[0].offset_bytes + out[0].size_bytes <= dmub->size) {
            FwLoaderState::Entry &e = st.entries[st.count++];
            e.fw_type = out[0].fw_type; e.src = dmub->data + out[0].offset_bytes; e.size = out[0].size_bytes;
            e.mc = 0; e.staged = false; e.blob = dmub->name;
            st.dmubPresent = true;
            UCODE_LOG("loader: dcn_dmub type %3u  %7u bytes @+%u", e.fw_type, e.size, out[0].offset_bytes);
        }
    }
    st.dev = &dev; st.psp = &psp;
    st.smuPresent = st.imuPresent = st.rlcPresent = st.cpPresent = st.mesPresent = st.sdmaPresent = true;
    psp.fwBufBumpOffset = 0;    // a successful re-run re-stages everything from the start of fw_buf
    UCODE_LOG("loader: %u payloads ready (smu %d imu %d rlc %d cp %d mes %d sdma %d dmub %d)", st.count,
              st.smuPresent, st.imuPresent, st.rlcPresent, st.cpPresent, st.mesPresent, st.sdmaPresent, st.dmubPresent);
    return kIOReturnSuccess;
}

const FwLoaderState::Entry *fw_loader_find(const FwLoaderState &st, uint32_t fw_type) {
    for (uint32_t i = 0; i < st.count; i++) if (st.entries[i].fw_type == fw_type) return &st.entries[i];
    return nullptr;
}

static kern_return_t get_payload(void *ctx, uint32_t fw_type, uint64_t &out_bus_addr, uint32_t &out_size) {
    FwLoaderState &st = *static_cast<FwLoaderState *>(ctx);
    FwLoaderState::Entry *e = const_cast<FwLoaderState::Entry *>(fw_loader_find(st, fw_type));
    if (!e) return kIOReturnUnsupported;                       // "not present" -> skipped by the caller
    if (!e->staged) {
        kern_return_t r = psp_fw_buf_stage(*st.dev, *st.psp, e->src, e->size, &e->mc);
        if (r != kIOReturnSuccess) { UCODE_LOG("loader: staging type %u (%u B) failed kr=%#x", fw_type, e->size, r); return r; }
        e->staged = true;
    }
    out_bus_addr = e->mc; out_size = e->size;
    return kIOReturnSuccess;
}

FirmwareLoader fw_loader_make(FwLoaderState &st) {
    FirmwareLoader l; l.fn = &get_payload; l.ctx = &st; return l;
}

} // namespace amdgpu
