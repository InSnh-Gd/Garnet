#include "ac_darwin_kext.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

struct transport {
   unsigned calls, queries, buffer_calls;
   uint32_t sel;
   int error;
   unsigned mode;
   struct rdna4n_buffer_op request;
};

static int
call(void *ctx, uint32_t sel, const uint64_t *sin, uint32_t nsin,
     const void *input, size_t size, uint64_t *output, uint32_t *count,
     void *stout, size_t *stoutsz)
{
   struct transport *t = ctx;
   (void)stout; (void)stoutsz;
   ++t->calls;
   t->sel = sel;
   if (t->error) return t->error;
   if (sel == ACD_SEL_QUERY_INFO) {
      ++t->queries;
      assert(*stoutsz == sizeof(struct rdna4n_info));
      memset(stout, 0, *stoutsz);
      ((struct rdna4n_info *)stout)->flags = t->mode == 5 ? 0 : RDNA4N_INFO_BUFFER_OPS;
      if (t->mode == 6) *stoutsz = 0;
      return 0;
   }
   if (sel == ACD_SEL_WAIT_SEQ) {
      assert(nsin == 3 && sin[0] == 19 && sin[1] <= RDNA4N_WAIT_CAP_NS);
      output[0] = t->mode == 1;
      output[1] = t->mode == 1 || t->mode == 4 ? 18 : 19;
      output[2] = 19;
      *count = 3;
   } else {
      ++t->buffer_calls;
      assert((sel == RDNA4N_SEL_BUFFER_COPY || sel == RDNA4N_SEL_BUFFER_FILL) && nsin == 0);
      assert(size == 48 && *count == 1 && sin == NULL);
      memcpy(&t->request, input, size);
      if (t->mode != 2) output[0] = 19;
      if (t->mode == 3) *count = 0;
   }
   return 0;
}
int acd_kext_open_iokit(struct acd_kext **out) { (void)out; return -ENODEV; }
int acd_mock_open(struct acd_kext **out) { (void)out; return -ENODEV; }

int main(void)
{
   struct transport t = {0};
   const struct acd_kext_ops ops = {.call = call};
   struct acd_kext *k = acd_kext_create(&ops, &t);
   assert(k);
   struct rdna4n_buffer_op q = {.src_handle = 7, .dst_handle = 8, .src_offset = 12,
                              .dst_offset = 24, .length = 32};
   uint64_t seq = 77;
   assert(acd_kext_buffer_op(k, &q, false, &seq) == 0 && seq == 19);
   assert(t.sel == RDNA4N_SEL_BUFFER_COPY && memcmp(&t.request, &q, 48) == 0);
   q.src_handle = 0; q.src_offset = 0; q.pattern = 0xaabbccdd;
   assert(acd_kext_buffer_op(k, &q, true, &seq) == 0 && seq == 19);
   assert(t.sel == RDNA4N_SEL_BUFFER_FILL && t.request.pattern == 0xaabbccdd);
   assert(t.queries == 1 && t.buffer_calls == 2);
   unsigned calls = t.calls;
   assert(acd_kext_buffer_op(NULL, &q, true, &seq) == -EINVAL && seq == 0);
   assert(acd_kext_buffer_op(k, NULL, true, &seq) == -EINVAL && seq == 0);
   assert(acd_kext_buffer_op(k, &q, true, NULL) == -EINVAL);
   bool invalid_busy = false;
   assert(acd_kext_buffer_wait(k, 0, 0, &invalid_busy) == -EINVAL && invalid_busy);
   assert(acd_kext_buffer_wait(k, 19, 0, NULL) == -EINVAL);
   for (unsigned i = 0; i < 9; ++i) {
      struct rdna4n_buffer_op bad = q;
      switch (i) {
      case 0: bad.length = 0; break;
      case 1: bad.length = RDNA4N_BUFFER_MAX_BYTES + 4; break;
      case 2: bad.dst_offset = UINT64_MAX - 3; break;
      case 3: bad.dst_handle = 0; break;
      case 4: bad.flags = 1; break;
      case 5: bad.reserved = 1; break;
      case 6: bad.src_handle = 7; break;
      case 7: bad.src_offset = 4; break;
      case 8: bad.length = 5; break;
      }
      seq = 77;
      assert(acd_kext_buffer_op(k, &bad, true, &seq) == -EINVAL && seq == 0);
   }
   assert(t.calls == calls);
   q = (struct rdna4n_buffer_op){.src_handle = 8, .dst_handle = 8, .length = 16, .dst_offset = 4};
   assert(acd_kext_buffer_op(k, &q, false, &seq) == -EINVAL && t.calls == calls);
   q.dst_offset = 16;
   assert(acd_kext_buffer_op(k, &q, false, &seq) == 0);
   q.pattern = 1;
   calls = t.calls;
   assert(acd_kext_buffer_op(k, &q, false, &seq) == -EINVAL && t.calls == calls);
   q.pattern = 0;
   struct rdna4n_buffer_op max_op = {.dst_handle = 8, .length = RDNA4N_BUFFER_MAX_BYTES};
   assert(acd_kext_buffer_op(k, &max_op, true, &seq) == 0);
   const uint32_t errors[] = {ACD_KR_UNSUPPORTED, ACD_KR_NOT_PRIVILEGED, ACD_KR_NOT_PERMITTED,
                              ACD_KR_BAD_ARGUMENT, ACD_KR_TIMEOUT, ACD_KR_ABORTED, 0x10000003};
   const int expected[] = {-ENOSYS, -ENOSYS, -EACCES, -EINVAL, -ETIME, -ECANCELED, -ENODEV};
   for (unsigned i = 0; i < sizeof(errors)/sizeof(errors[0]); ++i) {
      t.error = errors[i];
      assert(acd_kext_buffer_op(k, &q, false, &seq) == expected[i] && seq == 0);
   }
   t.error = 0;
   bool busy = true;
   t.mode = 1;
   assert(acd_kext_buffer_wait(k, 19, UINT64_MAX, &busy) == 0 && busy);
   t.mode = 0;
   assert(acd_kext_buffer_wait(k, 19, 0, &busy) == 0 && !busy);
   t.error = ACD_KR_ABORTED;
   assert(acd_kext_buffer_wait(k, 19, 0, &busy) == -ECANCELED && busy);
   t.error = 0;
   acd_kext_destroy(k);
   for (unsigned mode = 2; mode <= 3; ++mode) {
      k = acd_kext_create(&ops, &t);
      t.mode = mode;
      assert(acd_kext_buffer_op(k, &q, false, &seq) == -ECANCELED && seq == 0);
      calls = t.calls;
      t.mode = 0;
      assert(acd_kext_buffer_op(k, &q, false, &seq) == -ECANCELED && t.calls == calls);
      assert(acd_kext_buffer_wait(k, 19, 0, &busy) == -ECANCELED && busy);
      acd_kext_destroy(k);
   }
   k = acd_kext_create(&ops, &t);
   t.mode = 4;
   assert(acd_kext_buffer_wait(k, 19, 0, &busy) == -ECANCELED && busy);
   calls = t.calls;
   assert(acd_kext_buffer_op(k, &q, false, &seq) == -ECANCELED && t.calls == calls);
   acd_kext_destroy(k);
   t = (struct transport){.mode = 5};
   k = acd_kext_create(&ops, &t);
   assert(acd_kext_buffer_op(k, &q, false, &seq) == -ENOSYS && seq == 0);
   assert(acd_kext_buffer_op(k, &q, false, &seq) == -ENOSYS && seq == 0);
   assert(t.queries == 1 && t.buffer_calls == 0);
   acd_kext_destroy(k);
   t = (struct transport){.mode = 6};
   k = acd_kext_create(&ops, &t);
   assert(acd_kext_buffer_op(k, &q, false, &seq) == -EIO && seq == 0);
   assert(t.queries == 1 && t.buffer_calls == 0);
   acd_kext_destroy(k);
   puts("buffer bridge: wire copy/fill, invalid-no-call, cached capability/old-kernel fallback, errno, busy wait, malformed/sticky loss PASS (mock transport, no GPU)");
   return 0;
}
