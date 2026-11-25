// garnet_occ.h: bundle build 18, app-fix round 2, P2 (an internal design note section P2, "Maps crash: setVisibilityResultMode:offset:"). Pure C (host test: test-occ.c).
//
// THE MODEL (what the Vulkan side does with it is in GarnetDevice.m, RDNA4RenderEncoder; this file is only the bookkeeping and the arithmetic, so a host test can drive it).
//   * -setVisibilityResultMode:offset: only RECORDS the mode and offset (after the checks in rdna4occ_set).
//   * the first draw after a (mode, offset) is set BEGINS a Vulkan occlusion query (rdna4occ_draw); the query is ENDED when the mode or offset changes and at every place the Vulkan render pass ends
//     (rdna4occ_end, called through the one rdna4EndPass helper), because a query must end in the render pass it began in. Each begin/end is an INTERVAL of one offset.
//   * after the GPU has finished, the per-interval sample counts are folded per offset (rdna4occ_final) and written into the visibility buffer, BEFORE commandBufferDidComplete.
//
// THE METAL SEMANTICS (MEASURED on the host Mac's real Apple Metal, Apple M4, macOS 27.0.x; program: test-occlusion-semantics.m, output in INSTALL.md build 18):
//   S1  Counting: the value is the SUM of the passing samples of every draw made while that offset was selected - across draws, and across separate intervals of the same offset in one pass
//       (offset 0 selected, drawn twice, Disabled, selected again and drawn: 3 x 4096 = 12288). A Disabled draw adds nothing.
//   S2  Boolean: the value written is exactly 1 if any sample passed, else 0 (two draws, or a hidden draw then a visible one: 1).
//   S3  An offset that was selected (Boolean or Counting) but saw no draw is WRITTEN with 0. An offset never selected in the pass is left untouched. A pass that never calls
//       setVisibilityResultMode touches nothing. (Selecting Disabled@N does not write N; one run showed Disabled@0 after a Boolean@8 writing 0 at offset 0: UNEXPLAINED, not copied.)
//   S4  visibilityResultType Reset (the default): the offset is OVERWRITTEN with this pass's value. Only the offsets used are written; the rest of the buffer is not cleared.
//   S5  Accumulate: Counting adds the pass's sum to what the buffer held (preset 0xAB..AB + 4096 gave ...+4096; two passes in one command buffer: +8192); Boolean ORs (preset 4, visible: 5;
//       preset 5, visible: 5; preset 5, hidden: 5).
//   S6  The value is already in the buffer when the completed handler runs.
//   S7  Boolean and Counting on the SAME offset in one pass gave a meaningless value (2165) - undefined; here Counting wins (the sum is written).
//
// THE FAIL-SAFE. Zero means "hidden", and a hidden answer makes the client CULL geometry. So every uncertain case writes "visible": an exhausted query slot table (more intervals than RDNA4OCC_MAXQ or more
// distinct offsets than RDNA4OCC_MAXU in one pass), a pool that could not be created, a query that is not available, a failed vkGetQueryPoolResults, a lost device, a command buffer that never ran. In those
// cases every offset the pass selected gets 1 added to its sample sum (Boolean -> 1, Counting -> sum+1, Accumulate as above).
#ifndef RDNA4_OCC_H
#define RDNA4_OCC_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>

enum { RDNA4OCC_DISABLED = 0, RDNA4OCC_BOOLEAN = 1, RDNA4OCC_COUNTING = 2 };
#define RDNA4OCC_MAXQ 256   // query intervals per render pass = the chunk of query indices reserved at encoder creation
#define RDNA4OCC_MAXU 256   // distinct offsets per render pass
#define RDNA4OCC_MAX_BYTES (UINT64_C(256) * 1024u)   // Mac2 visibility query offset table limit (256 KiB)

typedef struct { uint64_t off; uint8_t b, c; } rdna4occ_u;   // a used offset: Boolean / Counting selected for it at least once
typedef struct {
    uint64_t buflen;            // length of the visibility result buffer
    int accumulate;             // visibilityResultType == Accumulate
    int mode; uint64_t off; int ui;   // the current setting; ui = index into u[] (-1 = none / not trackable)
    int active; uint32_t activeQ;     // a Vulkan query is open (and which)
    int dead;                         // no query pool: no query is ever begun
    uint32_t nq, nu; int overflow;    // intervals begun, offsets used; overflow = fail-safe for the whole pass
    uint32_t rejected;
    uint16_t ivu[RDNA4OCC_MAXQ];  // per interval (local query index): the used-offset it belongs to
    rdna4occ_u u[RDNA4OCC_MAXU];
} rdna4occ;

static inline void rdna4occ_init(rdna4occ *o, uint64_t buflen, int accumulate) {
    memset(o, 0, sizeof *o); o->buflen = buflen; o->accumulate = accumulate ? 1 : 0; o->ui = -1;
}
// The pass could not get a query pool: every offset used is answered "visible".
static inline void rdna4occ_dead(rdna4occ *o) { o->overflow = 1; o->dead = 1; }

// setVisibilityResultMode:offset:. Returns 0 = recorded, -1 = rejected (the mode and offset are then Disabled). *endActive = 1: the caller must vkCmdEndQuery the query that was open NOW (it is o->activeQ; the interval is already closed in the bookkeeping).
static inline int rdna4occ_set(rdna4occ *o, unsigned long mode, uint64_t off, int *endActive) {
    *endActive = 0;
    int bad = mode > RDNA4OCC_COUNTING || (mode != RDNA4OCC_DISABLED && ((off & 7u) != 0 || off > o->buflen || o->buflen - off < 8 || off >= RDNA4OCC_MAX_BYTES || RDNA4OCC_MAX_BYTES - off < 8));
    if (bad) { o->rejected++; mode = RDNA4OCC_DISABLED; }
    if ((int)mode == o->mode && (mode == RDNA4OCC_DISABLED || off == o->off)) return bad ? -1 : 0;   // nothing changed: the open interval continues
    if (o->active) { o->active = 0; *endActive = 1; }
    o->mode = (int)mode; o->off = (mode == RDNA4OCC_DISABLED) ? 0 : off; o->ui = -1;
    if (mode != RDNA4OCC_DISABLED) {
        uint32_t i = 0;
        for (; i < o->nu; i++) if (o->u[i].off == off) break;
        if (i == o->nu) {
            if (o->nu >= RDNA4OCC_MAXU) { o->overflow = 1; return bad ? -1 : 0; }
            o->u[i].off = off; o->u[i].b = o->u[i].c = 0; o->nu++;
        }
        if (mode == RDNA4OCC_BOOLEAN) o->u[i].b = 1; else o->u[i].c = 1;
        o->ui = (int)i;
    }
    return bad ? -1 : 0;
}
// A draw is about to be recorded (inside the render pass). Returns 1 if the caller must vkCmdBeginQuery(*q, precise ? PRECISE : 0) first.
static inline int rdna4occ_draw(rdna4occ *o, uint32_t *q, int *precise) {
    if (o->dead || o->mode == RDNA4OCC_DISABLED || o->ui < 0 || o->active) return 0;
    if (o->nq >= RDNA4OCC_MAXQ) { o->overflow = 1; return 0; }
    *q = o->nq++; o->ivu[*q] = (uint16_t)o->ui; o->active = 1; o->activeQ = *q;
    *precise = o->mode == RDNA4OCC_COUNTING;   // occlusionQueryPrecise is needed only to COUNT; a Boolean answer takes the cheap query
    return 1;
}
// The render pass ends (or the mode changes): returns 1 if a query is open; the caller must vkCmdEndQuery(*q) BEFORE vkCmdEndRenderPass.
static inline int rdna4occ_end(rdna4occ *o, uint32_t *q) {
    if (!o->active) return 0;
    o->active = 0; *q = o->activeQ; return 1;
}
// Folds the raw output of vkGetQueryPoolResults(64-bit | WITH_AVAILABILITY, stride 16) for the o->nq intervals into res[]: pairs (result, availability). 1 = every interval was available and the results are
// trustworthy; 0 = at least one interval was NOT available (a query that never ran, a reset that was not executed): the caller must answer "visible".
static inline int rdna4occ_read(const rdna4occ *o, const uint64_t *raw, uint64_t *res) {
    for (uint32_t q = 0; q < o->nq; q++) { if (!raw[2 * q + 1]) return 0; res[q] = raw[2 * q]; }
    return 1;
}
// The value to write for used offset u. res[q] = samples of interval q (res may be NULL when !ok); ok = the results are trustworthy. old = what the buffer holds now.
static inline uint64_t rdna4occ_final(const rdna4occ *o, unsigned u, const uint64_t *res, int ok, uint64_t old) {
    uint64_t sum = 0;
    if (!ok || !res) sum = 1;
    else for (uint32_t q = 0; q < o->nq; q++) if (o->ivu[q] == u) sum += res[q];
    if (o->overflow) sum += 1;   // fail-safe: some draws were not measured, assume they were visible
    if (o->u[u].c) return o->accumulate ? old + sum : sum;                       // S1, S4, S5, S7
    { uint64_t v = sum != 0 ? 1u : 0u; return o->accumulate ? (old | v) : v; }   // S2, S4, S5
}
#endif
