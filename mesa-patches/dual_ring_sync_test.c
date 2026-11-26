/*
 * SPDX-License-Identifier: MIT
 *
 * Host regression test for patch 0031 dual-ring synchronization, gang submit
 * seqno propagation (COMPUTE follower + GFX leader), and multi-point cross-ring
 * timeline waits in ac_darwin_sync.c + ac_darwin_kext.c.
 */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ac_darwin_drm.h"
#include "ac_darwin_kext.h"
#include "drm-uapi/drm.h"
#include "rdna4_native_abi.h"
#include "util/u_sync_provider.h"

static int g_checks = 0;
static int g_fails = 0;

#define CHECK(msg, cond)                                                      \
   do {                                                                       \
      g_checks++;                                                             \
      if (!(cond)) {                                                          \
         g_fails++;                                                           \
         fprintf(stderr, "FAIL [%s:%d] %s\n", __FILE__, __LINE__, (msg));     \
      }                                                                       \
   } while (0)

/* ---- fake sync backend ---- */

struct fake_sync_be {
   uint64_t retired[2]; /* 0 = GFX, 1 = Compute (raw seqnos) */
   uint32_t wait_calls[2];
   uint64_t last_waited[2];
};

static int
fake_wait_seq(void *ctx, uint64_t seq, uint64_t rel_ns, bool *busy, uint64_t *retired)
{
   (void)rel_ns;
   struct fake_sync_be *be = (struct fake_sync_be *)ctx;
   const uint32_t ring = (seq != RDNA4N_SEQ_LAST && (seq & RDNA4N_SEQ_RING_COMPUTE)) ? 1u : 0u;
   const uint64_t raw = seq & ~RDNA4N_SEQ_RING_COMPUTE;
   be->wait_calls[ring]++;
   be->last_waited[ring] = seq;
   *busy = raw > be->retired[ring];
   *retired = ring ? (be->retired[1] | RDNA4N_SEQ_RING_COMPUTE) : be->retired[0];
   return 0;
}

/* ---- fake kext transport for gang submit test ---- */

struct fake_kext_ctx {
   uint32_t ncalls;
   uint32_t ip_types[8];
   uint32_t has_fence[8];
   uint64_t next_gfx_seq;
   uint64_t next_comp_seq;
};

static int
fake_kcall(void *ctx, uint32_t sel, const uint64_t *sin, uint32_t nsin, const void *stin,
           size_t stinsz, uint64_t *sout, uint32_t *nsout, void *stout, size_t *stoutsz)
{
   (void)sin;
   (void)nsin;
   (void)stout;
   (void)stoutsz;
   struct fake_kext_ctx *f = (struct fake_kext_ctx *)ctx;
   if (sel != ACD_SEL_SUBMIT)
      return ACD_KR_BAD_ARGUMENT;
   if (stinsz < sizeof(struct rdna4n_cs_in) + sizeof(struct rdna4n_cs_ib))
      return ACD_KR_BAD_ARGUMENT;
   const struct rdna4n_cs_in *hdr = (const struct rdna4n_cs_in *)stin;
   const struct rdna4n_cs_ib *ibs = (const struct rdna4n_cs_ib *)((const uint8_t *)stin + sizeof(*hdr));
   if (f->ncalls < 8) {
      f->ip_types[f->ncalls] = ibs[0].ip_type;
      f->has_fence[f->ncalls] = (hdr->flags & RDNA4N_CS_HAS_FENCE) ? 1u : 0u;
   }
   f->ncalls++;
   if (ibs[0].ip_type == RDNA4N_HW_IP_COMPUTE) {
      sout[0] = (++f->next_comp_seq) | RDNA4N_SEQ_RING_COMPUTE;
   } else {
      sout[0] = ++f->next_gfx_seq;
   }
   *nsout = 1;
   return ACD_KR_SUCCESS;
}

static void
fake_close(void *ctx)
{
   (void)ctx;
}

int acd_kext_open_iokit(struct acd_kext **out) { (void)out; return -ENODEV; }
int acd_mock_open(struct acd_kext **out) { (void)out; return -ENODEV; }

static void
test_same_and_cross_ring_sync(void)
{
   struct fake_sync_be be = {.retired = {0, 0}};
   struct acd_sync_backend sbe = {.wait_seq = fake_wait_seq, .ctx = &be};
   struct util_sync_provider *p = acd_sync_provider_create(&sbe);
   CHECK("create sync provider", p != NULL);

   uint32_t h_gfx = 0, h_comp = 0;
   CHECK("create h_gfx", p->create(p, 0, &h_gfx) == 0);
   CHECK("create h_comp", p->create(p, 0, &h_comp) == 0);

   CHECK("attach gfx seq 5", acd_sync_cs_attach(p, h_gfx, 0, 5ull) == 0);
   CHECK("attach comp seq 3", acd_sync_cs_attach(p, h_comp, 0, 3ull | RDNA4N_SEQ_RING_COMPUTE) == 0);

   /* Same-ring waits must be lock-free (zero wait_seq backend calls). */
   CHECK("GFX->GFX lock-free", acd_sync_cs_wait_ready(p, 1u << 0, h_gfx, 0, 1000000ull) == 0);
   CHECK("Compute->Compute lock-free", acd_sync_cs_wait_ready(p, 1u << 1, h_comp, 0, 1000000ull) == 0);
   CHECK("no backend wait on same-ring deps", be.wait_calls[0] == 0 && be.wait_calls[1] == 0);

   /* Cross-ring waits must block until producer ring retires. */
   CHECK("GFX->Compute times out while GFX seq 5 busy",
         acd_sync_cs_wait_ready(p, 1u << 1, h_gfx, 0, 0) == -ETIME);
   CHECK("GFX->Compute polled GFX ring", be.wait_calls[0] >= 1 && be.wait_calls[1] == 0);

   be.retired[0] = 5;
   CHECK("GFX->Compute succeeds once GFX seq 5 retires",
         acd_sync_cs_wait_ready(p, 1u << 1, h_gfx, 0, 1000000ull) == 0);

   CHECK("Compute->GFX times out while Compute seq 3 busy",
         acd_sync_cs_wait_ready(p, 1u << 0, h_comp, 0, 0) == -ETIME);
   be.retired[1] = 3;
   CHECK("Compute->GFX succeeds once Compute seq 3 retires",
         acd_sync_cs_wait_ready(p, 1u << 0, h_comp, 0, 1000000ull) == 0);

   p->finalize(p);
}

static void
test_gang_submit_tracks_both_rings(void)
{
   /* 1. Verify acd_kext_submit_ex partitions mixed COMPUTE+GFX gang and returns BOTH seqnos. */
   struct fake_kext_ctx f = {.next_gfx_seq = 10, .next_comp_seq = 20};
   struct acd_kext_ops ops = {.call = fake_kcall, .close = fake_close};
   struct acd_kext *k = acd_kext_create(&ops, &f);
   CHECK("create fake kext", k != NULL);

   struct rdna4n_cs_ib ibs[2];
   memset(ibs, 0, sizeof(ibs));
   ibs[0].ip_type = AMDGPU_HW_IP_GFX;
   ibs[0].va_start = 0x100000;
   ibs[0].ib_bytes = 64;
   ibs[1].ip_type = AMDGPU_HW_IP_COMPUTE;
   ibs[1].va_start = 0x200000;
   ibs[1].ib_bytes = 64;

   struct acd_cs_plan plan;
   memset(&plan, 0, sizeof(plan));
   plan.ctx_id = 1;
   plan.ibs = ibs;
   plan.n_ibs = 2;
   plan.has_fence = true;
   plan.fence_handle = 1;
   plan.fence_offset = 0;

   uint64_t gfx_seq = 0, comp_seq = 0;
   CHECK("gang submit succeeds", acd_kext_submit_ex(k, &plan, &gfx_seq, &comp_seq) == 0);
   CHECK("gang submit issued 2 kcalls (KCQ first, GFX second)",
         f.ncalls == 2 && f.ip_types[0] == RDNA4N_HW_IP_COMPUTE && f.ip_types[1] == RDNA4N_HW_IP_GFX);
   CHECK("fence attached only to GFX leader", f.has_fence[0] == 0 && f.has_fence[1] == 1);
   CHECK("gang submit returned gfx_seq=11 and tagged comp_seq=21",
         gfx_seq == 11ull && comp_seq == (21ull | RDNA4N_SEQ_RING_COMPUTE));
   acd_kext_destroy(k);

   /* 2. Verify syncobj attached to gang pair does NOT complete until BOTH rings retire. */
   struct fake_sync_be be = {.retired = {0, 0}};
   struct acd_sync_backend sbe = {.wait_seq = fake_wait_seq, .ctx = &be};
   struct util_sync_provider *p = acd_sync_provider_create(&sbe);
   uint32_t h_bin = 0, h_tl = 0;
   CHECK("create gang syncobjs", p->create(p, 0, &h_bin) == 0 && p->create(p, 0, &h_tl) == 0);
   CHECK("attach gang pair to binary", acd_sync_cs_attach_pair(p, h_bin, 0, gfx_seq, comp_seq) == 0);
   CHECK("attach gang pair to timeline point 7", acd_sync_cs_attach_pair(p, h_tl, 7, gfx_seq, comp_seq) == 0);

   /* Only GFX retires -> binary and timeline point 7 must STILL be busy! */
   be.retired[0] = 11;
   uint64_t qpt = 0;
   CHECK("query gang timeline while ACE follower still busy stays 0",
         p->query(p, &h_tl, &qpt, 1, 0) == 0 && qpt == 0);
   CHECK("binary wait times out while ACE follower still busy",
         p->wait(p, &h_bin, 1, 0, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL, NULL) == -1 && errno == ETIME);
   /* Subsequent GFX submit waiting on gang binary must still wait for ACE follower! */
   CHECK("GFX submit waits on ACE follower of prior gang",
         acd_sync_cs_wait_ready(p, 1u << 0, h_bin, 0, 0) == -ETIME);

   /* Now ACE follower retires too -> BOTH binary and timeline point 7 complete! */
   be.retired[1] = 21;
   CHECK("query gang timeline advances to 7 once both rings retire",
         p->query(p, &h_tl, &qpt, 1, 0) == 0 && qpt == 7);
   CHECK("binary wait succeeds once both rings retire",
         p->wait(p, &h_bin, 1, 0, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL, NULL) == 0);
   CHECK("GFX submit proceeds once ACE follower retires",
         acd_sync_cs_wait_ready(p, 1u << 0, h_bin, 0, 0) == 0);

   p->finalize(p);
}

static void
test_multipoint_cross_ring_timeline(void)
{
   struct fake_sync_be be = {.retired = {0, 0}};
   struct acd_sync_backend sbe = {.wait_seq = fake_wait_seq, .ctx = &be};
   struct util_sync_provider *p = acd_sync_provider_create(&sbe);
   uint32_t h_tl = 0;
   CHECK("create timeline handle", p->create(p, 0, &h_tl) == 0);

   /* Point 1 signaled on GFX (seq 100); Point 2 signaled on Compute (seq 5). */
   CHECK("attach point 1 on GFX", acd_sync_cs_attach(p, h_tl, 1, 100ull) == 0);
   CHECK("attach point 2 on Compute", acd_sync_cs_attach(p, h_tl, 2, 5ull | RDNA4N_SEQ_RING_COMPUTE) == 0);

   /* A Compute submit waiting on point 1 is already ordered behind Compute point 2 -> FIFO fast path! */
   CHECK("Compute submit waiting on point 1 takes FIFO fast path via Compute point 2",
         acd_sync_cs_wait_ready(p, 1u << 1, h_tl, 1, 0) == 0);
   CHECK("FIFO fast path issued 0 backend waits", be.wait_calls[0] == 0 && be.wait_calls[1] == 0);

   /* A CPU timeline_wait on point 1 MUST poll BOTH rings and wake when Compute point 2 finishes even if GFX 100 is still busy! */
   be.retired[1] = 5; /* Compute point 2 done; GFX point 1 (seq 100) still busy */
   uint64_t pt = 1;
   CHECK("timeline_wait(point=1) wakes immediately when Compute point 2 retires",
         p->timeline_wait(p, &h_tl, &pt, 1, 0, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL, NULL) == 0);
   CHECK("timeline_wait polled Compute ring", be.wait_calls[1] >= 1);

   uint64_t qpt = 0;
   CHECK("timeline value is now 2", p->query(p, &h_tl, &qpt, 1, 0) == 0 && qpt == 2);

   /* Gang submit waiting on point 1 is now immediately P_DONE. */
   CHECK("gang submit sees P_DONE", acd_sync_cs_wait_ready(p, 0x3u, h_tl, 1, 0) == 0);

   p->finalize(p);
}

int
main(void)
{
   test_same_and_cross_ring_sync();
   test_gang_submit_tracks_both_rings();
   test_multipoint_cross_ring_timeline();
   printf("dual_ring_sync_test: %d checks, %d failed\n", g_checks, g_fails);
   return g_fails ? 1 : 0;
}
