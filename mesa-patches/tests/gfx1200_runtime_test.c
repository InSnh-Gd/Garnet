/* SPDX-License-Identifier: MIT
 * Exercises the patched Mesa backend; the fake transport never opens IOKit.
 */
#include "ac_darwin_drm.h"
#include "ac_darwin_kext.h"
#include "util/u_sync_provider.h"
#include "amdgpu_asic_addr.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MiB (1ull << 20)
#define GiB (1ull << 30)
static unsigned failures, checks, opens, reads;
static unsigned accepted_submits, failed_sync_allocations;
static bool fail_sync_allocation;
static uint32_t wait_result;
static unsigned wait_calls;
static bool wait_busy;
static uint64_t wait_retired;
static struct rdna4n_info live;
#define CHECK(c) do { ++checks; if (!(c)) { ++failures; \
   fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); } } while (0)

void *
acd_test_sync_realloc(void *pointer, size_t size)
{
   if (fail_sync_allocation) {
      ++failed_sync_allocations;
      return NULL;
   }
   return realloc(pointer, size);
}

static int
fake_call(void *ctx, uint32_t sel, const uint64_t *sin, uint32_t nsin,
          const void *stin, size_t stinsz, uint64_t *sout, uint32_t *nsout,
          void *stout, size_t *stoutsz)
{
   (void)ctx; (void)sin; (void)nsin; (void)stin; (void)stinsz;
   if (sel == ACD_SEL_HELLO) {
      sout[0] = RDNA4N_ABI_VERSION | ((uint64_t)RDNA4N_ABI_MINOR << 16);
      sout[1] = 1; sout[2] = RDNA4N_MAX_IBS; sout[3] = 8;
      *nsout = 4;
   } else if (sel == ACD_SEL_QUERY_INFO) {
      CHECK(*stoutsz == sizeof(live));
      memcpy(stout, &live, sizeof(live));
      *stoutsz = sizeof(live);
   } else if (sel == ACD_SEL_READ_REGS) {
      ++reads;
      *(uint32_t *)stout = ACD_GB_ADDR_CONFIG_GFX1201;
      *stoutsz = sizeof(uint32_t);
   } else if (sel == ACD_SEL_SUBMIT) {
      CHECK(nsin == 0 && *nsout == 1);
      CHECK(stinsz >= sizeof(struct rdna4n_cs_in) + sizeof(struct rdna4n_cs_ib));
      sout[0] = ++accepted_submits;
   } else if (sel == ACD_SEL_WAIT_SEQ) {
      ++wait_calls;
      CHECK(nsin == 3 && *nsout == 3);
      if (wait_result)
         return wait_result;
      sout[0] = wait_busy;
      sout[1] = wait_retired;
      sout[2] = sin[0];
   } else {
      return ACD_KR_UNSUPPORTED;
   }
   return 0;
}

int
acd_kext_open_iokit(struct acd_kext **out)
{
   static const struct acd_kext_ops ops = {.call = fake_call};
   ++opens;
   *out = acd_kext_create(&ops, NULL);
   return *out ? 0 : -ENOMEM;
}

static void
reset(uint32_t gc, uint64_t bytes)
{
   unsetenv("RADV_DARWIN_CHIP");
   unsetenv("RADV_DARWIN_HOST_BO");
   unsetenv("RADV_DARWIN_MOCK");
   unsetenv("RADV_DARWIN_MOCK_GC");
   unsetenv("RADV_DARWIN_ALLOW_ADDRCFG");
   live = (struct rdna4n_info) {
      .abi_version = RDNA4N_ABI_VERSION, .gc_version = gc,
      .gb_addr_config = ACD_GB_ADDR_CONFIG_GFX1201,
      .vram_vis_total = 248 * MiB, .vram_vis_free = 200 * MiB,
      .vram_hi_total = bytes - 256 * MiB, .vram_hi_free = bytes - 512 * MiB,
      .gtt_cap = 512 * MiB, .gtt_used = 32 * MiB, .gtt_max_bo = 64 * MiB,
      .va_low_first = 0x10000, .va_low_last = 0x7fffffffffffull,
      .va_high_first = 0xffff800000000000ull, .va_high_last = UINT64_MAX,
   };
}

static acd_device_handle
open_device(void)
{
   uint32_t major = 0, minor = 0;
   acd_device_handle dev = NULL;
   int r = acd_device_initialize(&major, &minor, &dev);
   CHECK(r == 0);
   CHECK(major == 3 && minor >= 15);
   return dev;
}

static void
close_device(acd_device_handle dev)
{
   struct util_sync_provider *sync = acd_device_get_sync_provider(dev);
   sync->finalize(sync); /* ac_drm_device_deinitialize normally releases the clone. */
   acd_device_deinitialize(dev);
}

static void
query(acd_device_handle dev, uint32_t type, uint32_t ip, void *out, size_t size)
{
   struct drm_amdgpu_info q = {.query = type, .return_pointer = (uintptr_t)out,
                               .return_size = size};
   q.query_hw_ip.type = ip;
   CHECK(acd_ioctl(dev, DRM_AMDGPU_INFO, &q, sizeof(q)) == 0);
}

static void
identity(acd_device_handle dev, bool navi44, bool native)
{
   struct drm_amdgpu_info_device d = {0};
   struct drm_amdgpu_info_hw_ip gfx = {0}, compute = {0};
   query(dev, AMDGPU_INFO_DEV_INFO, 0, &d, sizeof(d));
   query(dev, AMDGPU_INFO_HW_IP_INFO, AMDGPU_HW_IP_GFX, &gfx, sizeof(gfx));
   query(dev, AMDGPU_INFO_HW_IP_INFO, AMDGPU_HW_IP_COMPUTE, &compute, sizeof(compute));
   CHECK(d.device_id == (navi44 ? 0x7590u : 0x7550u));
   CHECK(d.family == AMDGPU_FAMILY_GC_12_0_0);
   CHECK(navi44 ? ASICREV_IS_GFX1200(d.external_rev) : ASICREV_IS(d.external_rev, GFX1201));
   CHECK(gfx.ip_discovery_version == (navi44 ? 0x0c0000u : 0x0c0001u));
   CHECK(gfx.available_rings == 1);
   /* The kernel cs_parse rejects every COMPUTE IB. A general GFX queue still supports compute. */
   CHECK(compute.available_rings == (native ? 0u : 0xfu));
   CHECK(strcmp(acd_get_marketing_name(dev), navi44 ? "AMD Radeon RX 9060 XT" : "AMD Radeon RX 9070 XT") == 0);
   if (navi44) {
      /* Target RX 9060 XT DRM_INFO capture, 2026-11-02. */
      CHECK(d.max_engine_clock == UINT64_C(2780000));
      CHECK(d.ids_flags == UINT64_C(0x18));
      CHECK(d.high_va_max == UINT64_C(0xffffffffffbf0000));
      unsigned cu = 0;
      for (unsigned se = 0; se < 4; ++se)
         for (unsigned sa = 0; sa < 4; ++sa) cu += __builtin_popcount(d.cu_bitmap[se][sa]);
      CHECK(cu == d.cu_active_number);
      CHECK(d.num_shader_engines * d.num_shader_arrays_per_engine * d.num_cu_per_sh == cu);
      CHECK((unsigned)__builtin_popcount(d.enabled_rb_pipes_mask) == d.num_rb_pipes);
      /* Consistency checks of synthetic data, not measurements of a physical GPU. */
   }
}

static void
selection(void)
{
   const uint32_t versions[] = {0x0c0000, 0x0c0001, 0};
   for (unsigned i = 0; i < 3; ++i) {
      reset(versions[i], 16 * GiB);
      unsigned before = reads;
      acd_device_handle dev = open_device();
      if (!dev) continue;
      identity(dev, i == 0, true);
      CHECK(reads == before); /* QueryInfo's GB value avoids root-only ReadRegs. */
      unsigned connected = opens;
      acd_device_handle second = open_device();
      CHECK(opens == connected);
      if (second) { identity(second, i == 0, true); close_device(second); }
      close_device(dev);
   }
   for (unsigned i = 0; i < 2; ++i) {
      reset(i ? 0x0c0000 : 0x0c0001, 8 * GiB);
      setenv("RADV_DARWIN_CHIP", i ? "gfx1201" : "gfx1200", 1);
      acd_device_handle dev = open_device();
      if (dev) { identity(dev, !i, true); close_device(dev); }
   }
   reset(0x0c0002, 8 * GiB);
   uint32_t major, minor;
   acd_device_handle dev = NULL;
   CHECK(acd_device_initialize(&major, &minor, &dev) == -ENODEV);
   CHECK(dev == NULL);
   reset(0x0c0000, 8 * GiB);
   setenv("RADV_DARWIN_CHIP", "invalid", 1);
   CHECK(acd_device_initialize(&major, &minor, &dev) == -ENODEV);
   CHECK(dev == NULL);
   reset(0x0c0000, 8 * GiB);
   live.gb_addr_config = 0;
   unsigned before = reads;
   dev = open_device();
   CHECK(reads == before + 1);
   if (dev) close_device(dev);
   reset(0x0c0000, 8 * GiB);
   live.gb_addr_config ^= 1;
   CHECK(acd_device_initialize(&major, &minor, &dev) == -ENODEV);
   puts("checked GC selection, overrides, legacy/unknown GC, shared core, GB checks");
}

static void
memory(void)
{
   for (unsigned gib = 8; gib <= 16; gib += 8) {
      for (unsigned budget = 0; budget < 2; ++budget) {
         reset(0x0c0000, gib * GiB);
         const uint64_t total = live.vram_vis_total + live.vram_hi_total;
         if (budget) {
            live.reserved[RDNA4N_INFO_R_BUDGET_FLAGS] = RDNA4N_BUDGET_VALID | RDNA4N_BUDGET_APP;
            live.reserved[RDNA4N_INFO_R_BUDGET_CAP_LO] = 2 * GiB;
            live.reserved[RDNA4N_INFO_R_BUDGET_AVAIL_LO] = 3 * GiB / 2;
         }
         acd_device_handle dev = open_device();
         if (!dev) continue;
         struct drm_amdgpu_memory_info m = {0};
         query(dev, AMDGPU_INFO_MEMORY, 0, &m, sizeof(m));
         /* Physical pools stay unshrunk; the budget only bounds max_allocation headroom. */
         const uint64_t free = live.vram_vis_free + live.vram_hi_free;
         const uint64_t used = total - free;
         const uint64_t expect_max = budget ? 3 * GiB / 2 : free;
         CHECK(m.vram.total_heap_size == total);
         CHECK(m.vram.usable_heap_size == m.vram.total_heap_size);
         CHECK(m.cpu_accessible_vram.total_heap_size == live.vram_vis_total);
         CHECK(m.cpu_accessible_vram.usable_heap_size == live.vram_vis_total);
         CHECK(m.vram.max_allocation == expect_max);
         /* Visible headroom is conservative: min(vis*3/4, vis_free, shared avail). */
         {
            uint64_t vis_max = live.vram_vis_total * 3 / 4;
            if (vis_max > live.vram_vis_free)
               vis_max = live.vram_vis_free;
            if (vis_max > expect_max)
               vis_max = expect_max;
            CHECK(m.cpu_accessible_vram.max_allocation == vis_max);
         }
         CHECK(m.cpu_accessible_vram.heap_usage == live.vram_vis_total - live.vram_vis_free);
         CHECK(m.gtt.total_heap_size == live.gtt_cap && m.gtt.heap_usage == live.gtt_used);
         CHECK(m.gtt.max_allocation == live.gtt_max_bo);
         CHECK(m.vram.heap_usage == used);
         uint64_t usage = 0;
         query(dev, AMDGPU_INFO_VRAM_USAGE, 0, &usage, sizeof(usage));
         CHECK(usage == used);
         /* VRAM_GTT must succeed so heap queries and VK_EXT_memory_budget are not stuck full. */
         {
            struct drm_amdgpu_info_vram_gtt vg = {0};
            query(dev, AMDGPU_INFO_VRAM_GTT, 0, &vg, sizeof(vg));
            CHECK(vg.vram_size == total);
            CHECK(vg.vram_cpu_accessible_size == live.vram_vis_total);
            CHECK(vg.gtt_size == live.gtt_cap);
         }
         /* A later query must not use the original canned heap or a cached snapshot. */
         live.vram_vis_free -= MiB;
         query(dev, AMDGPU_INFO_VIS_VRAM_USAGE, 0, &usage, sizeof(usage));
         CHECK(usage == live.vram_vis_total - live.vram_vis_free);
         close_device(dev);
         printf("checked gfx1200 %u GiB profile, budget=%u, heap=%llu bytes\n", gib, budget,
                (unsigned long long)m.vram.total_heap_size);
      }
   }
   reset(0x0c0000, 8 * GiB);
   live.vram_vis_total = 8 * GiB - 8 * MiB;
   live.vram_vis_free = live.vram_vis_total - 64 * MiB;
   live.vram_hi_total = live.vram_hi_free = 0;
   acd_device_handle dev = open_device();
   if (dev) {
      struct drm_amdgpu_memory_info m = {0};
      query(dev, AMDGPU_INFO_MEMORY, 0, &m, sizeof(m));
      CHECK(m.vram.total_heap_size == live.vram_vis_total);
      CHECK(m.cpu_accessible_vram.total_heap_size == live.vram_vis_total);
      CHECK(m.vram.heap_usage == 64 * MiB);
      close_device(dev);
   }
   puts("checked all-visible 8 GiB profile with no high VRAM pool");
}

static void
post_submit_allocation(void)
{
   reset(0x0c0000, 16 * GiB);
   acd_device_handle dev = open_device();
   if (!dev) return;
   struct util_sync_provider *sync = acd_device_get_sync_provider(dev);
   uint32_t handle = 0;
   CHECK(sync->create(sync, 0, &handle) == 0);
   struct drm_amdgpu_cs_chunk_ib ib = {.va_start = 0x10000, .ib_bytes = 4};
   struct drm_amdgpu_cs_chunk_syncobj signal = {.handle = handle, .point = 1};
   struct drm_amdgpu_cs_chunk chunks[] = {
      {.chunk_id = AMDGPU_CHUNK_ID_IB, .length_dw = sizeof(ib) / 4, .chunk_data = (uintptr_t)&ib},
      {.chunk_id = AMDGPU_CHUNK_ID_SYNCOBJ_TIMELINE_SIGNAL, .length_dw = sizeof(signal) / 4,
       .chunk_data = (uintptr_t)&signal},
   };
   uint64_t pointers[] = {(uintptr_t)&chunks[0], (uintptr_t)&chunks[1]};
   union drm_amdgpu_cs cs = {.in = {.ctx_id = 1, .num_chunks = 2, .chunks = (uintptr_t)pointers}};
   accepted_submits = failed_sync_allocations = 0;
   fail_sync_allocation = true;
   int result = acd_ioctl(dev, DRM_AMDGPU_CS, &cs, sizeof(cs));
   fail_sync_allocation = false;
   CHECK(result == -ECANCELED);
   CHECK(accepted_submits == 1 && failed_sync_allocations == 1);
   cs = (union drm_amdgpu_cs){.in = {.ctx_id = 1, .num_chunks = 2, .chunks = (uintptr_t)pointers}};
   CHECK(acd_ioctl(dev, DRM_AMDGPU_CS, &cs, sizeof(cs)) == -ECANCELED);
   CHECK(accepted_submits == 1);
   printf("POST_SUBMIT_ALLOCATION result=%d accepted=%u allocation_failures=%u\n",
          result, accepted_submits, failed_sync_allocations);
   CHECK(sync->destroy(sync, handle) == 0);
   close_device(dev);
}

static void
timeline_queries(void)
{
   reset(0x0c0000, 16 * GiB);
   acd_device_handle dev = open_device();
   if (!dev) return;
   struct util_sync_provider *sync = acd_device_get_sync_provider(dev);
   uint32_t handle = 0;
   CHECK(sync->create(sync, 0, &handle) == 0);
   uint64_t point = 5;
   CHECK(sync->timeline_signal(sync, &handle, &point, 1) == 0);
   CHECK(acd_sync_cs_attach(sync, handle, 9, 1) == 0);
   wait_result = 0;
   wait_calls = 0;
   wait_busy = true;
   wait_retired = 0;
   point = UINT64_MAX;
   CHECK(sync->query(sync, &handle, &point, 1, 0) == 0);
   CHECK(point == 5 && wait_calls == 1);

   const struct { uint32_t result; int error; } errors[] = {
      {ACD_KR_TIMEOUT, ECANCELED}, {ACD_KR_ABORTED, ECANCELED},
      {ACD_KR_NOT_READY, ENODEV}, {ACD_KR_BAD_ARGUMENT, EINVAL},
   };
   for (unsigned i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i) {
      wait_result = errors[i].result;
      point = UINT64_MAX;
      errno = 0;
      int result = sync->query(sync, &handle, &point, 1, 0);
      int error = errno;
      CHECK(result == -1 && error == errors[i].error);
      CHECK(point == UINT64_MAX);
      printf("TIMELINE_QUERY transport=%#x result=%d errno=%d output_untouched=%u\n",
             wait_result, result, error, point == UINT64_MAX);
   }
   wait_result = 0;
   point = UINT64_MAX;
   CHECK(sync->query(sync, &handle, &point, 1, 0) == 0);
   CHECK(point == 5);
   point = 9;
   errno = 0;
   CHECK(sync->timeline_wait(sync, &handle, &point, 1, 0, 0, NULL) == -1);
   CHECK(errno == ETIME);
   wait_busy = false;
   wait_retired = 1;
   CHECK(sync->query(sync, &handle, &point, 1, 0) == 0);
   CHECK(point == 9);
   unsigned before = wait_calls;
   wait_result = ACD_KR_ABORTED;
   CHECK(sync->query(sync, &handle, &point, 1, 0) == 0);
   CHECK(point == 9 && wait_calls == before);
   CHECK(sync->destroy(sync, handle) == 0);
   close_device(dev);
   wait_result = 0;
}

int main(void)
{
   printf("Mesa QueryInfo layout: size=%zu gc_version=%zu vram_vis_total=%zu vram_hi_total=%zu\n",
          sizeof(struct rdna4n_info), __builtin_offsetof(struct rdna4n_info, gc_version),
          __builtin_offsetof(struct rdna4n_info, vram_vis_total),
          __builtin_offsetof(struct rdna4n_info, vram_hi_total));
   selection();
   memory();
   post_submit_allocation();
   timeline_queries();
   for (unsigned i = 0; i < 2; ++i) {
      reset(0x0c0001, 16 * GiB);
      setenv("RADV_DARWIN_HOST_BO", "1", 1);
      if (i) setenv("RADV_DARWIN_CHIP", "gfx1200", 1);
      acd_device_handle dev = open_device();
      if (dev) { identity(dev, i, false); close_device(dev); }
      reset(0x0c0001, 16 * GiB);
      setenv("RADV_DARWIN_MOCK", "1", 1);
      if (i) setenv("RADV_DARWIN_MOCK_GC", "gfx1200", 1);
      dev = open_device();
      if (dev) { identity(dev, i, true); close_device(dev); }
   }
   printf("%u checks, %u failures (synthetic QueryInfo; no hardware)\n", checks, failures);
   return failures ? 1 : 0;
}
