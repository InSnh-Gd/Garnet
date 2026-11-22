#include "fw_loader.h"
#include "amdgpu_ucode_extract.h"
#include "amdgpu_ucode_psp.h"
#include "../src/fw/fw_table.h"

#include <cstdio>
#include <cstring>
#include <initializer_list>

using namespace amdgpu;

namespace {
constexpr FwId requiredFiles[] = {
    FwId::SMU, FwId::GC_IMU, FwId::GC_RLC, FwId::GC_PFP,
    FwId::GC_ME, FwId::GC_MEC, FwId::GC_UNI_MES, FwId::SDMA,
};
constexpr uint32_t requiredTypes[] = {
    PSPGfxFwType::SMU, PSPGfxFwType::IMU_I, PSPGfxFwType::IMU_D,
    PSPGfxFwType::RLC_G, PSPGfxFwType::RS64_PFP, PSPGfxFwType::RS64_PFP_P0,
    PSPGfxFwType::RS64_ME, PSPGfxFwType::RS64_ME_P0, PSPGfxFwType::RS64_MEC,
    PSPGfxFwType::RS64_MEC_P0, PSPGfxFwType::RS64_MEC_P1,
    PSPGfxFwType::CP_MES, PSPGfxFwType::CP_MES_DATA,
    PSPGfxFwType::CP_MES_KIQ, PSPGfxFwType::MES_KIQ_STACK,
    PSPGfxFwType::SDMA_UCODE_TH0,
};
constexpr size_t fileCount = static_cast<size_t>(FwId::Count);
constexpr uint64_t stageBase = 0x100000;
unsigned failures = 0;
const char *scenario = nullptr;

void check(bool ok, const char *expr, int line) {
    if (!ok) {
        std::fprintf(stderr, "FAIL %s:%d: %s\n", scenario, line, expr);
        ++failures;
    }
}
#define CHECK(expr) check((expr), #expr, __LINE__)

struct Fixture {
    uint8_t bytes[fileCount][512] {};
    FwBlob blobs[fileCount] {};
    bool absent[fileCount] {};
    DeviceContext dev {};
    PSPContext psp {};
    FwLoaderState state {};
    unsigned stageCalls = 0;
    kern_return_t stageResult = kIOReturnSuccess;
    const void *stagedBytes = nullptr;
    uint32_t stagedSize = 0;

    static size_t index(FwId id) { return static_cast<size_t>(id); }

    template<class Header> void store(FwId id, const Header &header) {
        const size_t i = index(id);
        std::memcpy(bytes[i], &header, sizeof(header));
        for (size_t j = sizeof(header); j < sizeof(header) + 8; ++j)
            bytes[i][j] = static_cast<uint8_t>(j + i);
        blobs[i] = {bytes[i], sizeof(header) + 8, "synthetic", id};
    }

    template<class Header, class Edit> void edit(FwId id, Edit change) {
        Header header {};
        std::memcpy(&header, bytes[index(id)], sizeof(header));
        change(header);
        std::memcpy(bytes[index(id)], &header, sizeof(header));
    }

    template<class Header> static common_firmware_header common(uint16_t major,
                                                                 uint16_t minor = 0) {
        common_firmware_header h {};
        h.size_bytes = sizeof(Header) + 8;
        h.header_size_bytes = sizeof(Header);
        h.header_version_major = major;
        h.header_version_minor = minor;
        h.ucode_size_bytes = 8;
        h.ucode_array_offset_bytes = sizeof(Header);
        return h;
    }

    Fixture() {
        store(FwId::SMU, common<common_firmware_header>(1));
        imu_firmware_header_v1_0 imu {};
        imu.header = common<decltype(imu)>(1);
        imu.imu_iram_ucode_size_bytes = imu.imu_dram_ucode_size_bytes = 4;
        store(FwId::GC_IMU, imu);

        rlc_firmware_header_v2_4 rlc {};
        rlc.v2_3.v2_2.v2_1.v2_0.header = common<decltype(rlc)>(2, 4);
        store(FwId::GC_RLC, rlc);

        gfx_firmware_header_v2_0 cp {};
        cp.header = common<decltype(cp)>(2);
        cp.ucode_size_bytes = cp.data_size_bytes = 4;
        cp.ucode_offset_bytes = cp.header.ucode_array_offset_bytes;
        cp.data_offset_bytes = cp.ucode_offset_bytes + 4;
        for (FwId id : {FwId::GC_PFP, FwId::GC_ME, FwId::GC_MEC}) store(id, cp);

        mes_firmware_header_v1_0 mes {};
        mes.header = common<decltype(mes)>(1);
        mes.mes_ucode_size_bytes = mes.mes_ucode_data_size_bytes = 4;
        mes.mes_ucode_offset_bytes = sizeof(mes);
        mes.mes_ucode_data_offset_bytes = sizeof(mes) + 4;
        store(FwId::GC_UNI_MES, mes);

        sdma_firmware_header_v3_0 sdma {};
        sdma.header = common<decltype(sdma)>(3);
        sdma.ucode_size_bytes = 8;
        store(FwId::SDMA, sdma);
        psp.fwBufBumpOffset = 0x5000;
    }

    bool noFlags() const {
        return !state.smuPresent && !state.imuPresent && !state.rlcPresent &&
               !state.cpPresent && !state.mesPresent && !state.sdmaPresent;
    }

    void rejected(kern_return_t expected) {
        const unsigned calls = stageCalls;
        const uint64_t bump = psp.fwBufBumpOffset;
        CHECK(fw_loader_prepare(dev, psp, state) == expected);
        CHECK(state.count == 0);
        CHECK(noFlags());
        CHECK(psp.fwBufBumpOffset == bump);
        auto loader = fw_loader_make(state);
        for (uint32_t type : requiredTypes) {
            CHECK(fw_loader_find(state, type) == nullptr);
            uint64_t bus = 0;
            uint32_t size = 0;
            CHECK(loader.get_payload(type, bus, size) == kIOReturnUnsupported);
        }
        CHECK(stageCalls == calls);
    }
};
Fixture *active = nullptr;

void valid_and_lazy_stage() {
    Fixture f;
    active = &f;
    CHECK(fw_loader_prepare(f.dev, f.psp, f.state) == kIOReturnSuccess);
    CHECK(f.state.count == sizeof(requiredTypes) / sizeof(requiredTypes[0]));
    CHECK(f.state.smuPresent && f.state.imuPresent && f.state.rlcPresent &&
          f.state.cpPresent && f.state.mesPresent && f.state.sdmaPresent);
    CHECK(f.state.dev == &f.dev && f.state.psp == &f.psp);
    CHECK(f.stageCalls == 0 && f.psp.fwBufBumpOffset == 0);
    auto loader = fw_loader_make(f.state);
    for (uint32_t type : requiredTypes) {
        const auto *entry = fw_loader_find(f.state, type);
        CHECK(entry != nullptr);
        if (!entry) continue;
        CHECK(!entry->staged && entry->mc == 0);
        uint64_t bus = 0;
        uint32_t size = 0;
        const unsigned before = f.stageCalls;
        CHECK(loader.get_payload(type, bus, size) == kIOReturnSuccess);
        CHECK(f.stageCalls == before + 1);
        CHECK(f.stagedBytes == entry->src && f.stagedSize == entry->size);
        CHECK(bus == stageBase + before * 0x1000 && size == entry->size);
        CHECK(entry->staged && entry->mc == bus);
        CHECK(loader.get_payload(type, bus, size) == kIOReturnSuccess);
        CHECK(f.stageCalls == before + 1);
    }
    CHECK(fw_loader_find(f.state, PSPGfxFwType::RS64_PFP_P1) == nullptr);
    CHECK(fw_loader_find(f.state, PSPGfxFwType::RS64_ME_P1) == nullptr);
    CHECK(fw_loader_find(f.state, PSPGfxFwType::RS64_MEC_P2) == nullptr);
}

void missing_files() {
    for (FwId id : requiredFiles) {
        for (unsigned kind = 0; kind < 3; ++kind) {
            Fixture f;
            active = &f;
            if (kind == 0) f.absent[Fixture::index(id)] = true;
            if (kind == 1) f.blobs[Fixture::index(id)].data = nullptr;
            if (kind == 2) f.blobs[Fixture::index(id)].size = 0;
            f.rejected(kIOReturnNotFound);
        }
    }
    for (FwId kept : requiredFiles) {
        Fixture f;
        active = &f;
        for (FwId id : requiredFiles) f.absent[Fixture::index(id)] = id != kept;
        f.rejected(kIOReturnNotFound);
    }
}

void malformed_files() {
    for (FwId id : requiredFiles) {
        for (unsigned kind = 0; kind < 2; ++kind) {
            Fixture f;
            active = &f;
            if (kind == 0) f.blobs[Fixture::index(id)].size = sizeof(common_firmware_header) - 1;
            else std::memset(f.bytes[Fixture::index(id)], 0, sizeof(common_firmware_header));
            f.rejected(kIOReturnBadArgument);
        }
    }
    for (FwId id : {FwId::SMU, FwId::SDMA}) {
        Fixture f;
        active = &f;
        f.edit<common_firmware_header>(id, [](auto &h) {
            h.ucode_array_offset_bytes = 0xffffffffu;
        });
        f.rejected(kIOReturnBadArgument);
    }
}

void partial_imu() {
    for (unsigned part = 0; part < 2; ++part) {
        for (bool outOfBounds : {false, true}) {
            Fixture f;
            active = &f;
            f.edit<imu_firmware_header_v1_0>(FwId::GC_IMU, [=](auto &h) {
                const uint32_t bad = outOfBounds ? 0xffffffffu : 0;
                if (part == 0) h.imu_iram_ucode_size_bytes = bad;
                else h.imu_dram_ucode_size_bytes = bad;
            });
            f.rejected(kIOReturnBadArgument);
        }
    }
}

void partial_rs64() {
    for (FwId id : {FwId::GC_PFP, FwId::GC_ME, FwId::GC_MEC}) {
        for (unsigned part = 0; part < 2; ++part) {
            for (bool outOfBounds : {false, true}) {
                Fixture f;
                active = &f;
                f.edit<gfx_firmware_header_v2_0>(id, [=](auto &h) {
                    const uint32_t bad = outOfBounds ? 0xffffffffu : 0;
                    if (part == 0) h.ucode_size_bytes = bad;
                    else h.data_size_bytes = bad;
                });
                f.rejected(kIOReturnBadArgument);
            }
        }
    }
}

void partial_mes() {
    for (unsigned part = 0; part < 2; ++part) {
        for (bool outOfBounds : {false, true}) {
            Fixture f;
            active = &f;
            f.edit<mes_firmware_header_v1_0>(FwId::GC_UNI_MES, [=](auto &h) {
                const uint32_t bad = outOfBounds ? 0xffffffffu : 0;
                if (part == 0) h.mes_ucode_size_bytes = bad;
                else h.mes_ucode_data_size_bytes = bad;
            });
            f.rejected(kIOReturnBadArgument);
        }
    }
}

void rlc_requires_g() {
    for (bool outOfBounds : {false, true}) {
        Fixture f;
        active = &f;
        f.edit<rlc_firmware_header_v2_4>(FwId::GC_RLC, [=](auto &h) {
            h.v2_3.v2_2.v2_1.v2_0.header.ucode_size_bytes = outOfBounds ? 0xffffffffu : 0;
            h.global_tap_delays_ucode_size_bytes = 4;
            h.global_tap_delays_ucode_offset_bytes = sizeof(h);
        });
        f.rejected(kIOReturnBadArgument);
    }
}

void optional_rlc() {
    for (uint16_t minor = 0; minor <= 4; ++minor) {
        Fixture f;
        active = &f;
        const uint32_t headerSizes[] = {
            sizeof(rlc_firmware_header_v2_0), sizeof(rlc_firmware_header_v2_1),
            sizeof(rlc_firmware_header_v2_2), sizeof(rlc_firmware_header_v2_3),
            sizeof(rlc_firmware_header_v2_4),
        };
        const uint32_t headerSize = headerSizes[minor];
        f.edit<common_firmware_header>(FwId::GC_RLC, [=](auto &h) {
            h.header_version_minor = minor;
            h.size_bytes = headerSize + 8;
            h.header_size_bytes = headerSize;
            h.ucode_array_offset_bytes = headerSize;
        });
        f.blobs[Fixture::index(FwId::GC_RLC)].size = headerSize + 8;
        CHECK(fw_loader_prepare(f.dev, f.psp, f.state) == kIOReturnSuccess);
        CHECK(f.state.rlcPresent);
        CHECK(fw_loader_find(f.state, PSPGfxFwType::RLC_G) != nullptr);
        CHECK(fw_loader_find(f.state, PSPGfxFwType::RLC_IRAM) == nullptr);
        CHECK(fw_loader_find(f.state, PSPGfxFwType::GLOBAL_TAP_DELAYS) == nullptr);
        CHECK(f.stageCalls == 0);
    }
    Fixture f;
    active = &f;
    f.edit<rlc_firmware_header_v2_4>(FwId::GC_RLC, [](auto &h) {
        h.global_tap_delays_ucode_size_bytes = 4;
        h.global_tap_delays_ucode_offset_bytes = sizeof(h);
    });
    CHECK(fw_loader_prepare(f.dev, f.psp, f.state) == kIOReturnSuccess);
    CHECK(fw_loader_find(f.state, PSPGfxFwType::GLOBAL_TAP_DELAYS) != nullptr);
    CHECK(fw_loader_find(f.state, PSPGfxFwType::RLC_G) != nullptr);
}

void reuse() {
    Fixture f;
    active = &f;
    CHECK(fw_loader_prepare(f.dev, f.psp, f.state) == kIOReturnSuccess);
    auto loader = fw_loader_make(f.state);
    uint64_t bus = 0;
    uint32_t size = 0;
    CHECK(loader.get_payload(PSPGfxFwType::SMU, bus, size) == kIOReturnSuccess);
    f.absent[Fixture::index(FwId::SMU)] = true;
    f.rejected(kIOReturnNotFound);
    for (FwId id : requiredFiles) f.absent[Fixture::index(id)] = true;
    f.rejected(kIOReturnNotFound);
    for (FwId id : requiredFiles) f.absent[Fixture::index(id)] = false;
    CHECK(fw_loader_prepare(f.dev, f.psp, f.state) == kIOReturnSuccess);
    CHECK(f.psp.fwBufBumpOffset == 0);
    const auto *entry = fw_loader_find(f.state, PSPGfxFwType::SMU);
    CHECK(entry && !entry->staged && entry->mc == 0);
    const unsigned calls = f.stageCalls;
    CHECK(loader.get_payload(PSPGfxFwType::SMU, bus, size) == kIOReturnSuccess);
    CHECK(f.stageCalls == calls + 1 && bus == stageBase);
}

void stage_failure_retry() {
    Fixture f;
    active = &f;
    CHECK(fw_loader_prepare(f.dev, f.psp, f.state) == kIOReturnSuccess);
    auto loader = fw_loader_make(f.state);
    f.stageResult = kIOReturnIOError;
    uint64_t bus = 123;
    uint32_t size = 456;
    CHECK(loader.get_payload(PSPGfxFwType::IMU_D, bus, size) == kIOReturnIOError);
    CHECK(bus == 123 && size == 456);
    const auto *entry = fw_loader_find(f.state, PSPGfxFwType::IMU_D);
    CHECK(entry && !entry->staged);
    f.stageResult = kIOReturnSuccess;
    CHECK(loader.get_payload(PSPGfxFwType::IMU_D, bus, size) == kIOReturnSuccess);
    CHECK(f.stageCalls == 2 && bus == stageBase && size == 4);
}
} // namespace

namespace amdgpu {
const FwBlob *fw_get(FwId id) {
    const size_t i = Fixture::index(id);
    return active->absent[i] ? nullptr : &active->blobs[i];
}

kern_return_t psp_fw_buf_stage(DeviceContext &dev, PSPContext &psp,
                               const void *bytes, uint32_t size, uint64_t *out) {
    CHECK(&dev == &active->dev && &psp == &active->psp);
    ++active->stageCalls;
    active->stagedBytes = bytes;
    active->stagedSize = size;
    if (active->stageResult != kIOReturnSuccess) return active->stageResult;
    *out = stageBase + psp.fwBufBumpOffset;
    psp.fwBufBumpOffset += 0x1000;
    return kIOReturnSuccess;
}

void rdna4_logf(const char *, ...) {}
} // namespace amdgpu

int main(int argc, char **argv) {
    struct Test { const char *name; void (*run)(); };
    const Test tests[] = {
        {"valid_and_lazy_stage", valid_and_lazy_stage},
        {"missing_files", missing_files}, {"malformed_files", malformed_files},
        {"partial_imu", partial_imu}, {"partial_rs64", partial_rs64},
        {"partial_mes", partial_mes}, {"rlc_requires_g", rlc_requires_g},
        {"optional_rlc", optional_rlc}, {"reuse", reuse},
        {"stage_failure_retry", stage_failure_retry},
    };
    unsigned ran = 0;
    for (const Test &test : tests) {
        if (argc > 1 && std::strcmp(argv[1], test.name) != 0) continue;
        scenario = test.name;
        const unsigned before = failures;
        test.run();
        std::printf("%s %s\n", before == failures ? "PASS" : "FAIL", test.name);
        ++ran;
    }
    std::printf("%u scenarios, %u failed checks\n", ran, failures);
    return ran && !failures ? 0 : 1;
}
