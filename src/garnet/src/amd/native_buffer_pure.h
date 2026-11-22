#pragma once
#include "native_s1c_pure.h"

namespace rdna4native { namespace s1c {

// Below the first user-mappable VA: users cannot overwrite the kernel-built IB.
constexpr uint64_t kBufferIbVa = 0x1000;
constexpr uint32_t kBufferChunk = 32736; // RADV GFX11+ CP DMA limit, rounded to 32 bytes.

inline bool buffer_address(const VaEnt *maps, uint32_t n, uint32_t handle,
                           uint64_t off, uint64_t len, uint32_t access, uint64_t *va) {
    for (uint32_t i = 0; i < n; ++i) {
        const auto &m = maps[i];
        if (m.handle != handle || (m.flags & access) != access || off < m.off) continue;
        const uint64_t delta = off - m.off;
        if (delta > m.size || len > m.size - delta) continue;
        *va = va_canonicalize(m.start + delta);
        return true;
    }
    return false;
}

inline uint32_t buffer_resolve(const rdna4n_buffer_op &q, bool fill, const VaEnt *maps, uint32_t n,
                               uint64_t srcSize, uint64_t dstSize, uint64_t *src, uint64_t *dst) {
    if (q.flags || q.reserved || !q.length || q.length > RDNA4N_BUFFER_MAX_BYTES ||
        ((q.src_offset | q.dst_offset | q.length) & 3) ||
        q.dst_offset > dstSize || q.length > dstSize - q.dst_offset) return kBadArg;
    if (fill) {
        if (q.src_handle || q.src_offset) return kBadArg;
    } else {
        if (q.pattern || q.src_offset > srcSize || q.length > srcSize - q.src_offset) return kBadArg;
        if (q.src_handle == q.dst_handle &&
            (q.src_offset <= q.dst_offset ? q.dst_offset - q.src_offset : q.src_offset - q.dst_offset) < q.length)
            return kBadArg;
    }
    uint64_t a = 0, b = 0;
    if ((!fill && !buffer_address(maps, n, q.src_handle, q.src_offset, q.length, RDNA4N_VM_PAGE_READABLE, &a)) ||
        !buffer_address(maps, n, q.dst_handle, q.dst_offset, q.length, RDNA4N_VM_PAGE_WRITEABLE, &b)) return kNotPermitted;
    *src = a; *dst = b;
    return kOk;
}

inline uint32_t buffer_build(uint32_t *out, uint32_t cap, uint64_t src, uint64_t dst,
                             uint64_t length, bool fill, uint32_t pattern) {
    if (!length || length > RDNA4N_BUFFER_MAX_BYTES || ((src | dst | length) & 3)) return 0;
    const uint32_t need = 18 + 7 * (uint32_t)((length + kBufferChunk - 1) / kBufferChunk);
    // Pad to eight dwords as required by the native IB parser and CP fetch.
    const uint32_t padded = (need + 7) & ~7u;
    if (cap < padded) return 0;
    uint32_t k = 0;
    auto acquire = [&]() {
        out[k++] = 0xc0065800; out[k++] = 0;
        out[k++] = 0xffffffff; out[k++] = 0xffffff;
        out[k++] = 0; out[k++] = 0; out[k++] = 10;
        out[k++] = 0x1c3f1; // SEQ, GL2 WB/INV, GL1/GLV/GLK/GLM INV/WB, GLI INV.
    };
    acquire();
    for (uint64_t off = 0; off < length;) {
        const uint32_t bytes = length - off > kBufferChunk ? kBufferChunk : (uint32_t)(length - off);
        const uint64_t source = fill ? pattern : src + off;
        const uint64_t dest = dst + off;
        out[k++] = 0xc0055000;
        out[k++] = fill ? 0xc0300000 : 0xe0300000; // CP_SYNC, GFX12 MALL source/destination.
        out[k++] = (uint32_t)source; out[k++] = (uint32_t)(source >> 32);
        out[k++] = (uint32_t)dest; out[k++] = (uint32_t)(dest >> 32);
        out[k++] = bytes | (1u << 30); // RAW_WAIT for earlier DMA writes.
        off += bytes;
    }
    out[k++] = 0xc0004200; out[k++] = 0; // PFP_SYNC_ME after the final CP_SYNC.
    acquire();
    while (k < padded) out[k++] = amdgpu::cp_p3_nop1();
    return k;
}

struct BufferBo { uint64_t size; bool live, host; };
struct BufferFlight { uint64_t seq; uint32_t id; };
constexpr bool buffer_reset_loses(BufferFlight flight, uint32_t liveId, uint64_t retired) {
    return flight.id == liveId && liveId != 0 && flight.seq > retired;
}

// Caller holds the session lock throughout; the environment owns only hardware effects.
template <class Env>
uint32_t buffer_run(Env &env, const rdna4n_buffer_op &q, bool fill, uint64_t *seqOut) {
    *seqOut = 0;
    uint32_t rc = env.gate();
    if (rc != kOk) return rc;
    const BufferBo dstBo = env.bo(q.dst_handle);
    const BufferBo srcBo = fill ? BufferBo{0, true, false} : env.bo(q.src_handle);
    if (!dstBo.live || !srcBo.live) return kNotFound;
    if (dstBo.host || srcBo.host) return kNotPermitted;
    uint64_t src = 0, dst = 0;
    rc = buffer_resolve(q, fill, env.maps(), env.map_count(), srcBo.size, dstBo.size, &src, &dst);
    if (rc != kOk) return rc;
    rc = env.idle();
    if (rc != kOk) return rc;
    rc = env.gate();
    if (rc != kOk) return rc;
    rc = env.prepare();
    if (rc != kOk) return rc;
    uint32_t packet[256];
    const uint32_t n = buffer_build(packet, 256, src, dst, q.length, fill, q.pattern);
    if (!n) return kBadArg;
    return env.submit(packet, n, seqOut);
}

} }
