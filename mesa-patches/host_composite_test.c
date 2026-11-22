// SPDX-License-Identifier: MIT
// Host test for segmented Darwin host-pointer imports. The generated
// host_composite_production.inc contains the real functions extracted from
// radv_amdgpu_bo.c and radv_amdgpu_cs.c; only the kernel transport is stubbed.
#ifndef HAVE_AMDGPU_DARWIN
#define HAVE_AMDGPU_DARWIN 1
#endif

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

typedef enum {
   VK_SUCCESS = 0,
   VK_ERROR_OUT_OF_HOST_MEMORY = -1,
   VK_ERROR_OUT_OF_DEVICE_MEMORY = -2,
   VK_ERROR_UNKNOWN = -13,
   VK_ERROR_INVALID_EXTERNAL_HANDLE = -1000072003,
} VkResult;

enum radeon_bo_domain {
   RADEON_DOMAIN_GTT = 2,
   RADEON_DOMAIN_VRAM = 4,
};

enum radeon_bo_flag {
   RADEON_FLAG_GL2_BYPASS = 1 << 4,
   RADEON_FLAG_READ_ONLY = 1 << 7,
   RADEON_FLAG_32BIT = 1 << 8,
   RADEON_FLAG_REPLAYABLE = 1 << 10,
   RADEON_FLAG_VM_UPDATE_WAIT = 1 << 13,
   RADEON_FLAG_VM_PAD_1PAGE = 1 << 14,
};

#define AMDGPU_VA_OP_MAP 1
#define AMDGPU_VA_OP_UNMAP 2
#define AMDGPU_VA_OP_CLEAR 3
#define AMDGPU_VA_OP_REPLACE 4
#define AMDGPU_VM_PAGE_READABLE (1u << 1)
#define AMDGPU_VM_PAGE_WRITEABLE (1u << 2)
#define AMDGPU_VM_PAGE_EXECUTABLE (1u << 3)
#define AMDGPU_VM_PAGE_PRT (1u << 4)
#define AMDGPU_VM_MTYPE_UC (1u << 5)
#define AMDGPU_VA_RANGE_32_BIT 0x1
#define AMDGPU_VA_RANGE_HIGH 0x2
#define AMDGPU_VA_RANGE_REPLAYABLE 0x4
#define DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL (1 << 0)
#define DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT (1 << 1)

#define MIN2(a, b) ((a) < (b) ? (a) : (b))
#define MAX2(a, b) ((a) > (b) ? (a) : (b))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define CALLOC_STRUCT(t) ((struct t *)calloc(1, sizeof(struct t)))
#define FREE(p) free(p)
#define UNUSED __attribute__((unused))
#define ASSERTED UNUSED

static inline uint64_t
align64(uint64_t value, uint64_t alignment)
{
   assert(alignment && !(alignment & (alignment - 1)));
   return (value + alignment - 1) & ~(alignment - 1);
}

static inline unsigned
util_last_bit64(uint64_t value)
{
   return value ? 64u - (unsigned)__builtin_clzll(value) : 0u;
}

#define p_atomic_add(v, i) ((void)__atomic_add_fetch((v), (i), __ATOMIC_ACQ_REL))

typedef int simple_mtx_t;

static inline void
simple_mtx_lock(simple_mtx_t *mutex)
{
   (void)mutex;
}

static inline void
simple_mtx_unlock(simple_mtx_t *mutex)
{
   (void)mutex;
}

enum amd_gfx_level {
   GFX9,
   GFX12,
};

struct radeon_winsys {
   int unused;
};

struct radeon_winsys_bo {
   uint64_t va;
   uint64_t size;
   bool is_local;
   bool vram_no_cpu_access;
   bool use_global_list;
   bool gfx12_allow_dcc;
   bool is_virtual;
   enum radeon_bo_domain initial_domain;
   uint64_t obj_id;
};

struct radeon_info {
   uint32_t pte_fragment_size;
   uint32_t gart_page_size;
   enum amd_gfx_level gfx_level;
   unsigned address_prt_wa_control_bit;
};

struct radv_amdgpu_alloc_tracker {
   uint64_t allocated_vram;
   uint64_t allocated_vram_vis;
   uint64_t allocated_gtt;
};

struct ac_drm_device {
   int is_darwin;
};
typedef struct ac_drm_device ac_drm_device;

struct acd_kext {
   int unused;
};

enum amdgpu_gpu_va_range {
   amdgpu_gpu_va_range_general = 0,
};

enum amdgpu_bo_handle_type {
   amdgpu_bo_handle_type_kms = 1,
   amdgpu_bo_handle_type_dma_buf_fd = 2,
};

struct fake_va {
   uint64_t base;
   uint64_t size;
   bool live;
};
typedef struct fake_va *amdgpu_va_handle;

typedef struct {
   void *abo;
} ac_drm_bo;

struct radv_amdgpu_winsys_bo {
   struct radeon_winsys_bo base;
   amdgpu_va_handle va_handle;
   uint32_t flags;
   uint8_t priority;
   ac_drm_bo bo;
   uint32_t bo_handle;
   void *cpu_map;
   bool emulate_sparse_residency;
   struct radv_amdgpu_winsys_bo **children;
   uint32_t child_count;
   bool composite_pad;
};

static inline struct radv_amdgpu_winsys_bo *
radv_amdgpu_winsys_bo(struct radeon_winsys_bo *bo)
{
   return (struct radv_amdgpu_winsys_bo *)bo;
}

struct radv_amdgpu_winsys {
   struct radeon_winsys base;
   ac_drm_device *dev;
   int fd;
   struct radeon_info info;
   bool debug_all_bos;
   bool debug_log_bos;
   bool debug_vm;
   uint64_t perftest;
   struct radv_amdgpu_alloc_tracker *alloc_tracker;
   simple_mtx_t vm_ioctl_lock;
   uint32_t vm_timeline_syncobj;
   uint64_t vm_timeline_seq_num;
};

static inline struct radv_amdgpu_winsys *
radv_amdgpu_winsys(struct radeon_winsys *winsys)
{
   return (struct radv_amdgpu_winsys *)winsys;
}

struct ac_cmdbuf {
   int unused;
};

struct drm_amdgpu_bo_list_entry {
   uint32_t bo_handle;
   uint32_t bo_priority;
};

struct radv_amdgpu_cs {
   struct ac_cmdbuf base;
   struct radv_amdgpu_winsys *ws;
   unsigned max_num_buffers;
   unsigned num_buffers;
   struct drm_amdgpu_bo_list_entry *handles;
   VkResult status;
   int buffer_hash_table[1024];
};

static inline struct radv_amdgpu_cs *
radv_amdgpu_cs(struct ac_cmdbuf *base)
{
   return (struct radv_amdgpu_cs *)base;
}

union drm_amdgpu_gem_mmap {
   struct {
      uint32_t handle;
      uint32_t pad;
   } in;
   struct {
      uint64_t addr_ptr;
   } out;
};

#define DRM_AMDGPU_GEM_MMAP 0

int drm_ioctl_write_read(int fd, unsigned long request, void *data, size_t size);
int ac_drm_create_bo_from_user_mem(ac_drm_device *dev, void *cpu, uint64_t size,
                                   ac_drm_bo *bo);
int ac_drm_bo_export(ac_drm_device *dev, ac_drm_bo bo, enum amdgpu_bo_handle_type type,
                     uint32_t *handle);
int ac_drm_bo_free(ac_drm_device *dev, ac_drm_bo bo);
int ac_drm_bo_cpu_map(ac_drm_device *dev, ac_drm_bo bo, void **cpu);
int ac_drm_bo_cpu_unmap(ac_drm_device *dev, ac_drm_bo bo);
int ac_drm_va_range_alloc(ac_drm_device *dev, enum amdgpu_gpu_va_range type,
                          uint64_t size, uint64_t alignment, uint64_t required,
                          uint64_t *out_va, amdgpu_va_handle *out_handle,
                          uint64_t flags);
int ac_drm_va_range_free(amdgpu_va_handle handle);
int ac_drm_bo_va_op_raw(ac_drm_device *dev, uint32_t handle, uint64_t offset,
                        uint64_t size, uint64_t address, uint64_t flags,
                        uint32_t operation);
int ac_drm_bo_va_op_raw2(ac_drm_device *dev, uint32_t handle, uint64_t offset,
                         uint64_t size, uint64_t address, uint64_t flags,
                         uint32_t operation, uint32_t syncobj,
                         uint64_t point, uint64_t fence, uint32_t num_fences);
int ac_drm_cs_syncobj_timeline_wait(ac_drm_device *dev, uint32_t *handles,
                                    uint64_t *points, unsigned count,
                                    int64_t timeout, unsigned flags,
                                    uint32_t *signaled);
struct acd_kext *ac_drm_darwin_kext(ac_drm_device *dev, uint32_t *abi_minor);
void radv_amdgpu_log_bo(struct radv_amdgpu_winsys *ws,
                        struct radv_amdgpu_winsys_bo *bo, bool destroyed);
int radv_amdgpu_global_bo_list_add(struct radv_amdgpu_winsys *ws,
                                   struct radv_amdgpu_winsys_bo *bo);
void radv_amdgpu_global_bo_list_del(struct radv_amdgpu_winsys *ws,
                                    struct radv_amdgpu_winsys_bo *bo);
VkResult radv_amdgpu_winsys_virtual_bo_create(struct radeon_winsys *ws,
                                               uint64_t size, unsigned alignment,
                                               enum radeon_bo_flag flags,
                                               uint64_t replay_address,
                                               struct radeon_winsys_bo **out_bo);
void radv_amdgpu_winsys_virtual_bo_destroy(struct radeon_winsys *ws,
                                           struct radeon_winsys_bo *bo);

static uint64_t radv_amdgpu_bo_va_size(uint64_t size, uint32_t flags);
static int radv_amdgpu_bo_va_op(struct radv_amdgpu_winsys *ws, uint32_t handle,
                                uint64_t offset, uint64_t size, uint64_t address,
                                uint32_t bo_flags, uint64_t internal_flags,
                                uint32_t operation);
static VkResult
radv_amdgpu_host_child_from_ptr(struct radv_amdgpu_winsys *ws, void *pointer,
                                uint64_t size, uint64_t va, unsigned priority,
                                struct radv_amdgpu_winsys_bo **out_child);
static VkResult
radv_amdgpu_host_composite_from_ptr(struct radeon_winsys *ws, void *pointer,
                                    uint64_t size, unsigned priority,
                                    uint64_t chunk_limit,
                                    struct radeon_winsys_bo **out_bo);
static uint64_t
radv_amdgpu_get_optimal_vm_alignment(struct radv_amdgpu_winsys *ws, uint64_t size,
                                      unsigned alignment);
static VkResult
radv_amdgpu_winsys_bo_from_ptr(struct radeon_winsys *ws, void *pointer,
                               uint64_t size, unsigned priority,
                               struct radeon_winsys_bo **out_bo);
static void radv_amdgpu_winsys_bo_destroy(struct radeon_winsys *ws,
                                          struct radeon_winsys_bo *bo);
static void *radv_amdgpu_winsys_bo_map(struct radeon_winsys *ws,
                                       struct radeon_winsys_bo *bo,
                                       bool use_fixed_addr, void *fixed_addr);
static void radv_amdgpu_winsys_bo_unmap(struct radeon_winsys *ws,
                                        struct radeon_winsys_bo *bo, bool replace);
static bool radv_amdgpu_winsys_get_fd(struct radeon_winsys *ws,
                                      struct radeon_winsys_bo *bo, int *fd);
static int radv_amdgpu_cs_find_buffer(struct radv_amdgpu_cs *cs, uint32_t handle);
static void radv_amdgpu_cs_add_buffer_internal(struct radv_amdgpu_cs *cs,
                                               uint32_t handle, uint8_t priority);
static void radv_amdgpu_cs_add_buffer(struct ac_cmdbuf *cs,
                                      struct radeon_winsys_bo *bo);

static unsigned g_checks;
static unsigned g_failures;

#define CHECK(condition)                                                        \
   do {                                                                         \
      ++g_checks;                                                               \
      if (!(condition)) {                                                       \
         ++g_failures;                                                          \
         fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition);           \
      }                                                                         \
   } while (0)

static int test_munmap(void *address, size_t size);
static int (*const real_munmap)(void *, size_t) = munmap;

#define munmap test_munmap
#include "host_composite_production.inc"
#undef munmap

enum {
   MAX_IMPORTS = 4096,
   MAX_MAPS = 4096,
};

struct fake_import {
   void *pointer;
   uint64_t size;
   uint32_t kms_handle;
   bool live;
   bool readonly;
};

struct fake_map {
   uint32_t kms_handle;
   uint64_t offset;
   uint64_t size;
   uint64_t address;
   uint64_t requested_flags;
   uint64_t effective_flags;
   bool live;
};

struct fake_unmap {
   uint32_t kms_handle;
   uint64_t offset;
   uint64_t size;
   uint64_t address;
   bool succeeded;
};

static struct radv_amdgpu_winsys g_ws;
static ac_drm_device g_dev;
static struct radv_amdgpu_alloc_tracker g_tracker;
static struct fake_import g_imports[MAX_IMPORTS];
static struct fake_map g_maps[MAX_MAPS];
static struct fake_unmap g_unmaps[MAX_MAPS];
static uint32_t g_free_order[MAX_IMPORTS];

static unsigned g_import_attempts;
static unsigned g_import_successes;
static unsigned g_import_live;
static unsigned g_free_calls;
static unsigned g_free_failures;
static unsigned g_free_with_live_map;
static unsigned g_export_calls;
static unsigned g_map_attempts;
static unsigned g_map_live;
static unsigned g_map_duplicates;
static unsigned g_map_record_count;
static unsigned g_unmap_attempts;
static unsigned g_unmap_record_count;
static unsigned g_va_alloc_calls;
static unsigned g_va_free_calls;
static unsigned g_va_live;
static uint64_t g_va_base;
static uint64_t g_last_va_size;
static uint64_t g_last_va_alignment;
static unsigned g_log_bo_calls;
static unsigned g_global_add_calls;
static unsigned g_global_del_calls;
static unsigned g_cpu_map_calls;
static unsigned g_cpu_unmap_calls;
static unsigned g_munmap_calls;
static unsigned g_unsupported_va_ops;
static unsigned g_raw2_calls;
static unsigned g_virtual_create_calls;
static unsigned g_virtual_destroy_calls;

static unsigned g_fail_import_at;
static unsigned g_fail_export_at;
static unsigned g_fail_map_at;
static int g_map_fail_code;
static unsigned g_fail_unmap_at;
static int g_unmap_fail_code;
static unsigned g_fail_free_at;
static int g_free_fail_code;
static bool g_fail_va_alloc;
static bool g_darwin_kext_present;
static int g_readonly_mode;
static uint32_t g_next_kms_handle;

static void
reset_state(void)
{
   memset(&g_ws, 0, sizeof(g_ws));
   memset(&g_dev, 0, sizeof(g_dev));
   memset(&g_tracker, 0, sizeof(g_tracker));
   memset(g_imports, 0, sizeof(g_imports));
   memset(g_maps, 0, sizeof(g_maps));
   memset(g_unmaps, 0, sizeof(g_unmaps));
   memset(g_free_order, 0, sizeof(g_free_order));

   g_dev.is_darwin = 1;
   g_ws.dev = &g_dev;
   g_ws.info.pte_fragment_size = 65536;
   g_ws.info.gart_page_size = 4096;
   g_ws.info.gfx_level = GFX12;
   g_ws.info.address_prt_wa_control_bit = 48;
   g_ws.alloc_tracker = &g_tracker;

   g_import_attempts = 0;
   g_import_successes = 0;
   g_import_live = 0;
   g_free_calls = 0;
   g_free_failures = 0;
   g_free_with_live_map = 0;
   g_export_calls = 0;
   g_map_attempts = 0;
   g_map_live = 0;
   g_map_duplicates = 0;
   g_map_record_count = 0;
   g_unmap_attempts = 0;
   g_unmap_record_count = 0;
   g_va_alloc_calls = 0;
   g_va_free_calls = 0;
   g_va_live = 0;
   g_va_base = UINT64_C(0x100000000);
   g_last_va_size = 0;
   g_last_va_alignment = 0;
   g_log_bo_calls = 0;
   g_global_add_calls = 0;
   g_global_del_calls = 0;
   g_cpu_map_calls = 0;
   g_cpu_unmap_calls = 0;
   g_munmap_calls = 0;
   g_unsupported_va_ops = 0;
   g_raw2_calls = 0;
   g_virtual_create_calls = 0;
   g_virtual_destroy_calls = 0;

   g_fail_import_at = 0;
   g_fail_export_at = 0;
   g_fail_map_at = 0;
   g_map_fail_code = 0;
   g_fail_unmap_at = 0;
   g_unmap_fail_code = 0;
   g_fail_free_at = 0;
   g_free_fail_code = 0;
   g_fail_va_alloc = false;
   g_darwin_kext_present = true;
   g_readonly_mode = 0;
   g_next_kms_handle = 0x1000;
}

static struct fake_import *
import_by_kms(uint32_t kms_handle)
{
   for (unsigned i = 0; i < g_import_successes; ++i) {
      if (g_imports[i].kms_handle == kms_handle)
         return &g_imports[i];
   }
   return NULL;
}

static bool
ranges_overlap(uint64_t a, uint64_t asize, uint64_t b, uint64_t bsize)
{
   return a < b + bsize && b < a + asize;
}

int
ac_drm_create_bo_from_user_mem(ac_drm_device *dev, void *cpu, uint64_t size,
                               ac_drm_bo *bo)
{
   (void)dev;
   ++g_import_attempts;
   if (g_fail_import_at && g_import_attempts == g_fail_import_at)
      return -ENOMEM;
   if (!cpu || !size || size > (UINT64_C(64) << 20))
      return -EINVAL;

   struct fake_import *import = &g_imports[g_import_successes];
   const unsigned index = g_import_successes++;
   import->pointer = cpu;
   import->size = size;
   import->kms_handle = g_next_kms_handle++;
   import->live = true;
   import->readonly =
      g_readonly_mode == 2 || (g_readonly_mode == 1 && (index & 1u));
   ++g_import_live;
   bo->abo = (void *)(uintptr_t)import->kms_handle;
   return 0;
}

int
ac_drm_bo_export(ac_drm_device *dev, ac_drm_bo bo,
                 enum amdgpu_bo_handle_type type, uint32_t *handle)
{
   (void)dev;
   (void)type;
   ++g_export_calls;
   if (g_fail_export_at && g_export_calls == g_fail_export_at)
      return -ENOMEM;

   struct fake_import *import =
      import_by_kms((uint32_t)(uintptr_t)bo.abo);
   if (!import || !import->live)
      return -EINVAL;
   *handle = import->kms_handle;
   return 0;
}

int
ac_drm_bo_free(ac_drm_device *dev, ac_drm_bo bo)
{
   (void)dev;
   ++g_free_calls;

   struct fake_import *import =
      import_by_kms((uint32_t)(uintptr_t)bo.abo);
   if (!import || !import->live) {
      ++g_free_failures;
      return -EINVAL;
   }

   for (unsigned i = 0; i < g_map_record_count; ++i) {
      if (g_maps[i].live && g_maps[i].kms_handle == import->kms_handle)
         ++g_free_with_live_map;
   }

   import->live = false;
   --g_import_live;
   g_free_order[g_free_calls - 1] = import->kms_handle;
   if (g_fail_free_at && g_free_calls == g_fail_free_at)
      return g_free_fail_code ? g_free_fail_code : -EIO;
   return 0;
}

int
ac_drm_bo_cpu_map(ac_drm_device *dev, ac_drm_bo bo, void **cpu)
{
   (void)dev;
   ++g_cpu_map_calls;
   struct fake_import *import =
      import_by_kms((uint32_t)(uintptr_t)bo.abo);
   if (!import || !import->live)
      return -EINVAL;
   *cpu = import->pointer;
   return 0;
}

int
ac_drm_bo_cpu_unmap(ac_drm_device *dev, ac_drm_bo bo)
{
   (void)dev;
   (void)bo;
   ++g_cpu_unmap_calls;
   return 0;
}

int
ac_drm_va_range_alloc(ac_drm_device *dev, enum amdgpu_gpu_va_range type,
                      uint64_t size, uint64_t alignment, uint64_t required,
                      uint64_t *out_va, amdgpu_va_handle *out_handle,
                      uint64_t flags)
{
   (void)dev;
   (void)type;
   (void)required;
   (void)flags;
   ++g_va_alloc_calls;
   if (g_fail_va_alloc)
      return -ENOMEM;

   struct fake_va *va = calloc(1, sizeof(*va));
   if (!va)
      return -ENOMEM;
   va->base = g_va_base;
   va->size = size;
   va->live = true;
   *out_va = va->base;
   *out_handle = va;
   g_last_va_size = size;
   g_last_va_alignment = alignment;
   ++g_va_live;
   return 0;
}

int
ac_drm_va_range_free(amdgpu_va_handle handle)
{
   if (!handle || !handle->live) {
      ++g_failures;
      fprintf(stderr, "FAIL: invalid VA range free\n");
      return -EINVAL;
   }
   handle->live = false;
   --g_va_live;
   ++g_va_free_calls;
   free(handle);
   return 0;
}

int
ac_drm_bo_va_op_raw(ac_drm_device *dev, uint32_t handle, uint64_t offset,
                    uint64_t size, uint64_t address, uint64_t flags,
                    uint32_t operation)
{
   (void)dev;
   struct fake_import *import = import_by_kms(handle);
   if (!import || !import->live)
      return -EINVAL;

   if (operation == AMDGPU_VA_OP_MAP) {
      ++g_map_attempts;
      if (g_fail_map_at && g_map_attempts == g_fail_map_at)
         return g_map_fail_code ? g_map_fail_code : -ENOMEM;

      for (unsigned i = 0; i < g_map_record_count; ++i) {
         if (g_maps[i].live &&
             ranges_overlap(address, size, g_maps[i].address, g_maps[i].size)) {
            ++g_map_duplicates;
            return -EEXIST;
         }
      }
      if (g_map_record_count == ARRAY_SIZE(g_maps))
         return -ENOMEM;

      uint64_t effective = flags;
      if (import->readonly)
         effective &= ~AMDGPU_VM_PAGE_WRITEABLE;
      struct fake_map *map = &g_maps[g_map_record_count++];
      map->kms_handle = handle;
      map->offset = offset;
      map->size = size;
      map->address = address;
      map->requested_flags = flags;
      map->effective_flags = effective;
      map->live = true;
      ++g_map_live;
      return 0;
   }

   if (operation == AMDGPU_VA_OP_UNMAP) {
      ++g_unmap_attempts;
      if (g_fail_unmap_at && g_unmap_attempts == g_fail_unmap_at)
         return g_unmap_fail_code ? g_unmap_fail_code : -EIO;

      for (unsigned i = 0; i < g_map_record_count; ++i) {
         struct fake_map *map = &g_maps[i];
         if (map->live && map->kms_handle == handle && map->offset == offset &&
             map->size == size && map->address == address) {
            map->live = false;
            --g_map_live;
            if (g_unmap_record_count < ARRAY_SIZE(g_unmaps)) {
               struct fake_unmap *unmap = &g_unmaps[g_unmap_record_count++];
               unmap->kms_handle = handle;
               unmap->offset = offset;
               unmap->size = size;
               unmap->address = address;
               unmap->succeeded = true;
            }
            return 0;
         }
      }
      return -EINVAL;
   }

   ++g_unsupported_va_ops;
   return -EINVAL;
}

int
ac_drm_bo_va_op_raw2(ac_drm_device *dev, uint32_t handle, uint64_t offset,
                     uint64_t size, uint64_t address, uint64_t flags,
                     uint32_t operation, uint32_t syncobj, uint64_t point,
                     uint64_t fence, uint32_t num_fences)
{
   (void)dev;
   (void)handle;
   (void)offset;
   (void)size;
   (void)address;
   (void)flags;
   (void)operation;
   (void)syncobj;
   (void)point;
   (void)fence;
   (void)num_fences;
   ++g_raw2_calls;
   return -EINVAL;
}

int
ac_drm_cs_syncobj_timeline_wait(ac_drm_device *dev, uint32_t *handles,
                                uint64_t *points, unsigned count,
                                int64_t timeout, unsigned flags,
                                uint32_t *signaled)
{
   (void)dev;
   (void)handles;
   (void)points;
   (void)count;
   (void)timeout;
   (void)flags;
   (void)signaled;
   return 0;
}

struct acd_kext *
ac_drm_darwin_kext(ac_drm_device *dev, uint32_t *abi_minor)
{
   (void)dev;
   (void)abi_minor;
   return g_darwin_kext_present ? (struct acd_kext *)(uintptr_t)1 : NULL;
}

void
radv_amdgpu_log_bo(struct radv_amdgpu_winsys *ws,
                   struct radv_amdgpu_winsys_bo *bo, bool destroyed)
{
   (void)ws;
   (void)bo;
   (void)destroyed;
   ++g_log_bo_calls;
}

int
radv_amdgpu_global_bo_list_add(struct radv_amdgpu_winsys *ws,
                               struct radv_amdgpu_winsys_bo *bo)
{
   (void)ws;
   (void)bo;
   ++g_global_add_calls;
   return VK_SUCCESS;
}

void
radv_amdgpu_global_bo_list_del(struct radv_amdgpu_winsys *ws,
                               struct radv_amdgpu_winsys_bo *bo)
{
   (void)ws;
   (void)bo;
   ++g_global_del_calls;
}

VkResult
radv_amdgpu_winsys_virtual_bo_create(struct radeon_winsys *ws, uint64_t size,
                                     unsigned alignment,
                                     enum radeon_bo_flag flags,
                                     uint64_t replay_address,
                                     struct radeon_winsys_bo **out_bo)
{
   (void)ws;
   (void)size;
   (void)alignment;
   (void)flags;
   (void)replay_address;
   ++g_virtual_create_calls;
   *out_bo = NULL;
   return VK_ERROR_OUT_OF_DEVICE_MEMORY;
}

void
radv_amdgpu_winsys_virtual_bo_destroy(struct radeon_winsys *ws,
                                      struct radeon_winsys_bo *bo)
{
   (void)ws;
   (void)bo;
   ++g_virtual_destroy_calls;
}

int
drm_ioctl_write_read(int fd, unsigned long request, void *data, size_t size)
{
   (void)fd;
   (void)request;
   (void)data;
   (void)size;
   return -1;
}

static int
test_munmap(void *address, size_t size)
{
   ++g_munmap_calls;
   return real_munmap(address, size);
}

static bool
gpu_read8(uint64_t address, uint8_t *value)
{
   for (unsigned i = 0; i < g_map_record_count; ++i) {
      const struct fake_map *map = &g_maps[i];
      if (!map->live || address < map->address ||
          address >= map->address + map->size)
         continue;
      const struct fake_import *import = import_by_kms(map->kms_handle);
      if (!import || !import->live)
         return false;
      const uint64_t offset = map->offset + address - map->address;
      *value = ((const uint8_t *)import->pointer)[offset];
      return true;
   }
   return false;
}

static bool
gpu_write8(uint64_t address, uint8_t value)
{
   for (unsigned i = 0; i < g_map_record_count; ++i) {
      const struct fake_map *map = &g_maps[i];
      if (!map->live || address < map->address ||
          address >= map->address + map->size)
         continue;
      if (!(map->effective_flags & AMDGPU_VM_PAGE_WRITEABLE))
         return false;
      struct fake_import *import = import_by_kms(map->kms_handle);
      if (!import || !import->live)
         return false;
      const uint64_t offset = map->offset + address - map->address;
      ((uint8_t *)import->pointer)[offset] = value;
      return true;
   }
   return false;
}

static void *
mapped_cpu(size_t size)
{
   void *pointer = mmap(NULL, size, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
   if (pointer == MAP_FAILED)
      return NULL;
   memset(pointer, 0, size);
   return pointer;
}

static void
release_cpu(void *pointer, size_t size)
{
   CHECK(pointer != NULL);
   if (pointer)
      CHECK(real_munmap(pointer, size) == 0);
}

static void
check_destroyed(unsigned expected_children)
{
   CHECK(g_va_alloc_calls == 1);
   CHECK(g_va_free_calls == 1);
   CHECK(g_va_live == 0);
   CHECK(g_import_live == 0);
   CHECK(g_map_live == 0);
   CHECK(g_tracker.allocated_gtt == 0);
   CHECK(g_munmap_calls == 0);
   CHECK(g_unmap_attempts == expected_children);
   CHECK(g_free_calls == expected_children);
   for (unsigned i = 0; i < expected_children; ++i) {
      const unsigned expected = (unsigned)g_imports[expected_children - i - 1].kms_handle;
      CHECK(g_free_order[i] == expected);
   }
}

static void
test_small_chunk_dual_view(void)
{
   const size_t size = 3 * 4096;
   uint8_t *cpu = mapped_cpu(size);
   CHECK(cpu != NULL);
   if (!cpu)
      return;

   reset_state();
   struct radeon_winsys_bo *bo_out = NULL;
   VkResult result = radv_amdgpu_host_composite_from_ptr(
      &g_ws.base, cpu, size, 7, 4096, &bo_out);
   CHECK(result == VK_SUCCESS);
   CHECK(bo_out != NULL);
   if (result != VK_SUCCESS || !bo_out) {
      release_cpu(cpu, size);
      return;
   }

   struct radv_amdgpu_winsys_bo *bo = radv_amdgpu_winsys_bo(bo_out);
   CHECK(bo->child_count == 3);
   CHECK(bo->cpu_map == cpu);
   CHECK(!bo->base.is_virtual);
   CHECK(bo->base.initial_domain == RADEON_DOMAIN_GTT);
   CHECK(bo->base.size == size);
   CHECK(bo->va_handle != NULL);
   CHECK(g_va_alloc_calls == 1);
   CHECK(g_import_successes == 3);
   CHECK(g_import_live == 3);
   CHECK(g_map_attempts == 3);
   CHECK(g_map_live == 3);
   CHECK(g_map_duplicates == 0);
   CHECK(g_tracker.allocated_gtt == size);

   for (unsigned i = 0; i < bo->child_count; ++i) {
      CHECK(bo->children[i]->base.va == bo->base.va + i * 4096ull);
      CHECK(bo->children[i]->base.size == 4096);
      CHECK(bo->children[i]->va_handle == NULL);
      CHECK(bo->children[i]->cpu_map == NULL);
      CHECK(g_imports[i].pointer == cpu + i * 4096);
      CHECK(g_imports[i].size == 4096);
   }

   cpu[0] = 0x11;
   cpu[4095] = 0x22;
   cpu[4096] = 0x33;
   cpu[8191] = 0x44;
   cpu[8192] = 0x55;
   cpu[12287] = 0x66;
   uint8_t value = 0;
   CHECK(gpu_read8(bo->base.va, &value) && value == 0x11);
   CHECK(gpu_read8(bo->base.va + 4095, &value) && value == 0x22);
   CHECK(gpu_read8(bo->base.va + 4096, &value) && value == 0x33);
   CHECK(gpu_read8(bo->base.va + 8191, &value) && value == 0x44);
   CHECK(gpu_read8(bo->base.va + 8192, &value) && value == 0x55);
   CHECK(gpu_read8(bo->base.va + 12287, &value) && value == 0x66);

   CHECK(gpu_write8(bo->base.va + 4095, 0x9a));
   CHECK(cpu[4095] == 0x9a);
   CHECK(gpu_write8(bo->base.va + 4096, 0x9b));
   CHECK(cpu[4096] == 0x9b);
   CHECK(gpu_write8(bo->base.va + 8191, 0x9c));
   CHECK(cpu[8191] == 0x9c);
   CHECK(gpu_write8(bo->base.va + 8192, 0x9d));
   CHECK(cpu[8192] == 0x9d);

   CHECK(radv_amdgpu_winsys_bo_map(&g_ws.base, bo_out, false, NULL) == cpu);
   CHECK(radv_amdgpu_winsys_bo_map(&g_ws.base, bo_out, true,
                                   (void *)(uintptr_t)0x12345000) == cpu);
   CHECK(g_cpu_map_calls == 0);
   radv_amdgpu_winsys_bo_unmap(&g_ws.base, bo_out, false);
   radv_amdgpu_winsys_bo_unmap(&g_ws.base, bo_out, true);
   CHECK(g_cpu_unmap_calls == 0);

   radv_amdgpu_winsys_bo_destroy(&g_ws.base, bo_out);
   check_destroyed(3);
   CHECK(cpu[4096] == 0x9b);
   cpu[1] = 0x77;
   CHECK(cpu[1] == 0x77);
   release_cpu(cpu, size);
}

static void
test_from_ptr_boundary(void)
{
   reset_state();
   struct radeon_winsys_bo *bo_out = NULL;
   void *small_pointer = (void *)(uintptr_t)UINT64_C(0x100000000);
   uint64_t small_size = UINT64_C(64) << 20;
   VkResult result = radv_amdgpu_winsys_bo_from_ptr(
      &g_ws.base, small_pointer, small_size, 3, &bo_out);
   CHECK(result == VK_SUCCESS);
   CHECK(bo_out != NULL);
   if (result == VK_SUCCESS && bo_out) {
      struct radv_amdgpu_winsys_bo *bo = radv_amdgpu_winsys_bo(bo_out);
      CHECK(bo->children == NULL);
      CHECK(bo->cpu_map == NULL);
      CHECK(bo->base.size == small_size);
      CHECK(g_import_successes == 1);
      CHECK(g_imports[0].size == small_size);
      CHECK(g_va_alloc_calls == 1);
      CHECK(g_map_attempts == 1);
      CHECK(g_tracker.allocated_gtt == small_size);
      CHECK(radv_amdgpu_winsys_bo_map(&g_ws.base, bo_out, false, NULL) ==
            small_pointer);
      radv_amdgpu_winsys_bo_destroy(&g_ws.base, bo_out);
      CHECK(g_va_free_calls == 1);
      CHECK(g_import_live == 0);
      CHECK(g_tracker.allocated_gtt == 0);
      CHECK(g_munmap_calls == 0);
   }

   reset_state();
   bo_out = NULL;
   void *large_pointer = (void *)(uintptr_t)UINT64_C(0x100000000);
   uint64_t large_size = (UINT64_C(64) << 20) + 4096;
   result = radv_amdgpu_winsys_bo_from_ptr(
      &g_ws.base, large_pointer, large_size, 3, &bo_out);
   CHECK(result == VK_SUCCESS);
   CHECK(bo_out != NULL);
   if (result == VK_SUCCESS && bo_out) {
      struct radv_amdgpu_winsys_bo *bo = radv_amdgpu_winsys_bo(bo_out);
      CHECK(bo->children != NULL);
      CHECK(bo->child_count == 2);
      CHECK(bo->cpu_map == large_pointer);
      CHECK(bo->children[0]->base.size == (UINT64_C(64) << 20));
      CHECK(bo->children[1]->base.size == 4096);
      CHECK(bo->children[1]->base.va == bo->base.va + (UINT64_C(64) << 20));
      CHECK(g_import_successes == 2);
      CHECK(g_map_live == 2);
      CHECK(radv_amdgpu_winsys_bo_map(&g_ws.base, bo_out, false, NULL) ==
            large_pointer);
      radv_amdgpu_winsys_bo_destroy(&g_ws.base, bo_out);
      check_destroyed(2);
   }
}

static void
test_synthetic_8gib_layout(void)
{
   reset_state();
   const uint64_t size = UINT64_C(8) << 30;
   void *pointer = (void *)(uintptr_t)UINT64_C(0x100000000);
   struct radeon_winsys_bo *bo_out = NULL;
   VkResult result = radv_amdgpu_host_composite_from_ptr(
      &g_ws.base, pointer, size, 4, RADV_DARWIN_HOST_CHUNK, &bo_out);
   CHECK(result == VK_SUCCESS);
   CHECK(bo_out != NULL);
   if (result != VK_SUCCESS || !bo_out)
      return;

   struct radv_amdgpu_winsys_bo *bo = radv_amdgpu_winsys_bo(bo_out);
   CHECK(bo->child_count == 128);
   CHECK(radv_amdgpu_winsys_bo(bo_out)->cpu_map == pointer);
   CHECK(g_va_alloc_calls == 1);
   CHECK(g_last_va_size == size);
   CHECK(g_import_successes == 128);
   CHECK(g_map_attempts == 128);
   CHECK(g_map_duplicates == 0);
   CHECK(g_tracker.allocated_gtt == size);
   for (unsigned i = 0; i < bo->child_count; ++i) {
      CHECK(bo->children[i]->base.va ==
            bo->base.va + (uint64_t)i * RADV_DARWIN_HOST_CHUNK);
      CHECK(bo->children[i]->base.size == RADV_DARWIN_HOST_CHUNK);
      CHECK(bo->children[i]->va_handle == NULL);
      CHECK(g_imports[i].pointer ==
            (uint8_t *)pointer + (uint64_t)i * RADV_DARWIN_HOST_CHUNK);
   }

   printf("synthetic 8 GiB layout: %u contiguous child VA/PTE requests "
          "(no physical backing)\n", bo->child_count);
   radv_amdgpu_winsys_bo_destroy(&g_ws.base, bo_out);
   check_destroyed(128);
}

static void
test_child_bound(void)
{
   const uint64_t pages_2049 = UINT64_C(2049) * 4096;
   const uint64_t pages_2048 = UINT64_C(2048) * 4096;
   void *pointer = (void *)(uintptr_t)UINT64_C(0x100000000);

   reset_state();
   struct radeon_winsys_bo *bo_out = (void *)(uintptr_t)1;
   VkResult result = radv_amdgpu_host_composite_from_ptr(
      &g_ws.base, pointer, pages_2049, 1, 4096, &bo_out);
   CHECK(result == VK_ERROR_OUT_OF_DEVICE_MEMORY);
   CHECK(bo_out == NULL);
   CHECK(g_va_alloc_calls == 0);
   CHECK(g_import_attempts == 0);

   reset_state();
   bo_out = NULL;
   result = radv_amdgpu_host_composite_from_ptr(
      &g_ws.base, pointer, pages_2048, 1, 4096, &bo_out);
   CHECK(result == VK_SUCCESS);
   CHECK(bo_out != NULL);
   if (result == VK_SUCCESS && bo_out) {
      struct radv_amdgpu_winsys_bo *bo = radv_amdgpu_winsys_bo(bo_out);
      CHECK(bo->child_count == 2048);
      CHECK(g_import_successes == 2048);
      CHECK(g_map_live == 2048);
      CHECK(g_tracker.allocated_gtt == pages_2048);
      radv_amdgpu_winsys_bo_destroy(&g_ws.base, bo_out);
      check_destroyed(2048);
   }
}

static void
check_failure_cleanup(unsigned expected_imports, unsigned expected_maps,
                      unsigned expected_unmaps)
{
   CHECK(g_import_successes == expected_imports);
   CHECK(g_import_live == 0);
   CHECK(g_free_calls == expected_imports);
   CHECK(g_map_live == 0);
   CHECK(g_unmap_attempts == expected_unmaps);
   CHECK(g_va_alloc_calls == 1);
   CHECK(g_va_free_calls == 1);
   CHECK(g_va_live == 0);
   CHECK(g_tracker.allocated_gtt == 0);
   CHECK(g_munmap_calls == 0);
   CHECK(g_map_attempts >= expected_maps);
}

static void
test_failure_rollback(void)
{
   const uint64_t size = 4 * 4096;
   void *pointer = (void *)(uintptr_t)UINT64_C(0x100000000);

   reset_state();
   g_fail_va_alloc = true;
   struct radeon_winsys_bo *bo_out = NULL;
   VkResult result = radv_amdgpu_host_composite_from_ptr(
      &g_ws.base, pointer, size, 1, 4096, &bo_out);
   CHECK(result == VK_ERROR_OUT_OF_DEVICE_MEMORY);
   CHECK(bo_out == NULL);
   CHECK(g_va_alloc_calls == 1 && g_va_free_calls == 0);
   CHECK(g_import_attempts == 0);

   reset_state();
   g_fail_import_at = 3;
   result = radv_amdgpu_host_composite_from_ptr(
      &g_ws.base, pointer, size, 1, 4096, &bo_out);
   CHECK(result == VK_ERROR_INVALID_EXTERNAL_HANDLE);
   CHECK(bo_out == NULL);
   CHECK(g_import_attempts == 3);
   check_failure_cleanup(2, 2, 2);

   reset_state();
   g_fail_export_at = 2;
   result = radv_amdgpu_host_composite_from_ptr(
      &g_ws.base, pointer, size, 1, 4096, &bo_out);
   CHECK(result == VK_ERROR_OUT_OF_DEVICE_MEMORY);
   CHECK(bo_out == NULL);
   CHECK(g_import_attempts == 2);
   CHECK(g_export_calls == 2);
   check_failure_cleanup(2, 1, 1);

   reset_state();
   g_fail_map_at = 2;
   g_map_fail_code = -ENOMEM;
   result = radv_amdgpu_host_composite_from_ptr(
      &g_ws.base, pointer, size, 1, 4096, &bo_out);
   CHECK(result == VK_ERROR_UNKNOWN);
   CHECK(bo_out == NULL);
   CHECK(g_import_attempts == 2);
   check_failure_cleanup(2, 1, 1);
   CHECK(g_map_attempts == 2);

   reset_state();
   result = radv_amdgpu_host_composite_from_ptr(
      &g_ws.base, pointer, 2 * 4096, 1, 4096, &bo_out);
   CHECK(result == VK_SUCCESS);
   if (result == VK_SUCCESS && bo_out) {
      g_fail_unmap_at = 1;
      g_unmap_fail_code = -EIO;
      radv_amdgpu_winsys_bo_destroy(&g_ws.base, bo_out);
      CHECK(g_unmap_attempts == 2);
      CHECK(g_free_calls == 2);
      CHECK(g_import_live == 0);
      CHECK(g_va_free_calls == 1);
      CHECK(g_va_live == 0);
      CHECK(g_tracker.allocated_gtt == 0);
      CHECK(g_munmap_calls == 0);
   }

   reset_state();
   result = radv_amdgpu_host_composite_from_ptr(
      &g_ws.base, pointer, 4096, 1, 4096, &bo_out);
   CHECK(result == VK_SUCCESS);
   if (result == VK_SUCCESS && bo_out) {
      g_fail_free_at = 1;
      g_free_fail_code = -EIO;
      radv_amdgpu_winsys_bo_destroy(&g_ws.base, bo_out);
      CHECK(g_free_calls == 1);
      CHECK(g_import_live == 0);
      CHECK(g_va_free_calls == 1);
      CHECK(g_tracker.allocated_gtt == 0);
      CHECK(g_munmap_calls == 0);
   }
}

static void
test_mixed_kernel_permissions(void)
{
   const size_t size = 3 * 4096;
   uint8_t *cpu = mapped_cpu(size);
   CHECK(cpu != NULL);
   if (!cpu)
      return;
   memset(cpu, 0x5a, size);

   reset_state();
   g_readonly_mode = 1;
   struct radeon_winsys_bo *bo_out = NULL;
   VkResult result = radv_amdgpu_host_composite_from_ptr(
      &g_ws.base, cpu, size, 1, 4096, &bo_out);
   CHECK(result == VK_SUCCESS);
   CHECK(bo_out != NULL);
   if (result != VK_SUCCESS || !bo_out) {
      release_cpu(cpu, size);
      return;
   }

   struct radv_amdgpu_winsys_bo *bo = radv_amdgpu_winsys_bo(bo_out);
   CHECK(bo->child_count == 3);
   CHECK(!g_imports[0].readonly);
   CHECK(g_imports[1].readonly);
   CHECK(!g_imports[2].readonly);
   CHECK(g_map_record_count == 3);
   for (unsigned i = 0; i < 3; ++i) {
      CHECK(g_maps[i].requested_flags & AMDGPU_VM_PAGE_READABLE);
      CHECK(g_maps[i].requested_flags & AMDGPU_VM_PAGE_WRITEABLE);
      CHECK(g_maps[i].effective_flags & AMDGPU_VM_PAGE_READABLE);
      if (i == 1)
         CHECK(!(g_maps[i].effective_flags & AMDGPU_VM_PAGE_WRITEABLE));
      else
         CHECK(g_maps[i].effective_flags & AMDGPU_VM_PAGE_WRITEABLE);
   }

   uint8_t value = 0;
   CHECK(gpu_read8(bo->base.va + 4096, &value) && value == 0x5a);
   CHECK(!gpu_write8(bo->base.va + 4096, 0xa5));
   CHECK(cpu[4096] == 0x5a);
   CHECK(gpu_write8(bo->base.va, 0xa6));
   CHECK(cpu[0] == 0xa6);

   radv_amdgpu_winsys_bo_destroy(&g_ws.base, bo_out);
   check_destroyed(3);
   CHECK(cpu[4096] == 0x5a);
   release_cpu(cpu, size);
}

static void
test_cs_fanout_and_rejects(void)
{
   const size_t size = 3 * 4096;
   uint8_t *cpu = mapped_cpu(size);
   CHECK(cpu != NULL);
   if (!cpu)
      return;

   reset_state();
   struct radeon_winsys_bo *bo_out = NULL;
   VkResult result = radv_amdgpu_host_composite_from_ptr(
      &g_ws.base, cpu, size, 9, 4096, &bo_out);
   CHECK(result == VK_SUCCESS);
   CHECK(bo_out != NULL);
   if (result != VK_SUCCESS || !bo_out) {
      release_cpu(cpu, size);
      return;
   }
   struct radv_amdgpu_winsys_bo *bo = radv_amdgpu_winsys_bo(bo_out);

   struct radv_amdgpu_cs cs;
   memset(&cs, 0, sizeof(cs));
   cs.ws = &g_ws;
   cs.status = VK_SUCCESS;
   for (unsigned i = 0; i < ARRAY_SIZE(cs.buffer_hash_table); ++i)
      cs.buffer_hash_table[i] = -1;
   radv_amdgpu_cs_add_buffer(&cs.base, bo_out);
   CHECK(cs.status == VK_SUCCESS);
   CHECK(cs.num_buffers == bo->child_count);
   for (unsigned i = 0; i < bo->child_count; ++i) {
      CHECK(cs.handles[i].bo_handle == bo->children[i]->bo_handle);
      CHECK(cs.handles[i].bo_priority == bo->priority);
   }
   const unsigned before = cs.num_buffers;
   radv_amdgpu_cs_add_buffer(&cs.base, bo_out);
   CHECK(cs.num_buffers == before);

   int fd = -1;
   const unsigned exports_before = g_export_calls;
   CHECK(!radv_amdgpu_winsys_get_fd(&g_ws.base, bo_out, &fd));
   CHECK(fd == -1);
   CHECK(g_export_calls == exports_before);
   CHECK(radv_amdgpu_winsys_bo_map(&g_ws.base, bo_out, false, NULL) == cpu);

   free(cs.handles);
   radv_amdgpu_winsys_bo_destroy(&g_ws.base, bo_out);
   check_destroyed(3);
   release_cpu(cpu, size);
}

int
main(void)
{
   _Static_assert(RADV_DARWIN_HOST_CHUNK == (UINT64_C(64) << 20),
                  "Darwin host import chunk must remain 64 MiB");
   _Static_assert(RADV_DARWIN_HOST_CHILDREN == 2048,
                  "Darwin host import child cap must remain 2048");

   test_small_chunk_dual_view();
   test_from_ptr_boundary();
   test_synthetic_8gib_layout();
   test_child_bound();
   test_failure_rollback();
   test_mixed_kernel_permissions();
   test_cs_fanout_and_rejects();

   CHECK(g_raw2_calls == 0);
   CHECK(g_unsupported_va_ops == 0);
   CHECK(g_virtual_create_calls == 0);
   CHECK(g_virtual_destroy_calls == 0);
   CHECK(g_map_duplicates == 0);
   CHECK(g_free_failures == 0);
   printf("host_composite_test: %u checks, %u failures\n", g_checks, g_failures);
   return g_failures ? 1 : 0;
}
