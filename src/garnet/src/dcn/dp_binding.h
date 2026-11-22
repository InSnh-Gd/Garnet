#ifndef RDNA4_DP_BINDING_H
#define RDNA4_DP_BINDING_H
#include "dp_aux_transport.h"

#define RDNA4_DPCD_BOUND_FUNCTION "RDNA4DpcdBoundSnapshotV1"
enum { RDNA4_BIND_OK = 0, RDNA4_BIND_INVALID, RDNA4_BIND_UNREADABLE, RDNA4_BIND_SOURCE,
       RDNA4_BIND_AMBIGUOUS, RDNA4_BIND_ENCODER, RDNA4_BIND_HPD, RDNA4_BIND_MUX,
       RDNA4_BIND_CHANGED, RDNA4_BIND_VBIOS, RDNA4_BIND_TRANSPORT };
typedef struct {
    uint32_t version, size, dp, dig, otg, reserved;
    uint64_t pciRegistryId, deadlineUs;
} rdna4_bound_request;
typedef struct {
    rdna4_dpcd_snapshot link;
    uint32_t version, size, binding, dp, dig, otg, beControl, feControl, phyMux, portType;
} rdna4_bound_snapshot;
static inline uint32_t rdna4_bound_validate(const rdna4_bound_request *r, uint64_t pci, uint64_t now) {
    if (!r || r->version != 1u || r->size != sizeof(*r) || r->reserved ||
        r->dp >= 4u || r->dig >= 4u || r->otg >= 4u || !pci || r->pciRegistryId != pci)
        return RDNA4_AUX_INVALID;
    if (r->deadlineUs <= now || r->deadlineUs - now > 50000u) return RDNA4_AUX_TIMEOUT;
    return RDNA4_AUX_OK;
}

static const uint32_t rdna4_binding_stream_offsets[] = {
    0x2122u, 0x211fu, 0x2120u, 0x2124u, 0x213du, 0x2093u, 0x2094u, 0x2095u};
static const uint32_t rdna4_binding_link_offsets[] = {0x20bcu, 0x20bbu, 0x20bdu, 0x211eu, 0x2121u, 0x212du};
static const uint32_t rdna4_binding_timing_offsets[] = {0x1b2au, 0x1b2fu, 0x1b2bu, 0x1b38u};
typedef struct { uint32_t stream[4][8], link[4][6], otg[4], timing[4][4], mux[4][2]; } rdna4_binding_raw;
static inline int rdna4_binding_read(uint32_t (*read)(void *, uint32_t), void *p, rdna4_binding_raw *r) {
    uint32_t i, j;
    for (i = 0; i < 4u; ++i) {
        for (j = 0; j < 2u; ++j) {
            r->mux[i][j] = read(p, 0x179au + 3u * i + j);
            if (r->mux[i][j] == 0xffffffffu) return 0;
        }
        r->otg[i] = read(p, 0x1b43u + i * 0x80u);
        if (r->otg[i] == 0xffffffffu) return 0;
        for (j = 0; j < 4u; ++j) {
            r->timing[i][j] = read(p, rdna4_binding_timing_offsets[j] + i * 0x80u);
            if (r->timing[i][j] == 0xffffffffu) return 0;
        }
        for (j = 0; j < 8u; ++j) {
            r->stream[i][j] = read(p, rdna4_binding_stream_offsets[j] + i * 0x124u);
            if (r->stream[i][j] == 0xffffffffu) return 0;
        }
        for (j = 0; j < 6u; ++j) {
            r->link[i][j] = read(p, rdna4_binding_link_offsets[j] + i * 0x124u);
            if (r->link[i][j] == 0xffffffffu) return 0;
        }
    }
    return 1;
}
static inline int rdna4_binding_same(const rdna4_binding_raw *a, const rdna4_binding_raw *b) {
    static const uint32_t sm[] = {0x10001u, 0x10731u, 0xff000000u, 0xffu, 0x11u, 0x107007u, 0x37u, 1u};
    /* BE HPD and all routing/swap bits are part of the source witness. */
    static const uint32_t lm[] = {0x70007f07u, 0x37u, 1u, 0x110u, 3u, 0x170u};
    uint32_t i, j;
    for (i = 0; i < 4u; ++i) {
        if ((a->mux[i][0] & 0x773u) != (b->mux[i][0] & 0x773u) ||
            (a->mux[i][1] & 1u) != (b->mux[i][1] & 1u)) return 0;
        if ((a->otg[i] & 1u) != (b->otg[i] & 1u)) return 0;
        for (j = 0; j < 4u; ++j)
            if ((a->timing[i][j] & (j < 2u ? 0x7fffu : 0x7fff7fffu)) !=
                (b->timing[i][j] & (j < 2u ? 0x7fffu : 0x7fff7fffu))) return 0;
        for (j = 0; j < 8u; ++j) if ((a->stream[i][j] & sm[j]) != (b->stream[i][j] & sm[j])) return 0;
        for (j = 0; j < 6u; ++j) if ((a->link[i][j] & lm[j]) != (b->link[i][j] & lm[j])) return 0;
    }
    return 1;
}
/* ObjectID.h -> link_factory.c -> dcn401_resource.c link_enc_regs[transmitter].
 * Only the four legacy UNIPHY A..D paths supported by this register reader. */
static inline uint32_t rdna4_binding_encoder(uint32_t object) {
    uint32_t id = object & 0xffu, instance = (object >> 8) & 0xfu;
    if ((object & 0xfffff000u) != 0x2000u || (instance != 1u && instance != 2u)) return 0xffffffffu;
    if (id == 0x1eu) return instance - 1u;
    if (id == 0x20u) return instance + 1u;
    return 0xffffffffu;
}
static inline uint32_t rdna4_binding_check(const rdna4_binding_raw *r, uint32_t dp, uint32_t dig,
                                        uint32_t otg, const rdna4_dpcd_snapshot *port) {
    uint32_t i, lit = 0, active = 0;
    const uint32_t *f, *b;
    if (!r || !port || dp >= 4u || dig >= 4u || otg >= 4u) return RDNA4_BIND_INVALID;
    if (!port->directDp || (port->connectorObj & 0xf0ffu) != 0x3013u || port->hpd >= 4u || port->aux >= 4u)
        return RDNA4_BIND_VBIOS;
    if (rdna4_binding_encoder(port->encoderObj) != dig) return RDNA4_BIND_ENCODER;
    for (i = 0; i < 4u; ++i) {
        lit += r->otg[i] & 1u;
        if ((r->stream[i][0] & 0x10001u) == 0x10001u) ++active;
    }
    if (lit > 1u || active > 1u) return RDNA4_BIND_AMBIGUOUS;
    f = r->stream[dp]; b = r->link[dig];
    if (!(r->otg[otg] & 1u) || (f[0] & 0x10001u) != 0x10001u ||
        (f[5] & 7u) != otg || (f[5] & 0x100000u) || (f[6] & 0x37u) != 0x10u ||
        !(f[7] & 1u) || (f[4] & 0x10u) || !(b[2] & 1u) ||
        (b[1] & 0x37u) != 0x10u || ((b[0] >> 8) & 0x7fu) != (1u << dp)) return RDNA4_BIND_SOURCE;
    /* ENC_TYPE_SEL=ENCODER_TYPE_DIG (link_encoder.h). Nonzero unmodelled
     * PORT_TYPE flags are refused, not interpreted as connector evidence. */
    if ((r->mux[dig][0] & 3u) || (r->mux[dig][1] & 1u)) return RDNA4_BIND_MUX;
    if (b[0] & 7u) return RDNA4_BIND_MUX; /* no proof for dual-link/swap/RB-switch */
    if (((b[0] >> 28) & 7u) != port->hpd) return RDNA4_BIND_HPD;
    for (i = 0; i < 4u; ++i) if (i != dig && (r->link[i][2] & 1u)) {
        if (((r->link[i][0] >> 8) & 0x7fu) & (1u << dp)) return RDNA4_BIND_AMBIGUOUS;
        if (((r->link[i][0] >> 28) & 7u) == port->hpd) return RDNA4_BIND_AMBIGUOUS;
    }
    return RDNA4_BIND_OK;
}
/* v1.5's reserved fields must be zero: no unmodelled external encoder/USB-C path.
 * The existing parser tolerates older layouts; this stronger proof does not. */
static inline int rdna4_binding_direct_path(const uint8_t *rom, size_t bytes, size_t offset,
                                           uint32_t connector, uint32_t encoder) {
    size_t size, count, i, j;
    uint32_t found = 0;
    if (!rom || offset > bytes || bytes - offset < 8u) return 0;
    size = rom[offset] | ((size_t)rom[offset + 1u] << 8);
    count = rom[offset + 6u];
    if (rom[offset + 2u] != 1u || rom[offset + 3u] != 5u ||
        size < 8u || size > bytes - offset || !count || count > RDNA4_DPCD_PORTS || size < 8u + count * 16u) return 0;
    for (i = 0; i < count; ++i) {
        const uint8_t *p = rom + offset + 8u + i * 16u;
        if ((uint32_t)(p[0] | ((uint32_t)p[1] << 8)) != connector ||
            (uint32_t)(p[4] | ((uint32_t)p[5] << 8)) != encoder) continue;
        ++found;
        for (j = 6u; j < 12u; ++j) if (p[j]) return 0;
        if (p[14] || p[15]) return 0;
    }
    return found == 1u;
}
#endif
