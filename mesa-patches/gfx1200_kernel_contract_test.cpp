// SPDX-License-Identifier: MIT
#include "garnet_submit_pure.h"
#include <cstdio>
#include <cstring>

int main() {
    struct Packet { rdna4n_cs_in header; rdna4n_cs_ib ib; } p {};
    p.header.abi = RDNA4N_ABI_VERSION;
    p.header.ctx_id = 1;
    p.header.num_ibs = 1;
    p.ib.va_start = 0x10000;
    p.ib.ib_bytes = 4;
    rdna4native::s1c::CsView view {};
    for (uint32_t ip = 0; ip < 2; ++ip) {
        p.ib.ip_type = ip;
        uint32_t result = rdna4native::s1c::cs_parse(reinterpret_cast<const uint8_t *>(&p), sizeof(p), &view);
        std::printf("kernel cs_parse ip_type=%u ring=0 result=%#x\n", ip, result);
        if (result != (ip == 0 ? rdna4native::s1c::kOk : rdna4native::s1c::kBadArg)) return 1;
    }
    std::printf("kernel QueryInfo layout: size=%zu gc_version=%zu vram_vis_total=%zu vram_hi_total=%zu\n",
                sizeof(rdna4n_info), __builtin_offsetof(rdna4n_info, gc_version),
                __builtin_offsetof(rdna4n_info, vram_vis_total), __builtin_offsetof(rdna4n_info, vram_hi_total));
    return sizeof(rdna4n_info) == 192 && __builtin_offsetof(rdna4n_info, gc_version) == 20 &&
           __builtin_offsetof(rdna4n_info, vram_vis_total) == 32 &&
           __builtin_offsetof(rdna4n_info, vram_hi_total) == 48 ? 0 : 1;
}
