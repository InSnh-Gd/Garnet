#ifndef RDNA4_DP_AUX_TRANSPORT_H
#define RDNA4_DP_AUX_TRANSPORT_H
#include <stdint.h>
#include <stddef.h>

/* Shared kernel-only plain-C contract. The provider is the FB on this PCI nub. */
#define RDNA4_DPCD_FUNCTION "RDNA4DpcdSnapshotV1"
#define RDNA4_DPCD_VERSION 1u
#define RDNA4_DPCD_PORTS 8u
enum { RDNA4_AUX_OK = 0, RDNA4_AUX_INVALID, RDNA4_AUX_BUSY, RDNA4_AUX_DISCONNECTED,
       RDNA4_AUX_TIMEOUT, RDNA4_AUX_IO, RDNA4_AUX_SHORT, RDNA4_AUX_NACK,
       RDNA4_AUX_DEFER, RDNA4_AUX_CHANGED, RDNA4_AUX_UNAVAILABLE };
typedef struct {
    uint32_t version, size, connector, reserved;
    uint64_t pciRegistryId, deadlineUs;
} rdna4_dpcd_request;
typedef struct {
    uint32_t version, size, status, connector;
    uint64_t pciRegistryId, generation, sampledUs;
    uint32_t connectorObj, encoderObj, aux, hpd;
    uint32_t directDp; /* verified direct v1.5 path; no USB-C/external mux */
    uint8_t config[9], lanes[6], reserved[5]; /* explicit tail: no property padding */
} rdna4_dpcd_snapshot;

/* Validate before touching the engine; request contains no arbitrary AUX op. */
static inline uint32_t rdna4_dpcd_validate(const rdna4_dpcd_request *r, uint64_t pci, uint64_t now) {
    if (!r || r->version != RDNA4_DPCD_VERSION || r->size != sizeof(*r) ||
        r->reserved || r->connector >= RDNA4_DPCD_PORTS || !pci || r->pciRegistryId != pci)
        return RDNA4_AUX_INVALID;
    if (r->deadlineUs <= now || r->deadlineUs - now > 50000u) return RDNA4_AUX_TIMEOUT;
    return RDNA4_AUX_OK;
}

typedef struct {
    void *cookie;
    uint32_t (*read)(void *, uint32_t off);
    int (*write)(void *, uint32_t off, uint32_t value);
    uint64_t (*now)(void *);
    void (*delay)(void *, uint32_t us);
} rdna4_aux_io;
typedef struct {
    rdna4_aux_io io;
    uint32_t inst, hpd, owned, requested, fault;
    uint64_t start, deadline;
} rdna4_aux_session;

static inline uint32_t rdna4_aux_reg(const rdna4_aux_session *s, uint32_t reg) {
    return 0x16b2u + s->inst * 0x1cu + reg;
}
static inline int rdna4_aux_time(rdna4_aux_session *s) {
    uint64_t now = s->io.now(s->io.cookie);
    if (now < s->start || now >= s->deadline) { s->fault = RDNA4_AUX_TIMEOUT; return 0; }
    return 1;
}
static inline int rdna4_aux_read(rdna4_aux_session *s, uint32_t off, uint32_t *v) {
    *v = s->io.read(s->io.cookie, off);
    if (*v == 0xffffffffu) { s->fault = RDNA4_AUX_IO; return 0; }
    return 1;
}
static inline int rdna4_aux_write(rdna4_aux_session *s, uint32_t reg, uint32_t v) {
    if (!rdna4_aux_time(s)) return 0;
    if (!s->io.write(s->io.cookie, rdna4_aux_reg(s, reg), v)) { s->fault = RDNA4_AUX_IO; return 0; }
    return 1;
}
/* No reset, enable, HPD acknowledgement, mux change or power write occurs here. */
static inline int rdna4_aux_present(rdna4_aux_session *s) {
    uint32_t ctl, hpd;
    if (!rdna4_aux_time(s) || !rdna4_aux_read(s, rdna4_aux_reg(s, 0), &ctl)) return 0;
    if ((ctl & 0x10031u) != 1u || ((ctl >> 20) & 7u) != s->hpd || (ctl & 0x10000000u)) {
        s->fault = RDNA4_AUX_INVALID; return 0;
    }
    if (!rdna4_aux_read(s, 0x175au + 8u * s->hpd, &hpd)) return 0;
    if ((hpd & 0x12u) != 0x12u) { s->fault = RDNA4_AUX_DISCONNECTED; return 0; }
    /* Pending HPD/RX events cannot be cleared by a read probe or used as an epoch. */
    if (hpd & 0x101u) { s->fault = RDNA4_AUX_CHANGED; return 0; }
    return 1;
}
static inline int rdna4_aux_valid(rdna4_aux_session *s) {
    uint32_t arb;
    if (!s->owned || !rdna4_aux_present(s) || !rdna4_aux_read(s, rdna4_aux_reg(s, 2), &arb)) return 0;
    if ((arb & 0xcu) != 4u || !(arb & 0x10000u)) { s->fault = RDNA4_AUX_CHANGED; return 0; }
    return 1;
}
/* The caller holds one device mutex across begin, every MOT/native request, end. */
static inline uint32_t rdna4_aux_begin(rdna4_aux_session *s) {
    uint32_t arb, sw;
    s->owned = s->requested = 0;
    s->fault = RDNA4_AUX_INVALID;
    if (!s->io.read || !s->io.write || !s->io.now || !s->io.delay || s->inst >= 4u || s->hpd >= 4u) return s->fault;
    s->start = s->io.now(s->io.cookie);
    if (!rdna4_aux_present(s) || !rdna4_aux_read(s, rdna4_aux_reg(s, 2), &arb) ||
        !rdna4_aux_read(s, rdna4_aux_reg(s, 1), &sw)) return s->fault;
    if ((arb & 0xcu) || (arb & 0x10000u) || (sw & 1u)) return s->fault = RDNA4_AUX_BUSY;
    if (!rdna4_aux_write(s, 2, (arb & ~0x20000u) | 0x10000u)) return s->fault;
    s->requested = 1;
    if (!rdna4_aux_read(s, rdna4_aux_reg(s, 2), &arb)) return s->fault;
    if ((arb & 0xcu) != 4u) return s->fault = RDNA4_AUX_BUSY;
    s->owned = 1;
    if (!rdna4_aux_valid(s)) return s->fault;
    return s->fault = RDNA4_AUX_OK;
}
static inline uint32_t rdna4_aux_end(rdna4_aux_session *s) {
    uint32_t arb, status = s->fault;
    /* Release/withdraw must run even after deadline, but never steal a new owner. */
    if (s->requested) {
        if (!rdna4_aux_read(s, rdna4_aux_reg(s, 2), &arb)) status = RDNA4_AUX_IO;
        else if ((arb & 0xcu) == 0u || (s->owned && (arb & 0xcu) == 4u)) {
            if (!s->io.write(s->io.cookie, rdna4_aux_reg(s, 2), (arb & ~0x10000u) | 0x20000u)) status = RDNA4_AUX_IO;
            else if (!rdna4_aux_read(s, rdna4_aux_reg(s, 2), &arb) || (arb & 0x10000u) || (arb & 0xcu) == 4u) status = RDNA4_AUX_CHANGED;
        } else status = RDNA4_AUX_CHANGED;
    }
    s->owned = s->requested = 0;
    return s->fault = status;
}

/* Same FIFO protocol as FB auxTransaction/dce_aux.c; grant spans whole sequences.
 * Returns the unmodified AUX reply nibble, or -1 with a typed fault. */
static inline int rdna4_aux_transaction(rdna4_aux_session *s, uint8_t action, uint32_t address,
                                     const uint8_t *data, uint8_t len,
                                     uint8_t *reply, uint8_t cap, uint8_t *received) {
    uint32_t sw, sts, value, n, i;
    int code;
    const int writing = (action & 0x10u) == 0u;
    if (received) *received = 0;
    s->fault = RDNA4_AUX_INVALID;
    if (address > 0xfffffu || len > 16u || cap > 16u || (writing && len && !data) ||
        (!writing && (len != cap || !reply)) ||
        (action != 0 && action != 0x10 && action != 0x40 && action != 0x50 && action != 0x80 && action != 0x90)) return -1;
    if (!rdna4_aux_valid(s) || !rdna4_aux_read(s, rdna4_aux_reg(s, 1), &sw)) return -1;
    /* A timed-out GO may still be running; even a cleanup STOP must not replace it. */
    if (sw & 1u) { s->fault = RDNA4_AUX_BUSY; return -1; }
    if (!rdna4_aux_write(s, 3, 2u)) return -1;
    for (i = 0; i < 100u; ++i) {
        if (!rdna4_aux_valid(s) || !rdna4_aux_read(s, rdna4_aux_reg(s, 4), &sts)) return -1;
        if (!(sts & 1u)) break;
        s->io.delay(s->io.cookie, 10u);
    }
    if (i == 100u) { s->fault = RDNA4_AUX_BUSY; return -1; }
    if (!rdna4_aux_read(s, rdna4_aux_reg(s, 1), &sw)) return -1;
    sw = (sw & ~0x1f0001u) | (((len ? 4u : 3u) + (writing ? len : 0u)) << 16);
    if (!rdna4_aux_write(s, 1, sw)) return -1;
    {
        uint8_t header[4];
        header[0] = (uint8_t)(action | ((address >> 16) & 0xfu));
        header[1] = (uint8_t)(address >> 8); header[2] = (uint8_t)address; header[3] = (uint8_t)(len - 1u);
        for (i = 0; i < (len ? 4u : 3u); ++i)
            if (!rdna4_aux_write(s, 6, ((uint32_t)header[i] << 8) | (i ? 0u : 0x80000000u))) return -1;
    }
    if (writing) for (i = 0; i < len; ++i)
        if (!rdna4_aux_write(s, 6, (uint32_t)data[i] << 8)) return -1;
    if (!rdna4_aux_valid(s) || !rdna4_aux_write(s, 1, sw | 1u)) return -1;
    for (i = 0; i < 2000u; ++i) {
        if (!rdna4_aux_valid(s) || !rdna4_aux_read(s, rdna4_aux_reg(s, 4), &sts)) return -1;
        if (sts & 1u) break;
        s->io.delay(s->io.cookie, 10u);
    }
    if (i == 2000u) { s->fault = RDNA4_AUX_TIMEOUT; return -1; }
    if (sts & 0x200u) { s->fault = RDNA4_AUX_DISCONNECTED; return -1; }
    if (sts & 0xf0u) { s->fault = RDNA4_AUX_TIMEOUT; return -1; }
    if (sts & 0xde5d00u) { s->fault = RDNA4_AUX_IO; return -1; }
    n = (sts >> 24) & 0x1fu;
    if (!n || n > 17u) { s->fault = RDNA4_AUX_SHORT; return -1; }
    if (!rdna4_aux_write(s, 6, 0x80000001u) ||
        !rdna4_aux_read(s, rdna4_aux_reg(s, 6), &value)) return -1;
    code = (int)((value >> 12) & 0xfu);
    if (code) { s->fault = (code == 2 || code == 8) ? RDNA4_AUX_DEFER : RDNA4_AUX_NACK; return code; }
    if (n - 1u != (writing ? 0u : len)) { s->fault = RDNA4_AUX_SHORT; return -1; }
    for (i = 0; i < n - 1u; ++i) {
        if (!rdna4_aux_read(s, rdna4_aux_reg(s, 6), &value)) return -1;
        reply[i] = (uint8_t)(value >> 8);
    }
    if (!rdna4_aux_valid(s)) return -1;
    if (received) *received = (uint8_t)(n - 1u);
    s->fault = RDNA4_AUX_OK;
    return code;
}
static inline uint32_t rdna4_aux_dpcd_block(rdna4_aux_session *s, uint32_t address, uint8_t *out, uint8_t len) {
    uint32_t i;
    uint8_t count;
    for (i = 0; i < 3u; ++i) {
        int rc = rdna4_aux_transaction(s, 0x90u, address, NULL, len, out, len, &count);
        if (!rc) return RDNA4_AUX_OK;
        if (rc != 2) return s->fault;
        if (i != 2u) s->io.delay(s->io.cookie, 500u);
    }
    return s->fault = RDNA4_AUX_DEFER;
}
/* Caller owns the software mutex; this also retains the hardware grant. */
static inline uint32_t rdna4_aux_dpcd_probe_checked(rdna4_aux_session *s, rdna4_dpcd_snapshot *out,
                                                int (*routeValid)(void *), void *route) {
    uint32_t i, pass, result, allConfig = 0xffu, allLanes = 0xffu;
    uint8_t config[2][9] = {{0}}, lanes[2][6] = {{0}};
    result = rdna4_aux_begin(s);
    if (result == RDNA4_AUX_OK && routeValid && !routeValid(route)) result = RDNA4_AUX_CHANGED;
    if (result == RDNA4_AUX_OK) for (pass = 0; pass < 2u; ++pass) {
        result = rdna4_aux_dpcd_block(s, 0x100u, config[pass], 9u);
        if (result != RDNA4_AUX_OK) break;
        result = rdna4_aux_dpcd_block(s, 0x202u, lanes[pass], 6u);
        if (result != RDNA4_AUX_OK) break;
    }
    if (result == RDNA4_AUX_OK) {
        for (i = 0; i < 9u; ++i) if (config[0][i] != config[1][i]) result = RDNA4_AUX_CHANGED;
        for (i = 0; i < 6u; ++i) if (lanes[0][i] != lanes[1][i]) result = RDNA4_AUX_CHANGED;
        for (i = 0; i < 9u; ++i) allConfig &= config[0][i];
        for (i = 0; i < 6u; ++i) allLanes &= lanes[0][i];
        if (allConfig == 0xffu || allLanes == 0xffu) result = RDNA4_AUX_INVALID;
        if (!rdna4_aux_valid(s)) result = s->fault;
        if (result == RDNA4_AUX_OK && routeValid && !routeValid(route)) result = RDNA4_AUX_CHANGED;
        if (result == RDNA4_AUX_OK && !rdna4_aux_valid(s)) result = s->fault;
    }
    s->fault = result;
    result = rdna4_aux_end(s);
    if (result == RDNA4_AUX_OK && !rdna4_aux_time(s)) result = s->fault;
    out->status = result;
    out->sampledUs = s->io.now(s->io.cookie);
    for (i = 0; i < 9u; ++i) out->config[i] = result == RDNA4_AUX_OK ? config[0][i] : 0u;
    for (i = 0; i < 6u; ++i) out->lanes[i] = result == RDNA4_AUX_OK ? lanes[0][i] : 0u;
    return result;
}
static inline uint32_t rdna4_aux_dpcd_probe(rdna4_aux_session *s, rdna4_dpcd_snapshot *out) {
    return rdna4_aux_dpcd_probe_checked(s, out, NULL, NULL);
}
#endif
