/* SPDX-License-Identifier: MIT
 * Real Darwin marshalling with a transport that fails after accepting work.
 */
#include "ac_darwin_kext.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

static unsigned checks, failures;
#define CHECK(c) do { ++checks; if (!(c)) { ++failures; \
   fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); } } while (0)

struct transport {
   unsigned calls, accepted, fail_at, bad_count_at;
   uint32_t error;
};

static int
call(void *ctx, uint32_t selector, const uint64_t *sin, uint32_t nsin,
     const void *input, size_t size, uint64_t *output, uint32_t *count,
     void *structure_out, size_t *structure_size)
{
   (void)sin; (void)structure_out; (void)structure_size;
   struct transport *t = ctx;
   if (selector == ACD_SEL_WAIT_SEQ) {
      CHECK(nsin == 3 && *count == 3);
      output[0] = 0;
      output[1] = output[2] = t->accepted;
      return 0;
   }
   CHECK(selector == ACD_SEL_SUBMIT && nsin == 0 && *count == 1);
   const struct rdna4n_cs_in *header = input;
   CHECK(size == sizeof(*header) + header->num_ibs * sizeof(struct rdna4n_cs_ib));
   CHECK(header->ctx_id == 1 && header->num_ibs <= RDNA4N_MAX_IBS);
   ++t->calls;
   if (t->calls == t->fail_at) return (int)t->error;
   output[0] = ++t->accepted;
   if (t->calls == t->bad_count_at) *count = 0;
   return 0;
}

int acd_kext_open_iokit(struct acd_kext **out) { (void)out; return -ENODEV; }
int acd_mock_open(struct acd_kext **out) { (void)out; return -ENODEV; }

static struct acd_kext *
create(struct transport *t)
{
   const struct acd_kext_ops ops = {.call = call};
   struct acd_kext *k = acd_kext_create(&ops, t);
   CHECK(k != NULL);
   return k;
}

int
main(void)
{
   struct rdna4n_cs_ib ibs[RDNA4N_MAX_IBS + 1] = {{0}};
   struct acd_cs_plan plan = {.ctx_id = 1, .ibs = ibs, .n_ibs = RDNA4N_MAX_IBS + 1};
   for (unsigned i = 0; i < plan.n_ibs; ++i) {
      ibs[i].va_start = 0x10000 + i * 4096;
      ibs[i].ib_bytes = 4;
   }
   uint64_t seq = 0;
   struct transport t = {0};
   struct acd_kext *k = create(&t);
   if (!k) return 1;
   CHECK(acd_kext_submit(k, &plan, &seq) == 0);
   CHECK(t.calls == 2 && t.accepted == 2 && seq == 2);
   acd_kext_destroy(k);

   t = (struct transport){.fail_at = 1, .error = ACD_KR_NO_MEMORY};
   k = create(&t);
   if (!k) return 1;
   CHECK(acd_kext_submit(k, &plan, &seq) == -ENOMEM);
   CHECK(t.accepted == 0);
   t.fail_at = 0;
   CHECK(acd_kext_submit(k, &plan, &seq) == 0);
   CHECK(t.calls == 3 && t.accepted == 2);
   acd_kext_destroy(k);

   const uint32_t errors[] = {ACD_KR_NO_MEMORY, ACD_KR_BAD_ARGUMENT, ACD_KR_TIMEOUT};
   for (unsigned i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i) {
      t = (struct transport){.fail_at = 2, .error = errors[i]};
      k = create(&t);
      if (!k) return 1;
      seq = 0;
      int result = acd_kext_submit(k, &plan, &seq);
      CHECK(result == -ECANCELED);
      CHECK(t.calls == 2 && t.accepted == 1 && seq == 1);
      t.fail_at = 0;
      CHECK(acd_kext_submit(k, &plan, &seq) == -ECANCELED);
      CHECK(t.calls == 2 && t.accepted == 1);
      bool busy = true;
      struct acd_wait_status status = {0};
      CHECK(acd_kext_wait_seq(k, 1, 0, &busy, &status) == 0);
      CHECK(!busy && status.retired == 1);
      printf("PARTIAL_SUBMIT error=%#x result=%d accepted=%u calls=%u\n",
             errors[i], result, t.accepted, t.calls);
      acd_kext_destroy(k);
   }

   t = (struct transport){.bad_count_at = 1};
   k = create(&t);
   if (!k) return 1;
   CHECK(acd_kext_submit(k, &plan, &seq) == -ECANCELED);
   CHECK(acd_kext_submit(k, &plan, &seq) == -ECANCELED);
   CHECK(t.calls == 1 && t.accepted == 1);
   acd_kext_destroy(k);
   printf("submit regression: %u checks, %u failed; simulated acceptance, no GPU\n", checks, failures);
   return failures ? 1 : 0;
}
