// edid_modes_pure.h — pure EDID / CTA-861 / DisplayID timing parser and bounded mode builder.
// Header-only, no kernel or IOKit dependencies; usable in both kernel and host tests.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace rdna4edidmodes {

constexpr size_t BlockSize = 128u;
constexpr size_t MaxBlocks = 4u;
constexpr size_t MaxDisplayIdTimings = 8u;
constexpr size_t MaxModes = 64u;
constexpr uint64_t MaxPixelClockHz = 3000000000ULL;

enum Source : uint32_t {
    SourceEdid = 0u,
    SourceDisplayId = 1u,
    SourceConsoleFallback = 2u,
    SourceCtaVic = 3u,
    SourceStandardTiming = 4u,
    SourceCvtRb2 = 5u,
};

struct DetailedTiming {
    uint64_t pixelClockHz;
    uint32_t width;
    uint32_t height;
    uint32_t hBlank;
    uint32_t hSyncOffset;
    uint32_t hSyncWidth;
    uint32_t vBlank;
    uint32_t vSyncOffset;
    uint32_t vSyncWidth;
    bool hPositive;
    bool vPositive;
    bool interlaced;
    bool preferred;
    uint32_t source;

    uint64_t hTotal() const { return static_cast<uint64_t>(width) + hBlank; }
    uint64_t vTotal() const { return static_cast<uint64_t>(height) + vBlank; }

    uint32_t refreshMilliHz() const {
        const uint64_t denom = hTotal() * vTotal();
        if (!denom || !pixelClockHz) return 0u;
        const uint64_t q = pixelClockHz / denom;
        const uint64_t r = pixelClockHz % denom;
        return static_cast<uint32_t>(q * 1000ULL + ((r * 1000ULL + denom / 2ULL) / denom));
    }

    // 16.16 fixed-point refresh rate without __uint128 division (safe under -mkernel).
    uint32_t refresh1616() const {
        const uint64_t denom = hTotal() * vTotal();
        if (!denom || !pixelClockHz) return 0u;
        const uint64_t q = pixelClockHz / denom;
        const uint64_t r = pixelClockHz % denom;
        return static_cast<uint32_t>((q << 16) + (((r << 16) + denom / 2ULL) / denom));
    }
};

struct Limits {
    uint32_t minRefreshMilliHz;
    uint32_t maxRefreshMilliHz;
    uint32_t minHHz;
    uint32_t maxHHz;
    uint64_t maxPixelClockHz;
    bool valid;
};

struct Mode {
    uint32_t modeId;
    DetailedTiming timing;
};

struct ModeList {
    Mode modes[MaxModes];
    size_t count;
    Limits limits;
};

inline uint16_t readLe16(const uint8_t *p) {
    return static_cast<uint16_t>(static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8));
}

inline uint32_t readLe24(const uint8_t *p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16);
}

inline bool blockChecksumOk(const uint8_t *block) {
    if (!block) return false;
    uint32_t sum = 0u;
    for (size_t i = 0; i < BlockSize; ++i) sum += block[i];
    return (sum & 0xFFu) == 0u;
}

inline bool baseBlockValid(const uint8_t *base) {
    if (!base) return false;
    static constexpr uint8_t kHeader[8] = {0x00u, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0x00u};
    for (size_t i = 0; i < 8u; ++i) {
        if (base[i] != kHeader[i]) return false;
    }
    return blockChecksumOk(base);
}

inline size_t readExtensions(const uint8_t *edid, size_t bytes, size_t *outBlockIndices = nullptr) {
    if (!edid || bytes < BlockSize || !baseBlockValid(edid)) return 0u;
    if (outBlockIndices) outBlockIndices[0] = 0u;
    size_t validCount = 1u;
    const size_t availableExt = (bytes / BlockSize) - 1u;
    size_t declaredExt = edid[126];
    if (declaredExt > availableExt) declaredExt = availableExt;
    if (declaredExt > MaxBlocks - 1u) declaredExt = MaxBlocks - 1u;
    for (size_t i = 1u; i <= declaredExt; ++i) {
        const uint8_t *ext = edid + i * BlockSize;
        if (!blockChecksumOk(ext)) continue;
        if (outBlockIndices) outBlockIndices[validCount] = i;
        ++validCount;
    }
    return validCount;
}

inline bool parseDtd(const uint8_t *d, bool preferred, DetailedTiming &out) {
    if (!d) return false;
    const uint16_t pclk10k = readLe16(d);
    if (pclk10k == 0u) return false;
    const uint32_t width = static_cast<uint32_t>(d[2]) | ((static_cast<uint32_t>(d[4] & 0xF0u)) << 4);
    const uint32_t hBlank = static_cast<uint32_t>(d[3]) | ((static_cast<uint32_t>(d[4] & 0x0Fu)) << 8);
    const uint32_t height = static_cast<uint32_t>(d[5]) | ((static_cast<uint32_t>(d[7] & 0xF0u)) << 4);
    const uint32_t vBlank = static_cast<uint32_t>(d[6]) | ((static_cast<uint32_t>(d[7] & 0x0Fu)) << 8);
    const uint32_t hSyncOff = static_cast<uint32_t>(d[8]) | ((static_cast<uint32_t>(d[11] & 0xC0u)) << 2);
    const uint32_t hSyncW = static_cast<uint32_t>(d[9]) | ((static_cast<uint32_t>(d[11] & 0x30u)) << 4);
    const uint32_t vSyncOff = static_cast<uint32_t>(d[10] >> 4) | ((static_cast<uint32_t>(d[11] & 0x0Cu)) << 2);
    const uint32_t vSyncW = static_cast<uint32_t>(d[10] & 0x0Fu) | ((static_cast<uint32_t>(d[11] & 0x03u)) << 4);
    if (!width || !height || !hBlank || !vBlank) return false;

    out = {};
    out.pixelClockHz = static_cast<uint64_t>(pclk10k) * 10000ULL;
    out.width = width;
    out.height = height;
    out.hBlank = hBlank;
    out.hSyncOffset = hSyncOff;
    out.hSyncWidth = hSyncW;
    out.vBlank = vBlank;
    out.vSyncOffset = vSyncOff;
    out.vSyncWidth = vSyncW;
    // EDID 1.4 byte 17 bits 4..3 == 0b11 for digital separate sync; bit 2 = VSync+, bit 1 = HSync+.
    const bool digitalSep = (d[17] & 0x18u) == 0x18u;
    out.vPositive = digitalSep ? ((d[17] & 0x04u) != 0u) : true;
    out.hPositive = digitalSep ? ((d[17] & 0x02u) != 0u) : true;
    out.interlaced = (d[17] & 0x80u) != 0u;
    out.preferred = preferred;
    out.source = SourceEdid;
    return true;
}

inline size_t parseBaseBlock(const uint8_t *base, DetailedTiming *out, size_t maxOut, Limits *outLimits = nullptr) {
    if (outLimits) *outLimits = {};
    if (!baseBlockValid(base)) return 0u;
    size_t count = 0u;
    for (size_t i = 0u; i < 4u; ++i) {
        const uint8_t *d = base + 54u + i * 18u;
        if (d[0] == 0u && d[1] == 0u && d[2] == 0u && d[3] == 0xFDu && (d[4] & 0xF0u) == 0u) {
            if (outLimits) {
                uint32_t minV = d[5];
                uint32_t maxV = d[6];
                uint32_t minH = d[7];
                uint32_t maxH = d[8];
                if (d[4] & 0x01u) minV += 255u;
                if (d[4] & 0x02u) maxV += 255u;
                if (d[4] & 0x04u) minH += 255u;
                if (d[4] & 0x08u) maxH += 255u;
                if (minV > 0u && maxV >= minV) {
                    outLimits->minRefreshMilliHz = minV * 1000u;
                    outLimits->maxRefreshMilliHz = maxV * 1000u;
                    outLimits->minHHz = minH * 1000u;
                    outLimits->maxHHz = maxH * 1000u;
                    outLimits->maxPixelClockHz = static_cast<uint64_t>(d[9]) * 10000000ULL;
                    outLimits->valid = true;
                }
            }
            continue;
        }
        DetailedTiming t{};
        if (out && count < maxOut && parseDtd(d, i == 0u, t)) {
            out[count++] = t;
        }
    }
    return count;
}

inline bool ctaVicTiming(uint8_t vic, DetailedTiming &out) {
    struct VicRow {
        uint8_t vic;
        uint16_t w, h, hb, hso, hsw, vb, vso, vsw;
        uint32_t clkKhz;
    };
    // Standard progressive 16:9 CTA-861-G VICs (positive H/V sync).
    static constexpr VicRow kVics[] = {
        {   4u, 1280u,  720u,  370u,  110u, 40u,  30u, 5u, 5u,   74250u }, // 720p60
        {  19u, 1280u,  720u,  700u,  440u, 40u,  30u, 5u, 5u,   74250u }, // 720p50
        {  16u, 1920u, 1080u,  280u,   88u, 44u,  45u, 4u, 5u,  148500u }, // 1080p60
        {  31u, 1920u, 1080u,  720u,  528u, 44u,  45u, 4u, 5u,  148500u }, // 1080p50
        {  63u, 1920u, 1080u,  280u,   88u, 44u,  45u, 4u, 5u,  297000u }, // 1080p120
        {  64u, 1920u, 1080u,  720u,  528u, 44u,  45u, 4u, 5u,  297000u }, // 1080p100
        {  96u, 3840u, 2160u, 1440u, 1056u, 88u,  90u, 8u, 10u, 594000u }, // 2160p50
        {  97u, 3840u, 2160u,  560u,  176u, 88u,  90u, 8u, 10u, 594000u }, // 2160p60
        { 117u, 3840u, 2160u, 1440u, 1056u, 88u,  90u, 8u, 10u,1188000u }, // 2160p100
        { 118u, 3840u, 2160u,  560u,  176u, 88u,  90u, 8u, 10u,1188000u }, // 2160p120
    };
    for (size_t i = 0u; i < sizeof(kVics) / sizeof(kVics[0]); ++i) {
        if (kVics[i].vic != vic) continue;
        out = {};
        out.pixelClockHz = static_cast<uint64_t>(kVics[i].clkKhz) * 1000ULL;
        out.width = kVics[i].w;
        out.height = kVics[i].h;
        out.hBlank = kVics[i].hb;
        out.hSyncOffset = kVics[i].hso;
        out.hSyncWidth = kVics[i].hsw;
        out.vBlank = kVics[i].vb;
        out.vSyncOffset = kVics[i].vso;
        out.vSyncWidth = kVics[i].vsw;
        out.hPositive = true;
        out.vPositive = true;
        out.interlaced = false;
        out.preferred = false;
        out.source = SourceCtaVic;
        return true;
    }
    return false;
}

inline size_t parseCtaVics(const uint8_t *ext, DetailedTiming *out, size_t maxOut) {
    if (!ext || !out || maxOut == 0u || ext[0] != 0x02u || !blockChecksumOk(ext)) return 0u;
    const size_t dtdStart = ext[2];
    if (dtdStart <= 4u || dtdStart > BlockSize - 1u) return 0u;
    size_t count = 0u;
    size_t pos = 4u;
    while (pos < dtdStart && count < maxOut) {
        const uint8_t hdr = ext[pos];
        const uint8_t tag = (hdr >> 5) & 0x07u;
        const size_t len = hdr & 0x1Fu;
        if (pos + 1u + len > dtdStart) break;
        if (tag == 2u) { // CTA-861 Video Data Block
            for (size_t i = 0u; i < len && count < maxOut; ++i) {
                const uint8_t svd = ext[pos + 1u + i];
                // Short Video Descriptor: 1..64 use bit 7 as native flag; 65..127 and 129..192 use full byte.
                const uint8_t vic = (svd >= 129u && svd <= 192u) ? (svd & 0x7Fu) : svd;
                DetailedTiming t{};
                if (ctaVicTiming(vic, t)) {
                    out[count++] = t;
                }
            }
        }
        pos += 1u + len;
    }
    return count;
}

inline DetailedTiming makeCvtRb2Timing(uint32_t width, uint32_t height, uint32_t refreshHz,
                                       uint32_t maxHHz = 0u, uint64_t maxClockHz = 0ULL) {
    DetailedTiming t{};
    if (!width || !height || !refreshHz) return t;
    // VESA CVT-RB (universal aspect-ratio VSync per VESA CVT Table 3-2):
    // HBlank=80 (front=8, sync=32, back=40), VFront=3, VBack = vBlank - VFront - VSync.
    const uint32_t hBlank = 80u;
    const uint32_t hTotal = width + hBlank;
    const uint32_t vFront = 3u;
    uint32_t vSync = 8u;
    if (width * 3u == height * 4u) vSync = 4u;       // 4:3
    else if (width * 9u == height * 16u) vSync = 5u; // 16:9
    else if (width * 10u == height * 16u) vSync = 6u;// 16:10
    else if (width * 4u == height * 5u || width * 9u == height * 15u) vSync = 7u; // 5:4 / 15:9
    const uint64_t r = refreshHz;
    const uint64_t vbiUs = 460ULL;
    const uint64_t frameNs = 1000000000ULL;
    const uint64_t denom = (frameNs > vbiUs * 1000ULL * r) ? (frameNs - vbiUs * 1000ULL * r) : 1ULL;
    uint32_t vBlank = static_cast<uint32_t>(
        (vbiUs * 1000ULL * static_cast<uint64_t>(height) * r + denom - 1ULL) / denom);
    if (vBlank < vFront + vSync + 6u) vBlank = vFront + vSync + 6u;

    uint64_t vTotalLimit = 0ULL;
    if (maxHHz > 0u) {
        const uint64_t hLim = static_cast<uint64_t>(maxHHz) / r;
        if (hLim > height) vTotalLimit = hLim;
    }
    if (maxClockHz > 0ULL) {
        const uint64_t cLim = maxClockHz / (static_cast<uint64_t>(hTotal) * r);
        if (cLim > height && (vTotalLimit == 0ULL || cLim < vTotalLimit)) {
            vTotalLimit = cLim;
        }
    }
    if (vTotalLimit > 0ULL && static_cast<uint64_t>(height) + vBlank > vTotalLimit) {
        const uint64_t compactVb = vTotalLimit - height;
        if (compactVb >= 32ULL) {
            vBlank = static_cast<uint32_t>(compactVb);
        }
    }
    const uint64_t vTotal = static_cast<uint64_t>(height) + vBlank;
    t.width = width;
    t.height = height;
    t.hBlank = hBlank;
    t.hSyncOffset = 8u;
    t.hSyncWidth = 32u;
    t.vBlank = vBlank;
    t.vSyncOffset = vFront;
    t.vSyncWidth = vSync;
    t.pixelClockHz = static_cast<uint64_t>(hTotal) * vTotal * r;
    t.hPositive = true;
    t.vPositive = false;
    t.interlaced = false;
    t.preferred = false;
    t.source = SourceCvtRb2;
    return t;
}

inline size_t parseCtaBlock(const uint8_t *ext, DetailedTiming *out, size_t maxOut) {
    if (!ext || !out || maxOut == 0u || ext[0] != 0x02u || !blockChecksumOk(ext)) return 0u;
    const size_t dtdStart = ext[2];
    if (dtdStart < 4u || dtdStart >= BlockSize - 1u) return 0u;
    size_t count = 0u;
    for (size_t pos = dtdStart; pos + 18u <= BlockSize - 1u && count < maxOut; pos += 18u) {
        if (ext[pos] == 0u && ext[pos + 1u] == 0u) break;
        DetailedTiming t{};
        if (parseDtd(ext + pos, false, t)) {
            out[count++] = t;
        }
    }
    return count;
}

inline size_t parseDisplayIdBlock(const uint8_t *ext, DetailedTiming *out, size_t maxOut) {
    if (!ext || !out || maxOut == 0u || ext[0] != 0x70u || !blockChecksumOk(ext)) return 0u;
    const uint8_t version = ext[1];
    if (version != 0x12u && version != 0x13u && version != 0x20u && version != 0x21u) return 0u;
    const size_t payloadBytes = ext[2];
    const size_t end = 5u + payloadBytes;
    if (end >= BlockSize - 1u) return 0u;

    uint32_t innerSum = 0u;
    for (size_t i = 1u; i <= end; ++i) innerSum += ext[i];
    if ((innerSum & 0xFFu) != 0u) return 0u;

    const uint8_t wantTag = (version < 0x20u) ? 0x03u : 0x22u;
    const uint64_t clockMultHz = (version < 0x20u) ? 10000ULL : 1000ULL;

    size_t count = 0u;
    size_t pos = 5u;
    while (pos < end) {
        if (end - pos < 3u) break;
        const uint8_t tag = ext[pos];
        const uint8_t rev = ext[pos + 1u];
        const size_t bytes = ext[pos + 2u];
        if (tag == 0u && bytes == 0u) break; // DisplayID zero-padding to end of section
        if (bytes > end - pos - 3u) return 0u;
        const size_t body = pos + 3u;

        if (tag == wantTag) {
            // DisplayID 2.0 Type VII rev 0/1/2 share the identical 20-byte timing layout.
            const uint8_t maxRev = (version >= 0x20u) ? 2u : 0u;
            if ((rev & 0x07u) > maxRev || bytes == 0u || (bytes % 20u) != 0u) return 0u;
            for (size_t off = 0u; off < bytes && count < maxOut && count < MaxDisplayIdTimings; off += 20u) {
                const uint8_t *e = ext + body + off;
                const uint8_t options = e[3];
                if (options & 0x70u) continue;
                const uint64_t pclkUnits = static_cast<uint64_t>(readLe24(e)) + 1ULL;
                const uint32_t width = static_cast<uint32_t>(readLe16(e + 4)) + 1u;
                const uint32_t hBlank = static_cast<uint32_t>(readLe16(e + 6)) + 1u;
                const uint16_t hOffPol = readLe16(e + 8);
                const uint32_t hSyncOff = static_cast<uint32_t>(hOffPol & 0x7FFFu) + 1u;
                const uint32_t hSyncW = static_cast<uint32_t>(readLe16(e + 10)) + 1u;
                const uint32_t height = static_cast<uint32_t>(readLe16(e + 12)) + 1u;
                const uint32_t vBlank = static_cast<uint32_t>(readLe16(e + 14)) + 1u;
                const uint16_t vOffPol = readLe16(e + 16);
                const uint32_t vSyncOff = static_cast<uint32_t>(vOffPol & 0x7FFFu) + 1u;
                const uint32_t vSyncW = static_cast<uint32_t>(readLe16(e + 18)) + 1u;
                if (width > 0xFFFFu || hBlank > 0xFFFFu || hSyncOff > 0xFFFFu || hSyncW > 0xFFFFu ||
                    height > 0xFFFFu || vBlank > 0xFFFFu || vSyncOff > 0xFFFFu || vSyncW > 0xFFFFu ||
                    width + hBlank > 0xFFFFu || height + vBlank > 0xFFFFu ||
                    hSyncOff + hSyncW > hBlank || vSyncOff + vSyncW > vBlank) {
                    continue;
                }

                DetailedTiming t{};
                t.pixelClockHz = pclkUnits * clockMultHz;
                t.width = width;
                t.height = height;
                t.hBlank = hBlank;
                t.hSyncOffset = hSyncOff;
                t.hSyncWidth = hSyncW;
                t.vBlank = vBlank;
                t.vSyncOffset = vSyncOff;
                t.vSyncWidth = vSyncW;
                t.hPositive = (hOffPol & 0x8000u) != 0u;
                t.vPositive = (vOffPol & 0x8000u) != 0u;
                t.interlaced = false;
                t.preferred = (options & 0x80u) != 0u;
                t.source = SourceDisplayId;
                out[count++] = t;
            }
        }
        pos = body + bytes;
    }
    return count;
}

inline bool surfaceFitsFramebuffer(uint32_t width, uint32_t height, uint32_t fbWidth,
                                   uint32_t fbHeight, uint32_t rowBytes) {
    if (!width || !height || !fbWidth || !fbHeight || !rowBytes) return false;
    if (width > fbWidth || height > fbHeight) return false;
    return static_cast<uint64_t>(width) * 4ULL <= static_cast<uint64_t>(rowBytes);
}

inline DetailedTiming makeConsoleTiming(uint32_t width, uint32_t height, uint32_t refreshHz = 60u) {
    DetailedTiming t{};
    t.width = width ? width : 1920u;
    t.height = height ? height : 1080u;
    t.hBlank = 280u;
    t.hSyncOffset = 88u;
    t.hSyncWidth = 44u;
    t.vBlank = 45u;
    t.vSyncOffset = 4u;
    t.vSyncWidth = 5u;
    t.pixelClockHz = (static_cast<uint64_t>(t.width) + t.hBlank) *
                     (static_cast<uint64_t>(t.height) + t.vBlank) *
                     static_cast<uint64_t>(refreshHz ? refreshHz : 60u);
    t.hPositive = true;
    t.vPositive = true;
    t.interlaced = false;
    t.preferred = true;
    t.source = SourceConsoleFallback;
    return t;
}

inline bool timingDuplicate(const ModeList &list, const DetailedTiming &t) {
    const uint32_t r = t.refreshMilliHz();
    for (size_t i = 0; i < list.count; ++i) {
        const DetailedTiming &e = list.modes[i].timing;
        if (e.width == t.width && e.height == t.height &&
            e.refreshMilliHz() == r && e.interlaced == t.interlaced) {
            return true;
        }
    }
    return false;
}

inline bool hasNearRefresh(const ModeList &list, uint32_t width, uint32_t height, uint32_t targetHz) {
    const uint32_t targetMilliHz = targetHz * 1000u;
    for (size_t i = 0; i < list.count; ++i) {
        const DetailedTiming &e = list.modes[i].timing;
        if (e.width != width || e.height != height || e.interlaced) continue;
        const uint32_t r = e.refreshMilliHz();
        const uint32_t diff = (r >= targetMilliHz) ? (r - targetMilliHz) : (targetMilliHz - r);
        if (diff <= 1500u) return true;
    }
    return false;
}

inline void appendTimingIfBounded(ModeList &list, const DetailedTiming &t, uint32_t fbWidth,
                                  uint32_t fbHeight, uint32_t rowBytes,
                                  uint64_t maxClockHz = MaxPixelClockHz,
                                  bool sameSizeOnly = false) {
    if (list.count >= MaxModes) return;
    const uint64_t cap = (maxClockHz > 0ULL && maxClockHz < MaxPixelClockHz) ? maxClockHz : MaxPixelClockHz;
    if (!t.pixelClockHz || t.pixelClockHz > cap) return;
    if (!surfaceFitsFramebuffer(t.width, t.height, fbWidth, fbHeight, rowBytes)) return;
    if (sameSizeOnly && (t.width != fbWidth || t.height != fbHeight)) return;
    if (t.interlaced) return;
    const uint32_t mhz = t.refreshMilliHz();
    if (!mhz) return;
    if (t.source != SourceEdid && t.source != SourceCtaVic &&
        list.limits.valid && list.limits.minRefreshMilliHz > 0u &&
        list.limits.maxRefreshMilliHz >= list.limits.minRefreshMilliHz) {
        if (mhz < list.limits.minRefreshMilliHz || mhz > list.limits.maxRefreshMilliHz) return;
    }
    if (timingDuplicate(list, t)) return;
    Mode &m = list.modes[list.count];
    m.modeId = static_cast<uint32_t>(list.count + 1u);
    m.timing = t;
    ++list.count;
}

inline size_t build(const uint8_t *edid, size_t edidBytes, uint32_t fbWidth, uint32_t fbHeight,
                    uint32_t rowBytes, ModeList &out, uint64_t maxClockHz = MaxPixelClockHz,
                    bool sameSizeOnly = false, bool synthesizeRefreshSteps = false) {
    out = {};
    size_t blockIndices[MaxBlocks]{};
    const size_t validBlocks = readExtensions(edid, edidBytes, blockIndices);

    if (validBlocks > 0u) {
        DetailedTiming baseTimings[4]{};
        const size_t baseCount = parseBaseBlock(edid, baseTimings, 4u, &out.limits);
        for (size_t i = 0u; i < baseCount; ++i) {
            appendTimingIfBounded(out, baseTimings[i], fbWidth, fbHeight, rowBytes, maxClockHz, sameSizeOnly);
        }
        for (size_t b = 1u; b < validBlocks; ++b) {
            const uint8_t *ext = edid + blockIndices[b] * BlockSize;
            if (ext[0] == 0x70u) {
                DetailedTiming didTimings[MaxDisplayIdTimings]{};
                const size_t n = parseDisplayIdBlock(ext, didTimings, MaxDisplayIdTimings);
                for (size_t i = 0u; i < n; ++i) {
                    appendTimingIfBounded(out, didTimings[i], fbWidth, fbHeight, rowBytes, maxClockHz, sameSizeOnly);
                }
            } else if (ext[0] == 0x02u) {
                DetailedTiming ctaTimings[6]{};
                const size_t n = parseCtaBlock(ext, ctaTimings, 6u);
                for (size_t i = 0u; i < n; ++i) {
                    appendTimingIfBounded(out, ctaTimings[i], fbWidth, fbHeight, rowBytes, maxClockHz, sameSizeOnly);
                }
                DetailedTiming vicTimings[16]{};
                const size_t nv = parseCtaVics(ext, vicTimings, 16u);
                for (size_t i = 0u; i < nv; ++i) {
                    appendTimingIfBounded(out, vicTimings[i], fbWidth, fbHeight, rowBytes, maxClockHz, sameSizeOnly);
                }
            }
        }
    }

    if (surfaceFitsFramebuffer(fbWidth, fbHeight, fbWidth, fbHeight, rowBytes)) {
        bool haveConsoleSize = false;
        for (size_t i = 0u; i < out.count; ++i) {
            if (out.modes[i].timing.width == fbWidth && out.modes[i].timing.height == fbHeight) {
                haveConsoleSize = true;
                break;
            }
        }
        if (!haveConsoleSize && out.count < MaxModes) {
            Mode &m = out.modes[out.count];
            m.modeId = static_cast<uint32_t>(out.count + 1u);
            m.timing = makeConsoleTiming(fbWidth, fbHeight);
            ++out.count;
        }
        // Fill standard high-refresh tiers within monitor range limits and hardware clock budget
        // without duplicating any factory EDID / CTA / DisplayID timing.
        if (synthesizeRefreshSteps && out.limits.valid && out.limits.maxRefreshMilliHz > 60000u) {
            uint64_t effCap = (maxClockHz > 0ULL && maxClockHz < MaxPixelClockHz) ? maxClockHz : MaxPixelClockHz;
            if (out.limits.maxPixelClockHz > 0ULL && out.limits.maxPixelClockHz < effCap) {
                effCap = out.limits.maxPixelClockHz;
            }
            const uint32_t maxHz = out.limits.maxRefreshMilliHz / 1000u;
            const uint32_t minHz = out.limits.minRefreshMilliHz / 1000u;
            const uint32_t steps[] = { 50u, 60u, 75u, 90u, 100u, 120u, 144u, 160u, 165u, 180u, 240u, maxHz };
            for (size_t i = 0u; i < sizeof(steps) / sizeof(steps[0]); ++i) {
                const uint32_t hz = steps[i];
                if (hz < minHz || hz > maxHz) continue;
                if (hasNearRefresh(out, fbWidth, fbHeight, hz)) continue;
                const DetailedTiming cvt = makeCvtRb2Timing(fbWidth, fbHeight, hz, out.limits.maxHHz, effCap);
                if (cvt.pixelClockHz == 0ULL || cvt.pixelClockHz > effCap) continue;
                if (out.limits.maxHHz > 0u && cvt.vTotal() * static_cast<uint64_t>(hz) > out.limits.maxHHz) {
                    continue;
                }
                appendTimingIfBounded(out, cvt, fbWidth, fbHeight, rowBytes, effCap, sameSizeOnly);
            }
        }
    }
    return out.count;
}

inline const Mode *findById(const ModeList &list, uint32_t modeId) {
    for (size_t i = 0u; i < list.count; ++i) {
        if (list.modes[i].modeId == modeId) return &list.modes[i];
    }
    return nullptr;
}

inline const Mode *preferredAtSize(const ModeList &list, uint32_t width, uint32_t height) {
    const Mode *best = nullptr;
    uint32_t bestRefresh = 0u;
    bool bestPref = false;
    for (size_t i = 0u; i < list.count; ++i) {
        const Mode &m = list.modes[i];
        if (m.timing.width != width || m.timing.height != height) continue;
        const uint32_t r = m.timing.refreshMilliHz();
        if (!best || r > bestRefresh || (r == bestRefresh && m.timing.preferred && !bestPref)) {
            best = &m;
            bestRefresh = r;
            bestPref = m.timing.preferred;
        }
    }
    return best;
}

inline bool measuredRateMatches(uint32_t targetMilliHz, uint32_t measuredMilliHz) {
    if (!targetMilliHz || !measuredMilliHz) return false;
    const uint32_t diff = (targetMilliHz >= measuredMilliHz)
                              ? (targetMilliHz - measuredMilliHz)
                              : (measuredMilliHz - targetMilliHz);
    return static_cast<uint64_t>(diff) * 100ULL <= static_cast<uint64_t>(targetMilliHz) * 2ULL;
}

} // namespace rdna4edidmodes
