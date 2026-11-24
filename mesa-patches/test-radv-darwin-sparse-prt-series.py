#!/usr/bin/env python3
"""Verify Mesa RADV Darwin patch 0028 (GFX12 sparse / PRT VM support).

Checks:
  1. Contiguous 0001..0028 patch series and unified-diff hunk count integrity.
  2. Clean `patch -p1 --fuzz=0` replay of 0001..0028 on all Darwin-added files
     plus exact post-0027 upstream context anchors (or a full Mesa archive
     when `--mesa-archive` is supplied), with zero `.rej`/`.orig` files.
  3. Bit-for-bit ABI constant parity between `src/garnet/src/GarnetNativeABI.h`
     and post-0028 `include/darwin/rdna4_native_abi.h`.
  4. Compilation and execution of the real post-0028 `ac_darwin_kext.c`,
     `ac_darwin_mock.c`, `acd_kext_test.c`, and a pure-C sparse/PRT +
     `radv_sparse_enabled()` gating harness under `-Wall -Wextra -Werror`
     (both optimized `-O2` and `-O1 -fsanitize=address,undefined`).
"""

from __future__ import annotations

import argparse
from contextlib import nullcontext
import hashlib
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tarfile


PATCH_RE = re.compile(r"^(\d{4})-.+\.patch$")

REQUIRED_ABI_CONSTANTS = (
    "RDNA4N_ABI_VERSION",
    "RDNA4N_ABI_MINOR",
    "RDNA4N_VA_OP_MAP",
    "RDNA4N_VA_OP_UNMAP",
    "RDNA4N_VA_OP_CLEAR",
    "RDNA4N_VA_OP_REPLACE",
    "RDNA4N_VM_DELAY_UPDATE",
    "RDNA4N_VM_PAGE_READABLE",
    "RDNA4N_VM_PAGE_WRITEABLE",
    "RDNA4N_VM_PAGE_EXECUTABLE",
    "RDNA4N_VM_PAGE_PRT",
    "RDNA4N_VM_MTYPE_MASK",
    "RDNA4N_VM_MTYPE_DEFAULT",
    "RDNA4N_VM_MTYPE_NC",
    "RDNA4N_VM_MTYPE_WC",
    "RDNA4N_VM_MTYPE_CC",
    "RDNA4N_VM_MTYPE_UC",
    "RDNA4N_VM_MTYPE_RW",
    "RDNA4N_VM_PAGE_NOALLOC",
    "RDNA4N_INFO_HUNG",
    "RDNA4N_INFO_S1B_POSITIVE",
    "RDNA4N_INFO_NATIVE_BOOT",
    "RDNA4N_INFO_BUFFER_OPS",
    "RDNA4N_CAP_VM_PRT",
)

# Minimal drm-uapi headers used only when running standalone without a Mesa tree
# (matches Linux/Mesa include/drm-uapi/amdgpu_drm.h layout and values).
STUB_DRM_H = """\
#ifndef _DRM_H_
#define _DRM_H_
#include <stdint.h>
typedef uint32_t __u32;
typedef int32_t  __s32;
typedef uint64_t __u64;
typedef int64_t  __s64;
#endif
"""

STUB_AMDGPU_DRM_H = """\
#ifndef __AMDGPU_DRM_H__
#define __AMDGPU_DRM_H__
#include "drm.h"

#define AMDGPU_GEM_DOMAIN_CPU       0x1
#define AMDGPU_GEM_DOMAIN_GTT       0x2
#define AMDGPU_GEM_DOMAIN_VRAM      0x4
#define AMDGPU_GEM_DOMAIN_GDS       0x8
#define AMDGPU_GEM_DOMAIN_GWS       0x10
#define AMDGPU_GEM_DOMAIN_OA        0x20
#define AMDGPU_GEM_DOMAIN_DOORBELL  0x40

#define AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED (1 << 0)
#define AMDGPU_GEM_CREATE_NO_CPU_ACCESS       (1 << 1)
#define AMDGPU_GEM_CREATE_CPU_GTT_USWC        (1 << 2)
#define AMDGPU_GEM_CREATE_VRAM_CLEARED        (1 << 3)
#define AMDGPU_GEM_CREATE_VRAM_CONTIGUOUS     (1 << 5)
#define AMDGPU_GEM_CREATE_VM_ALWAYS_VALID     (1 << 6)
#define AMDGPU_GEM_CREATE_EXPLICIT_SYNC       (1 << 7)
#define AMDGPU_GEM_CREATE_CP_MQD_GFX9         (1 << 8)
#define AMDGPU_GEM_CREATE_ENCRYPTED           (1 << 10)
#define AMDGPU_GEM_CREATE_PREEMPTIBLE         (1 << 11)
#define AMDGPU_GEM_CREATE_DISCARDABLE         (1 << 12)

struct drm_amdgpu_gem_create_in {
   __u64 bo_size;
   __u64 alignment;
   __u64 domains;
   __u64 domain_flags;
};

#define AMDGPU_CTX_OP_ALLOC_CTX    1
#define AMDGPU_CTX_OP_FREE_CTX     2
#define AMDGPU_CTX_OP_QUERY_STATE  3
#define AMDGPU_CTX_OP_QUERY_STATE2 4

struct drm_amdgpu_ctx_in {
   __u32 op;
   __u32 flags;
   __u32 ctx_id;
   __s32 priority;
};

union drm_amdgpu_ctx_out {
   struct { __u32 ctx_id; __u32 _pad; } alloc;
   struct { __u64 flags; __u32 hangs; __u32 reset_status; } state;
};

union drm_amdgpu_ctx {
   struct drm_amdgpu_ctx_in in;
   union drm_amdgpu_ctx_out out;
};

struct drm_amdgpu_bo_list_in {
   __u32 operation;
   __u32 list_handle;
   __u32 bo_number;
   __u32 bo_info_size;
   __u64 bo_info_ptr;
};

#define AMDGPU_CHUNK_ID_IB                      0x01
#define AMDGPU_CHUNK_ID_FENCE                   0x02
#define AMDGPU_CHUNK_ID_DEPENDENCIES            0x03
#define AMDGPU_CHUNK_ID_SYNCOBJ_IN              0x04
#define AMDGPU_CHUNK_ID_SYNCOBJ_OUT             0x05
#define AMDGPU_CHUNK_ID_BO_HANDLES              0x06
#define AMDGPU_CHUNK_ID_SCHEDULED_DEPENDENCIES  0x07
#define AMDGPU_CHUNK_ID_SYNCOBJ_TIMELINE_WAIT   0x08
#define AMDGPU_CHUNK_ID_SYNCOBJ_TIMELINE_SIGNAL 0x09
#define AMDGPU_CHUNK_ID_CP_GFX_SHADOW           0x0a

struct drm_amdgpu_cs_chunk {
   __u32 chunk_id;
   __u32 length_dw;
   __u64 chunk_data;
};

#define AMDGPU_IB_FLAG_CE                  (1 << 0)
#define AMDGPU_IB_FLAG_PREAMBLE            (1 << 1)
#define AMDGPU_IB_FLAG_PREEMPT             (1 << 2)
#define AMDGPU_IB_FLAG_TC_WB_NOT_INVALIDATE (1 << 3)
#define AMDGPU_IB_FLAG_RESET_GDS_MAX_WAVE_ID (1 << 4)
#define AMDGPU_IB_FLAGS_SECURE             (1 << 5)
#define AMDGPU_IB_FLAG_EMIT_MEM_SYNC       (1 << 6)

struct drm_amdgpu_cs_chunk_ib {
   __u32 _pad;
   __u32 flags;
   __u64 va_start;
   __u32 ib_bytes;
   __u32 ip_type;
   __u32 ip_instance;
   __u32 ring;
};

struct drm_amdgpu_cs_chunk_fence {
   __u32 handle;
   __u32 offset;
};

struct drm_amdgpu_cs_chunk_sem {
   __u32 handle;
};

struct drm_amdgpu_cs_chunk_syncobj {
   __u32 handle;
   __u32 flags;
   __u64 point;
};

#define AMDGPU_VA_OP_MAP      1
#define AMDGPU_VA_OP_UNMAP    2
#define AMDGPU_VA_OP_CLEAR    3
#define AMDGPU_VA_OP_REPLACE  4

#define AMDGPU_VM_DELAY_UPDATE     (1 << 0)
#define AMDGPU_VM_PAGE_READABLE    (1 << 1)
#define AMDGPU_VM_PAGE_WRITEABLE   (1 << 2)
#define AMDGPU_VM_PAGE_EXECUTABLE  (1 << 3)
#define AMDGPU_VM_PAGE_PRT         (1 << 4)
#define AMDGPU_VM_MTYPE_MASK       (0xf << 5)
#define AMDGPU_VM_MTYPE_DEFAULT    (0 << 5)
#define AMDGPU_VM_MTYPE_NC         (1 << 5)
#define AMDGPU_VM_MTYPE_WC         (2 << 5)
#define AMDGPU_VM_MTYPE_CC         (3 << 5)
#define AMDGPU_VM_MTYPE_UC         (4 << 5)
#define AMDGPU_VM_MTYPE_RW         (5 << 5)
#define AMDGPU_VM_PAGE_NOALLOC     (1 << 9)

struct drm_amdgpu_gem_va {
   __u32 handle;
   __u32 _pad;
   __u32 operation;
   __u32 flags;
   __u64 va_address;
   __u64 offset_in_bo;
   __u64 map_size;
   __u64 vm_timeline_point;
   __u32 vm_timeline_syncobj_out;
   __u32 num_syncobj_handles;
   __u64 input_fence_syncobj_handles;
};

#define AMDGPU_HW_IP_GFX 0
#endif
"""

HOST_HARNESS_C = r"""
#include "ac_darwin_kext.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int acd_kext_open_iokit(struct acd_kext **out) { (void)out; return -ENODEV; }

/* ---- Minimal stubs to compile & exercise the exact patched radv_sparse_enabled() ---- */
#define HAVE_AMDGPU_DARWIN 1
#define RADV_DRM_DEVICE_AMDGPU 0
#define RADV_DRM_DEVICE_AMDGPU_DARWIN 3
#define RADV_QUEUE_DISABLE_SPARSE (1u << 0)

typedef struct ac_drm_device {
   bool is_darwin;
   bool has_vm_prt;
} ac_drm_device;

struct radv_instance {
   uint32_t queue_disable_flags;
};

struct radv_physical_device {
   const struct radv_instance *instance;
   int drm_device_type;
   int drm_device_mtx;
   ac_drm_device *drm_device;
   ac_drm_device *lazy_device;
};

static unsigned g_locks, g_unlocks, g_opens;

static void simple_mtx_lock(int *m) { (void)m; g_locks++; }
static void simple_mtx_unlock(int *m) { (void)m; g_unlocks++; }
static const struct radv_instance *radv_physical_device_instance(const struct radv_physical_device *pdev)
{
   return pdev->instance;
}
static void radv_create_drm_device_locked(struct radv_physical_device *pdev)
{
   g_opens++;
   if (!pdev->drm_device)
      pdev->drm_device = pdev->lazy_device;
}
static bool ac_drm_darwin_has_vm_prt(ac_drm_device *dev)
{
   return dev && dev->is_darwin && dev->has_vm_prt;
}

#include "radv_sparse_enabled_actual.inc"

/* ---- Minimal stubs to compile & exercise verbatim acd_cmd_gem_va() & acd_device_has_vm_prt() ---- */
#define DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL        (1u << 0)
#define DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT (1u << 1)

struct util_sync_provider {
   int (*wait)(struct util_sync_provider *p, uint32_t *handles, unsigned num_handles,
               int64_t timeout_nsec, unsigned flags, uint32_t *first_signaled);
   int (*signal)(struct util_sync_provider *p, const uint32_t *handles, uint32_t handle_count);
   int (*timeline_signal)(struct util_sync_provider *p, const uint32_t *handles, uint64_t *points,
                          uint32_t handle_count);
};

struct acd_core {
   bool has_vm_prt;
   struct acd_kext *kext;
};

struct acd_device {
   bool host_bo;
   struct acd_core *core;
   struct util_sync_provider *sync;
};
typedef struct acd_device *acd_device_handle;

#include "acd_drm_sparse_actual.inc"

/* ---- Test assertions ---- */
static unsigned g_checks, g_failures;
#define CHECK(c)                                                                                   \
   do {                                                                                            \
      g_checks++;                                                                                  \
      if (!(c)) {                                                                                  \
         g_failures++;                                                                             \
         fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                              \
      }                                                                                            \
   } while (0)

struct fake_tx {
   uint32_t info_flags;
   unsigned gemva_calls;
   unsigned info_calls;
   struct rdna4n_gem_va last_va;
};

static int
fake_tx_call(void *ctx, uint32_t sel, const uint64_t *sin, uint32_t nsin, const void *stin,
             size_t stinsz, uint64_t *sout, uint32_t *nsout, void *stout, size_t *stoutsz)
{
   (void)sin; (void)nsin; (void)sout; (void)nsout;
   struct fake_tx *f = ctx;
   if (sel == ACD_SEL_QUERY_INFO) {
      f->info_calls++;
      struct rdna4n_info inf;
      memset(&inf, 0, sizeof(inf));
      inf.abi_version = RDNA4N_ABI_VERSION;
      inf.flags = f->info_flags;
      memcpy(stout, &inf, sizeof(inf));
      *stoutsz = sizeof(inf);
      return 0;
   }
   if (sel == ACD_SEL_GEM_VA) {
      f->gemva_calls++;
      if (stinsz == sizeof(f->last_va) && stin)
         memcpy(&f->last_va, stin, sizeof(f->last_va));
      return 0;
   }
   return (int)ACD_KR_UNSUPPORTED;
}

static void
test_translate_and_gating(void)
{
   const uint64_t hi_va = 0xffff800100000000ull;
   const uint64_t lo_va = 0x0000000010000000ull;
   struct rdna4n_gem_va out;

   /* Legacy MAP / UNMAP with handle != 0 succeed on both old (has_vm_prt=false) and new kernels. */
   struct drm_amdgpu_gem_va map_bo = {
      .handle = 5,
      .operation = AMDGPU_VA_OP_MAP,
      .flags = AMDGPU_VM_PAGE_READABLE | AMDGPU_VM_PAGE_WRITEABLE | AMDGPU_VM_MTYPE_UC,
      .va_address = hi_va,
      .offset_in_bo = 0x2000,
      .map_size = 0x10000,
      .vm_timeline_point = 42,
      .vm_timeline_syncobj_out = 9,
      .num_syncobj_handles = 1,
      .input_fence_syncobj_handles = 0xdeadbeefull,
   };
   CHECK(acd_kext_translate_gem_va(&map_bo, false, &out) == 0);
   CHECK(out.handle == 5 && out.operation == RDNA4N_VA_OP_MAP && out.flags == map_bo.flags);
   CHECK(out.va_address == hi_va && out.offset_in_bo == 0x2000 && out.map_size == 0x10000);
   CHECK(out._pad == 0 && out.vm_timeline_point == 0 && out.vm_timeline_syncobj_out == 0 &&
         out.num_syncobj_handles == 0 && out.input_fence_syncobj_handles == 0);

   struct drm_amdgpu_gem_va unmap_bo = {
      .handle = 5, .operation = AMDGPU_VA_OP_UNMAP, .flags = 0,
      .va_address = lo_va, .offset_in_bo = 0, .map_size = 0x10000,
   };
   CHECK(acd_kext_translate_gem_va(&unmap_bo, false, &out) == 0);
   CHECK(out.operation == RDNA4N_VA_OP_UNMAP && out.handle == 5);

   /* PRT / CLEAR / REPLACE must be rejected with -EOPNOTSUPP when has_vm_prt == false. */
   struct drm_amdgpu_gem_va prt_map = {
      .handle = 0, .operation = AMDGPU_VA_OP_MAP, .flags = AMDGPU_VM_PAGE_PRT,
      .va_address = hi_va, .offset_in_bo = 0, .map_size = 0x40000,
   };
   struct drm_amdgpu_gem_va prt_unmap = {
      .handle = 0, .operation = AMDGPU_VA_OP_UNMAP, .flags = AMDGPU_VM_PAGE_PRT,
      .va_address = hi_va, .offset_in_bo = 0, .map_size = 0x40000,
   };
   struct drm_amdgpu_gem_va prt_replace = {
      .handle = 0, .operation = AMDGPU_VA_OP_REPLACE,
      .flags = AMDGPU_VM_PAGE_PRT | AMDGPU_VM_PAGE_READABLE | AMDGPU_VM_PAGE_WRITEABLE,
      .va_address = hi_va + 0x10000, .offset_in_bo = 0, .map_size = 0x10000,
   };
   struct drm_amdgpu_gem_va bo_replace = {
      .handle = 7, .operation = AMDGPU_VA_OP_REPLACE,
      .flags = AMDGPU_VM_PAGE_READABLE | AMDGPU_VM_PAGE_WRITEABLE | AMDGPU_VM_PAGE_NOALLOC,
      .va_address = hi_va + 0x10000, .offset_in_bo = 0x10000, .map_size = 0x10000,
   };
   struct drm_amdgpu_gem_va clear_va = {
      .handle = 0, .operation = AMDGPU_VA_OP_CLEAR, .flags = 0,
      .va_address = hi_va, .offset_in_bo = 0, .map_size = 0x40000,
   };
   struct drm_amdgpu_gem_va clear_prt_va = {
      .handle = 0, .operation = AMDGPU_VA_OP_CLEAR, .flags = AMDGPU_VM_PAGE_PRT,
      .va_address = hi_va, .offset_in_bo = 0, .map_size = 0x40000,
   };

   CHECK(acd_kext_translate_gem_va(&prt_map, false, &out) == -EOPNOTSUPP);
   CHECK(acd_kext_translate_gem_va(&prt_unmap, false, &out) == -EOPNOTSUPP);
   CHECK(acd_kext_translate_gem_va(&prt_replace, false, &out) == -EOPNOTSUPP);
   CHECK(acd_kext_translate_gem_va(&bo_replace, false, &out) == -EOPNOTSUPP);
   CHECK(acd_kext_translate_gem_va(&clear_va, false, &out) == -EOPNOTSUPP);
   CHECK(acd_kext_translate_gem_va(&clear_prt_va, false, &out) == -EOPNOTSUPP);

   /* With has_vm_prt == true (ABI 1.13+): all valid sparse/PRT ops translate cleanly. */
   CHECK(acd_kext_translate_gem_va(&prt_map, true, &out) == 0);
   CHECK(out.operation == RDNA4N_VA_OP_MAP && out.handle == 0 && out.flags == RDNA4N_VM_PAGE_PRT &&
         out.offset_in_bo == 0 && out.map_size == 0x40000);
   CHECK(acd_kext_translate_gem_va(&prt_unmap, true, &out) == 0);
   CHECK(out.operation == RDNA4N_VA_OP_UNMAP && out.handle == 0 && out.flags == RDNA4N_VM_PAGE_PRT);
   CHECK(acd_kext_translate_gem_va(&prt_replace, true, &out) == 0);
   CHECK(out.operation == RDNA4N_VA_OP_REPLACE && out.handle == 0 &&
         (out.flags & RDNA4N_VM_PAGE_PRT) == RDNA4N_VM_PAGE_PRT);
   CHECK(acd_kext_translate_gem_va(&bo_replace, true, &out) == 0);
   CHECK(out.operation == RDNA4N_VA_OP_REPLACE && out.handle == 7 && out.offset_in_bo == 0x10000 &&
         (out.flags & RDNA4N_VM_PAGE_NOALLOC) == RDNA4N_VM_PAGE_NOALLOC);
   CHECK(acd_kext_translate_gem_va(&clear_va, true, &out) == 0);
   CHECK(out.operation == RDNA4N_VA_OP_CLEAR && out.handle == 0 && out.flags == 0);
   CHECK(acd_kext_translate_gem_va(&clear_prt_va, true, &out) == 0);
   CHECK(out.operation == RDNA4N_VA_OP_CLEAR && out.handle == 0 && out.flags == RDNA4N_VM_PAGE_PRT);

   /* Invalid flags, MTYPEs, handle/offset combos, alignment, and VA bounds rejected with -EINVAL. */
   CHECK(acd_kext_translate_gem_va(NULL, true, &out) == -EINVAL);
   CHECK(acd_kext_translate_gem_va(&prt_map, true, NULL) == -EINVAL);

   struct drm_amdgpu_gem_va bad = prt_map;
   bad.operation = 0; CHECK(acd_kext_translate_gem_va(&bad, true, &out) == -EINVAL);
   bad.operation = 5; CHECK(acd_kext_translate_gem_va(&bad, true, &out) == -EINVAL);

   /* Unknown flag bits */
   for (unsigned bit = 10; bit < 32; bit++) {
      bad = bo_replace;
      bad.flags |= (1u << bit);
      CHECK(acd_kext_translate_gem_va(&bad, true, &out) == -EINVAL);
   }
   /* Invalid MTYPE encodings 6..15 */
   for (unsigned mtype = 6; mtype <= 15; mtype++) {
      bad = bo_replace;
      bad.flags = AMDGPU_VM_PAGE_READABLE | (mtype << 5);
      CHECK(acd_kext_translate_gem_va(&bad, true, &out) == -EINVAL);
   }
   /* Valid MTYPE encodings 0..5 */
   for (unsigned mtype = 0; mtype <= 5; mtype++) {
      bad = bo_replace;
      bad.flags = AMDGPU_VM_PAGE_READABLE | (mtype << 5);
      CHECK(acd_kext_translate_gem_va(&bad, true, &out) == 0);
   }

   /* PRT with nonzero handle or nonzero offset */
   bad = prt_map; bad.handle = 1; CHECK(acd_kext_translate_gem_va(&bad, true, &out) == -EINVAL);
   bad = prt_map; bad.offset_in_bo = 4096; CHECK(acd_kext_translate_gem_va(&bad, true, &out) == -EINVAL);
   bad = prt_replace; bad.handle = 2; CHECK(acd_kext_translate_gem_va(&bad, true, &out) == -EINVAL);
   bad = prt_unmap; bad.handle = 2; CHECK(acd_kext_translate_gem_va(&bad, true, &out) == -EINVAL);
   bad = clear_prt_va; bad.handle = 2; CHECK(acd_kext_translate_gem_va(&bad, true, &out) == -EINVAL);
   bad = clear_va; bad.offset_in_bo = 4096; CHECK(acd_kext_translate_gem_va(&bad, true, &out) == -EINVAL);

   /* Non-PRT MAP/REPLACE/UNMAP with handle == 0 */
   bad = map_bo; bad.handle = 0; CHECK(acd_kext_translate_gem_va(&bad, true, &out) == -EINVAL);
   bad = map_bo; bad.handle = 0; CHECK(acd_kext_translate_gem_va(&bad, false, &out) == -EINVAL);
   bad = bo_replace; bad.handle = 0; CHECK(acd_kext_translate_gem_va(&bad, true, &out) == -EINVAL);
   bad = unmap_bo; bad.handle = 0; CHECK(acd_kext_translate_gem_va(&bad, true, &out) == -EINVAL);

   /* Alignment, zero size, canonical bounds, seam straddle, overflow */
   bad = prt_map; bad.va_address += 4; CHECK(acd_kext_translate_gem_va(&bad, true, &out) == -EINVAL);
   bad = prt_map; bad.map_size = 0; CHECK(acd_kext_translate_gem_va(&bad, true, &out) == -EINVAL);
   bad = prt_map; bad.map_size = 4095; CHECK(acd_kext_translate_gem_va(&bad, true, &out) == -EINVAL);
   bad = bo_replace; bad.offset_in_bo = 128; CHECK(acd_kext_translate_gem_va(&bad, true, &out) == -EINVAL);
   bad = prt_map; bad.va_address = 0x8000ull; CHECK(acd_kext_translate_gem_va(&bad, true, &out) == -EINVAL);
   bad = prt_map; bad.va_address = 0x0000800000000000ull; CHECK(acd_kext_translate_gem_va(&bad, true, &out) == -EINVAL);
   bad = prt_map; bad.va_address = 0x00007ffffffff000ull; bad.map_size = 0x2000;
   CHECK(acd_kext_translate_gem_va(&bad, true, &out) == -EINVAL);
   bad = prt_map; bad.va_address = 0xfffffffffffff000ull; bad.map_size = 0x2000;
   CHECK(acd_kext_translate_gem_va(&bad, true, &out) == -EINVAL);

   /* Fake transport end-to-end gating: old kernel vs ABI 1.13 kernel */
   struct fake_tx f_old = {.info_flags = RDNA4N_INFO_S1B_POSITIVE | RDNA4N_INFO_NATIVE_BOOT};
   struct acd_kext_ops ops = {.call = fake_tx_call};
   struct acd_kext *k_old = acd_kext_create(&ops, &f_old);
   CHECK(k_old != NULL);
   CHECK(!acd_kext_has_vm_prt(k_old));
   CHECK(f_old.info_calls == 1);
   CHECK(!acd_kext_has_vm_prt(k_old));
   CHECK(f_old.info_calls == 1); /* cached */
   CHECK(acd_kext_gem_va(k_old, &prt_map) == -EOPNOTSUPP && f_old.gemva_calls == 0);
   CHECK(acd_kext_gem_va(k_old, &bo_replace) == -EOPNOTSUPP && f_old.gemva_calls == 0);
   CHECK(acd_kext_gem_va(k_old, &clear_va) == -EOPNOTSUPP && f_old.gemva_calls == 0);
   CHECK(acd_kext_gem_va(k_old, &map_bo) == 0 && f_old.gemva_calls == 1);
   acd_kext_destroy(k_old);

   struct fake_tx f_new = {.info_flags = RDNA4N_INFO_S1B_POSITIVE | RDNA4N_INFO_NATIVE_BOOT | RDNA4N_CAP_VM_PRT};
   struct acd_kext *k_new = acd_kext_create(&ops, &f_new);
   CHECK(k_new != NULL);
   CHECK(acd_kext_has_vm_prt(k_new));
   CHECK(acd_kext_gem_va(k_new, &prt_map) == 0 && f_new.gemva_calls == 1 &&
         f_new.last_va.operation == RDNA4N_VA_OP_MAP && f_new.last_va.flags == RDNA4N_VM_PAGE_PRT);
   CHECK(acd_kext_gem_va(k_new, &bo_replace) == 0 && f_new.gemva_calls == 2 &&
         f_new.last_va.operation == RDNA4N_VA_OP_REPLACE && f_new.last_va.handle == 7);
   CHECK(acd_kext_gem_va(k_new, &clear_va) == 0 && f_new.gemva_calls == 3 &&
         f_new.last_va.operation == RDNA4N_VA_OP_CLEAR && f_new.last_va.handle == 0);
   acd_kext_destroy(k_new);
}

static void
test_mock_prt_interval_lifecycle(void)
{
   struct acd_kext *k = NULL;
   CHECK(acd_mock_open(&k) == 0 && k != NULL);

   struct acd_hello h;
   CHECK(acd_kext_hello(k, &h) == 0 && h.abi == RDNA4N_ABI_VERSION && h.minor == 13u);
   struct rdna4n_info info;
   CHECK(acd_kext_query_info(k, &info) == 0);
   CHECK((info.flags & RDNA4N_CAP_VM_PRT) != 0);
   CHECK(acd_kext_has_vm_prt(k));

   struct rdna4n_ctx ctx_in = {.op = AMDGPU_CTX_OP_ALLOC_CTX}, ctx_out;
   CHECK(acd_kext_ctx(k, &ctx_in, &ctx_out) == 0 && ctx_out.op >= 1);
   const uint32_t ctx_id = ctx_out.op;

   struct rdna4n_gem_create_in ci = {.bo_size = 0x40000, .alignment = 0x10000,
                                     .domains = AMDGPU_GEM_DOMAIN_VRAM};
   struct acd_bo_created bo1, bo2;
   CHECK(acd_kext_bo_create(k, &ci, &bo1) == 0);
   CHECK(acd_kext_bo_create(k, &ci, &bo2) == 0);

   const uint64_t base = 0xffff800100000000ull;
   /* 1. Map 256 KiB PRT region */
   struct drm_amdgpu_gem_va va = {
      .handle = 0, .operation = AMDGPU_VA_OP_MAP, .flags = AMDGPU_VM_PAGE_PRT,
      .va_address = base, .offset_in_bo = 0, .map_size = 0x40000,
   };
   CHECK(acd_kext_gem_va(k, &va) == 0);

   /* Submitting an IB pointing at PRT pages must fail */
   struct rdna4n_cs_ib ib = {.va_start = base + 0x10000, .ib_bytes = 64, .ip_type = 0};
   struct acd_cs_plan plan = {.ctx_id = ctx_id, .ibs = &ib, .n_ibs = 1};
   uint64_t seq = 0;
   CHECK(acd_kext_submit(k, &plan, &seq) == -EINVAL);

   /* 2. REPLACE middle [64K, 128K) with bo1 (splits PRT into 3 intervals) */
   va = (struct drm_amdgpu_gem_va){
      .handle = bo1.handle, .operation = AMDGPU_VA_OP_REPLACE,
      .flags = AMDGPU_VM_PAGE_READABLE | AMDGPU_VM_PAGE_WRITEABLE | AMDGPU_VM_PAGE_EXECUTABLE,
      .va_address = base + 0x10000, .offset_in_bo = 0, .map_size = 0x10000,
   };
   CHECK(acd_kext_gem_va(k, &va) == 0);
   CHECK(acd_kext_submit(k, &plan, &seq) == 0 && seq == 1);
   /* Unmapped/PRT neighbors still cannot serve IBs */
   ib.va_start = base;
   CHECK(acd_kext_submit(k, &plan, &seq) == -EINVAL);
   ib.va_start = base + 0x20000;
   CHECK(acd_kext_submit(k, &plan, &seq) == -EINVAL);

   /* 3. REPLACE [32K, 160K) with bo2 (trims left PRT, removes bo1, trims right PRT) */
   va = (struct drm_amdgpu_gem_va){
      .handle = bo2.handle, .operation = AMDGPU_VA_OP_REPLACE,
      .flags = AMDGPU_VM_PAGE_READABLE | AMDGPU_VM_PAGE_WRITEABLE | AMDGPU_VM_PAGE_EXECUTABLE,
      .va_address = base + 0x8000, .offset_in_bo = 0x8000, .map_size = 0x20000,
   };
   CHECK(acd_kext_gem_va(k, &va) == 0);
   ib.va_start = base + 0x8000;
   CHECK(acd_kext_submit(k, &plan, &seq) == 0 && seq == 2);

   /* 4. REPLACE [64K, 128K) back to PRT (middle-splits bo2!) */
   va = (struct drm_amdgpu_gem_va){
      .handle = 0, .operation = AMDGPU_VA_OP_REPLACE, .flags = AMDGPU_VM_PAGE_PRT,
      .va_address = base + 0x10000, .offset_in_bo = 0, .map_size = 0x10000,
   };
   CHECK(acd_kext_gem_va(k, &va) == 0);
   ib.va_start = base + 0x10000;
   CHECK(acd_kext_submit(k, &plan, &seq) == -EINVAL);
   ib.va_start = base + 0x8000;
   CHECK(acd_kext_submit(k, &plan, &seq) == 0 && seq == 3);
   ib.va_start = base + 0x20000;
   CHECK(acd_kext_submit(k, &plan, &seq) == 0 && seq == 4);

   /* 5. CLEAR entire [0, 256K), then verify MAP succeeds with zero residual overlap */
   va = (struct drm_amdgpu_gem_va){
      .handle = 0, .operation = AMDGPU_VA_OP_CLEAR, .flags = 0,
      .va_address = base, .offset_in_bo = 0, .map_size = 0x40000,
   };
   CHECK(acd_kext_gem_va(k, &va) == 0);
   ib.va_start = base + 0x8000;
   CHECK(acd_kext_submit(k, &plan, &seq) == -EINVAL);

   va = (struct drm_amdgpu_gem_va){
      .handle = 0, .operation = AMDGPU_VA_OP_MAP, .flags = AMDGPU_VM_PAGE_PRT,
      .va_address = base, .offset_in_bo = 0, .map_size = 0x40000,
   };
   CHECK(acd_kext_gem_va(k, &va) == 0);
   va.operation = AMDGPU_VA_OP_UNMAP;
   CHECK(acd_kext_gem_va(k, &va) == 0);

   CHECK(acd_kext_bo_free(k, bo2.handle) == 0);
   CHECK(acd_kext_bo_free(k, bo1.handle) == 0);
   acd_kext_destroy(k);
}

static void
test_radv_sparse_enabled_gating(void)
{
   struct radv_instance inst = {.queue_disable_flags = 0};
   ac_drm_device old_dev = {.is_darwin = true, .has_vm_prt = false};
   ac_drm_device new_dev = {.is_darwin = true, .has_vm_prt = true};

   /* Old ABI <= 1.12 kernel -> radv_sparse_enabled returns false */
   struct radv_physical_device pdev = {
      .instance = &inst,
      .drm_device_type = RADV_DRM_DEVICE_AMDGPU_DARWIN,
      .lazy_device = &old_dev,
   };
   g_locks = g_unlocks = g_opens = 0;
   CHECK(radv_sparse_enabled(&pdev) == false);
   CHECK(g_locks == 1 && g_unlocks == 1 && g_opens == 1);

   /* ABI 1.13 kernel with RDNA4N_CAP_VM_PRT -> radv_sparse_enabled returns true */
   pdev.drm_device = NULL;
   pdev.lazy_device = &new_dev;
   CHECK(radv_sparse_enabled(&pdev) == true);

   /* Respects Mesa's RADV_QUEUE_DISABLE_SPARSE opt-out even when kernel advertises PRT */
   inst.queue_disable_flags = RADV_QUEUE_DISABLE_SPARSE;
   CHECK(radv_sparse_enabled(&pdev) == false);
   inst.queue_disable_flags = 0;

   /* Missing drm_device (HOST_BO / open failure) -> false */
   pdev.drm_device = NULL;
   pdev.lazy_device = NULL;
   CHECK(radv_sparse_enabled(&pdev) == false);

   /* Non-Darwin device skips Darwin lock/query and follows Mesa flags */
   pdev.drm_device_type = RADV_DRM_DEVICE_AMDGPU;
   unsigned before_opens = g_opens;
   CHECK(radv_sparse_enabled(&pdev) == true && g_opens == before_opens);
   inst.queue_disable_flags = RADV_QUEUE_DISABLE_SPARSE;
   CHECK(radv_sparse_enabled(&pdev) == false && g_opens == before_opens);
}

struct fake_sync {
   struct util_sync_provider base;
   unsigned wait_calls;
   unsigned bin_sig_calls;
   unsigned tl_sig_calls;
   int wait_errno;
   uint32_t last_sig_handle;
   uint64_t last_sig_point;
};

static int
fake_sync_wait(struct util_sync_provider *p, uint32_t *handles, unsigned num_handles,
               int64_t timeout_nsec, unsigned flags, uint32_t *first_signaled)
{
   (void)handles; (void)num_handles; (void)timeout_nsec; (void)flags; (void)first_signaled;
   struct fake_sync *s = (struct fake_sync *)p;
   s->wait_calls++;
   if (s->wait_errno) {
      errno = s->wait_errno;
      return -1;
   }
   return 0;
}

static int
fake_sync_signal(struct util_sync_provider *p, const uint32_t *handles, uint32_t handle_count)
{
   struct fake_sync *s = (struct fake_sync *)p;
   s->bin_sig_calls += handle_count;
   if (handle_count)
      s->last_sig_handle = handles[0];
   return 0;
}

static int
fake_sync_tl_signal(struct util_sync_provider *p, const uint32_t *handles, uint64_t *points,
                    uint32_t handle_count)
{
   struct fake_sync *s = (struct fake_sync *)p;
   s->tl_sig_calls += handle_count;
   if (handle_count) {
      s->last_sig_handle = handles[0];
      s->last_sig_point = points[0];
   }
   return 0;
}

static void
test_acd_cmd_gem_va_and_cap(void)
{
   struct fake_tx f = {.info_flags = RDNA4N_INFO_S1B_POSITIVE | RDNA4N_INFO_NATIVE_BOOT};
   struct acd_kext_ops ops = {.call = fake_tx_call};
   struct acd_kext *k = acd_kext_create(&ops, &f);
   struct fake_sync sync = {
      .base = {.wait = fake_sync_wait, .signal = fake_sync_signal, .timeline_signal = fake_sync_tl_signal},
   };
   struct acd_core core = {.has_vm_prt = false, .kext = k};
   struct acd_device dev = {.host_bo = false, .core = &core, .sync = &sync.base};

   CHECK(!acd_device_has_vm_prt(NULL));
   CHECK(!acd_device_has_vm_prt(&dev));
   core.has_vm_prt = true;
   CHECK(acd_device_has_vm_prt(&dev));
   dev.host_bo = true;
   CHECK(!acd_device_has_vm_prt(&dev));
   dev.host_bo = false;

   /* Old kernel (!has_vm_prt) rejects PRT REPLACE fast BEFORE waiting on input fences */
   core.has_vm_prt = false;
   uint32_t in_fence = 11;
   struct drm_amdgpu_gem_va va = {
      .handle = 0, .operation = AMDGPU_VA_OP_REPLACE, .flags = AMDGPU_VM_PAGE_PRT,
      .va_address = 0xffff800100000000ull, .offset_in_bo = 0, .map_size = 0x10000,
      .vm_timeline_point = 7, .vm_timeline_syncobj_out = 21,
      .num_syncobj_handles = 1, .input_fence_syncobj_handles = (uint64_t)(uintptr_t)&in_fence,
   };
   CHECK(acd_cmd_gem_va(&dev, &va) == -EOPNOTSUPP);
   CHECK(sync.wait_calls == 0 && sync.tl_sig_calls == 0 && f.gemva_calls == 0);
   acd_kext_destroy(k);

   /* ABI 1.13 kernel: waits on input fence, executes REPLACE PRT, signals timeline point */
   f = (struct fake_tx){.info_flags = RDNA4N_INFO_S1B_POSITIVE | RDNA4N_INFO_NATIVE_BOOT | RDNA4N_CAP_VM_PRT};
   k = acd_kext_create(&ops, &f);
   core.kext = k;
   core.has_vm_prt = true;
   CHECK(acd_cmd_gem_va(&dev, &va) == 0);
   CHECK(sync.wait_calls == 1 && f.gemva_calls == 1 && sync.tl_sig_calls == 1 &&
         sync.last_sig_handle == 21 && sync.last_sig_point == 7);

   /* Binary out syncobj (point == 0) calls signal(), not timeline_signal() */
   va.vm_timeline_point = 0;
   va.vm_timeline_syncobj_out = 22;
   va.num_syncobj_handles = 0;
   va.input_fence_syncobj_handles = 0;
   CHECK(acd_cmd_gem_va(&dev, &va) == 0);
   CHECK(sync.bin_sig_calls == 1 && sync.last_sig_handle == 22 && sync.tl_sig_calls == 1);

   /* Nonzero num_syncobj_handles with NULL pointer rejected with -EINVAL */
   va.num_syncobj_handles = 1;
   va.input_fence_syncobj_handles = 0;
   CHECK(acd_cmd_gem_va(&dev, &va) == -EINVAL);

   /* Wait failure propagates -errno and does not call kernel */
   unsigned prev_gemva = f.gemva_calls;
   va.input_fence_syncobj_handles = (uint64_t)(uintptr_t)&in_fence;
   sync.wait_errno = ECANCELED;
   CHECK(acd_cmd_gem_va(&dev, &va) == -ECANCELED);
   CHECK(f.gemva_calls == prev_gemva);
   acd_kext_destroy(k);
}

int
main(void)
{
   test_translate_and_gating();
   test_mock_prt_interval_lifecycle();
   test_radv_sparse_enabled_gating();
   test_acd_cmd_gem_va_and_cap();
   printf("radv_darwin_sparse_prt_host_test: %u checks, %u failures\n", g_checks, g_failures);
   return g_failures != 0;
}
"""


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(
    command: list[str], *, stdin: Path | None = None, env: dict[str, str] | None = None
) -> subprocess.CompletedProcess[str]:
    print("+", " ".join(command), flush=True)
    with stdin.open("rb") if stdin else nullcontext(None) as input_file:
        result = subprocess.run(
            command,
            stdin=input_file,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            env=env,
        )
    if result.stdout:
        print(result.stdout, end="", flush=True)
    if result.returncode:
        raise SystemExit(f"command failed ({result.returncode}): {' '.join(command)}")
    return result


def patch_files(patch_dir: Path, last_patch: str) -> list[Path]:
    if not re.fullmatch(r"\d{4}", last_patch):
        raise SystemExit(f"invalid final patch number: {last_patch}")
    patches: list[tuple[str, Path]] = []
    for path in patch_dir.iterdir():
        match = PATCH_RE.match(path.name)
        if match and match.group(1) <= last_patch:
            patches.append((match.group(1), path))
    patches.sort()
    actual = [number for number, _ in patches]
    expected = [f"{number:04d}" for number in range(1, int(last_patch) + 1)]
    if actual != expected:
        raise SystemExit(
            f"patch series is not contiguous through {last_patch}: "
            f"expected {expected[0]}..{expected[-1]}, got {actual}"
        )
    return [path for _, path in patches]


def split_patch_sections(text: str) -> list[tuple[bool, str, str]]:
    lines = text.splitlines(keepends=True)
    sections: list[str] = []
    cur: list[str] = []
    for line in lines:
        if line.startswith("diff --git "):
            if cur:
                sections.append("".join(cur))
            cur = [line]
        elif line.startswith("--- ") and cur and any(x.startswith("+++ ") for x in cur):
            sections.append("".join(cur))
            cur = [line]
        else:
            cur.append(line)
    if cur:
        sections.append("".join(cur))
    parsed: list[tuple[bool, str, str]] = []
    for sec in sections:
        m_old = re.search(r"^--- (.+)$", sec, re.M)
        m_new = re.search(r"^\+\+\+ (.+)$", sec, re.M)
        if not (m_old and m_new):
            continue
        old_p = m_old.group(1).split("\t")[0].strip()
        new_p = m_new.group(1).split("\t")[0].strip()
        rel = new_p[2:] if new_p.startswith("b/") else new_p
        parsed.append((old_p == "/dev/null", rel, sec))
    return parsed


def validate_patch_hunks(patch_path: Path) -> int:
    lines = patch_path.read_text(encoding="utf-8").splitlines()
    i = 0
    hunks = 0
    cur_file = "<none>"
    while i < len(lines):
        line = lines[i]
        if line.startswith("+++ "):
            cur_file = line[4:]
        elif line.startswith("@@ "):
            m = re.match(r"^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@", line)
            if not m:
                raise SystemExit(f"{patch_path.name}:{i+1}: malformed hunk header: {line}")
            o_cnt = int(m.group(2)) if m.group(2) is not None else 1
            n_cnt = int(m.group(4)) if m.group(4) is not None else 1
            j = i + 1
            ao = an = 0
            while j < len(lines) and not lines[j].startswith(
                ("diff --git ", "--- ", "+++ ", "@@ ")
            ):
                if lines[j].startswith(" "):
                    ao += 1
                    an += 1
                elif lines[j].startswith("-"):
                    ao += 1
                elif lines[j].startswith("+"):
                    an += 1
                elif lines[j].startswith("\\"):
                    pass
                else:
                    raise SystemExit(
                        f"{patch_path.name}:{j+1}: invalid hunk line in {cur_file}: {lines[j]!r}"
                    )
                j += 1
            if ao != o_cnt or an != n_cnt:
                raise SystemExit(
                    f"{patch_path.name}:{i+1}: hunk count mismatch in {cur_file}: "
                    f"header -{o_cnt}/+{n_cnt} vs actual -{ao}/+{an}"
                )
            hunks += 1
            i = j
            continue
        i += 1
    if hunks == 0:
        raise SystemExit(f"{patch_path.name}: contains zero hunks")
    return hunks


def parse_defines(header: Path) -> dict[str, str]:
    defs: dict[str, str] = {}
    for line in header.read_text(encoding="utf-8").splitlines():
        m = re.match(r"^\s*#define\s+(RDNA4N_[A-Z0-9_]+)\s+(.+?)(?:/\*.*)?$", line)
        if m:
            defs[m.group(1)] = m.group(2).strip()
    return defs


def check_abi_parity(kernel_abi: Path, mesa_abi: Path) -> None:
    k_defs = parse_defines(kernel_abi)
    m_defs = parse_defines(mesa_abi)
    mismatches: list[str] = []
    for name in REQUIRED_ABI_CONSTANTS:
        kv = k_defs.get(name)
        mv = m_defs.get(name)
        if kv is None or mv is None or kv != mv:
            mismatches.append(f"{name}: kernel={kv!r} mesa={mv!r}")
    if mismatches:
        raise SystemExit("ABI constant mismatch:\n  " + "\n  ".join(mismatches))
    print(f"ABI_PARITY_PASS ({len(REQUIRED_ABI_CONSTANTS)} constants match GarnetNativeABI.h bit-for-bit)")


def build_upstream_anchors(patch_dir: Path, tree: Path) -> None:
    """Build exact post-0027 synthetic anchors for upstream files touched by 0028."""
    # 1. ac_linux_drm.h (post-0004 context at line 438)
    ac_h_lines = ["/* pad */"] * 437 + [
        "#ifdef HAVE_AMDGPU_DARWIN",
        "/* Darwin only: the kernel implements BoImportHost (native ABI >= 1.9), so ac_drm_create_bo_from_user_mem works (has_userptr). */",
        "bool ac_drm_darwin_has_userptr(ac_drm_device *dev);",
        "#endif",
        "MESAPROC int ac_drm_bo_free(ac_drm_device *dev, ac_drm_bo bo) TAIL;",
        "MESAPROC int ac_drm_bo_cpu_map(ac_drm_device *dev, ac_drm_bo bo, void **cpu) TAIL;",
    ]
    p = tree / "src/amd/common/ac_linux_drm.h"
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text("\n".join(ac_h_lines) + "\n", encoding="utf-8")

    # 2. ac_linux_drm.c (post-0004 context at line 74)
    ac_c_lines = ["/* pad */"] * 73 + [
        "bool ac_drm_darwin_has_userptr(ac_drm_device *dev)",
        "{",
        "   return dev->is_darwin && acd_device_has_userptr(dev->ddev);",
        "}",
        "#endif",
        "",
        "int ac_drm_device_initialize(int fd, bool is_virtio,",
    ]
    p = tree / "src/amd/common/ac_linux_drm.c"
    p.write_text("\n".join(ac_c_lines) + "\n", encoding="utf-8")

    # 3. radv_physical_device.c (post-0019 + 0021 + 0022 exact context at lines 49 & 99)
    pdev_lines = ["/* pad */"] * 48 + [
        '#include "git_sha1.h"',
        "",
        "#ifdef HAVE_AMDGPU_DARWIN",
        "struct acd_kext *ac_drm_darwin_kext(ac_drm_device *dev, uint32_t *abi_minor);",
        "static void radv_create_drm_device_locked(struct radv_physical_device *pdev);",
        "#endif",
        "",
    ]
    assert len(pdev_lines) == 55
    pdev_lines += ["/* pad */"] * (97 - len(pdev_lines)) + [
        "bool",
        "radv_sparse_enabled(const struct radv_physical_device *pdev)",
        "{",
        "#ifdef HAVE_AMDGPU_DARWIN",
        "   /* RDNA4N has no PRT mappings or sparse binds, including experimental mode. */",
        "   return false;",
        "#endif",
        "",
        "   const struct radv_instance *instance = radv_physical_device_instance(pdev);",
        "",
        "   if (instance->queue_disable_flags & RADV_QUEUE_DISABLE_SPARSE)",
        "      return false;",
        "",
        "   return true;",
        "}",
    ]
    p = tree / "src/amd/vulkan/radv_physical_device.c"
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text("\n".join(pdev_lines) + "\n", encoding="utf-8")

    # 4. radv_amdgpu_bo.c (post-0017 + 0018 exact context at line 274)
    bo_lines = ["/* pad */"] * 273 + [
        "",
        "   assert(parent->base.is_virtual);",
        "   assert(!bo || !bo->base.is_virtual);",
        "   if (parent->children)",
        "      return VK_ERROR_FEATURE_NOT_PRESENT;",
        "",
        "   /* When the BO is NULL, AMDGPU will reset the PTE VA range to the initial state. Otherwise, it",
    ]
    p = tree / "src/amd/vulkan/winsys/amdgpu/radv_amdgpu_bo.c"
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text("\n".join(bo_lines) + "\n", encoding="utf-8")

    # Also verify our anchor snippets literally match 0004/0017/0019/0021 patch texts.
    p0021 = (patch_dir / "0021-radv-darwin-hide-unsupported-sparse-resources.patch").read_text(encoding="utf-8")
    if "RDNA4N has no PRT mappings or sparse binds, including experimental mode." not in p0021:
        raise SystemExit("0021 context anchor drift detected")
    p0017 = (patch_dir / "0017-radv-darwin-composite-private-backing.patch").read_text(encoding="utf-8")
    if "if (parent->children)" not in p0017:
        raise SystemExit("0017 context anchor drift detected")


def extract_c_function(source_path: Path, signature_prefix: str) -> str:
    text = source_path.read_text(encoding="utf-8")
    idx = text.find(signature_prefix)
    if idx < 0:
        raise SystemExit(f"{signature_prefix!r} not found in {source_path}")
    brace = text.find("{", idx)
    depth = 0
    end = -1
    for pos in range(brace, len(text)):
        if text[pos] == "{":
            depth += 1
        elif text[pos] == "}":
            depth -= 1
            if depth == 0:
                end = pos + 1
                break
    if end < 0:
        raise SystemExit(f"unbalanced braces in {signature_prefix!r} ({source_path})")
    return text[idx:end]


def extract_radv_sparse_enabled(pdev_c: Path, out_inc: Path) -> None:
    body = extract_c_function(pdev_c, "bool\nradv_sparse_enabled(")
    # Replace the upstream tail only if this came from a full Mesa archive with
    # extra RADV fields beyond our minimal test stub; for our anchor keep verbatim.
    if "return true;" not in body:
        # Full Mesa archive: truncate cleanly after RADV_QUEUE_DISABLE_SPARSE return false;
        marker = "if (instance->queue_disable_flags & RADV_QUEUE_DISABLE_SPARSE)\n      return false;"
        m_idx = body.find(marker)
        if m_idx < 0:
            raise SystemExit("unexpected radv_sparse_enabled structure in Mesa archive")
        body = body[: m_idx + len(marker)] + "\n   return true;\n}"
    out_inc.write_text(body + "\n", encoding="utf-8")


def extract_acd_drm_sparse(drm_c: Path, out_inc: Path) -> None:
    fn1 = extract_c_function(drm_c, "static int\nacd_cmd_gem_va(")
    fn2 = extract_c_function(drm_c, "bool\nacd_device_has_vm_prt(")
    out_inc.write_text(fn1 + "\n\n" + fn2 + "\n", encoding="utf-8")


def replay_on_archive(
    archive: Path,
    patches: list[Path],
    dest: Path,
    expected_sha256: str | None,
) -> Path:
    archive_hash = digest(archive)
    if expected_sha256 and archive_hash != expected_sha256:
        raise SystemExit(
            f"archive hash mismatch: expected {expected_sha256}, got {archive_hash}"
        )
    dest.mkdir(parents=True)
    with tarfile.open(archive, "r:*") as tar:
        tar.extractall(dest, filter="data")
    roots = [p for p in dest.iterdir() if p.name != "__MACOSX"]
    if len(roots) != 1 or not roots[0].is_dir():
        raise SystemExit(f"archive must contain one top-level directory: {roots}")
    source = roots[0]
    for patch in patches:
        run(
            [
                "patch",
                "-d",
                str(source),
                "-p1",
                "--fuzz=0",
                "--force",
                "--batch",
                "--no-backup-if-mismatch",
            ],
            stdin=patch,
        )
    rejects = list(source.rglob("*.rej")) + list(source.rglob("*.orig"))
    if rejects:
        raise SystemExit(f"patch series left rejects/originals: {rejects}")
    return source


def replay_standalone(patches: list[Path], patch_dir: Path, source: Path) -> None:
    source.mkdir(parents=True)
    created: set[str] = set()
    # Apply 0001..0027 to all Darwin-created files
    for patch in patches[:-1]:
        for is_new, rel, sec in split_patch_sections(patch.read_text(encoding="utf-8")):
            if is_new:
                created.add(rel)
            if rel in created:
                (source / rel).parent.mkdir(parents=True, exist_ok=True)
                proc = subprocess.run(
                    [
                        "patch",
                        "-d",
                        str(source),
                        "-p1",
                        "--fuzz=0",
                        "--force",
                        "--batch",
                        "--no-backup-if-mismatch",
                    ],
                    input=sec,
                    text=True,
                    capture_output=True,
                )
                if proc.returncode != 0:
                    raise SystemExit(
                        f"failed applying {patch.name} to {rel}:\n{proc.stdout}\n{proc.stderr}"
                    )
    build_upstream_anchors(patch_dir, source)
    # Apply final patch (0028) as one atomic patch across all 11 touched files with --fuzz=0
    run(
        [
            "patch",
            "-d",
            str(source),
            "-p1",
            "--fuzz=0",
            "--force",
            "--batch",
            "--no-backup-if-mismatch",
        ],
        stdin=patches[-1],
    )
    rejects = list(source.rglob("*.rej")) + list(source.rglob("*.orig"))
    if rejects:
        raise SystemExit(f"0028 left rejects/originals: {rejects}")
    print(f"STANDALONE_REPLAY_PASS (0001..{patches[-1].name[:4]}, --fuzz=0, 0 rejects)")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("output_dir", type=Path)
    parser.add_argument("--patches", type=Path, default=Path(__file__).resolve().parent)
    parser.add_argument("--last", default="0028")
    parser.add_argument("--mesa-archive", type=Path)
    parser.add_argument("--expected-archive-sha256")
    parser.add_argument("--cc", default=os.environ.get("CC", "clang"))
    args = parser.parse_args()

    out = args.output_dir.resolve()
    patch_dir = args.patches.resolve()
    repo_root = patch_dir.parent
    kernel_abi = repo_root / "src/garnet/src/GarnetNativeABI.h"
    if not kernel_abi.is_file():
        raise SystemExit(f"missing kernel ABI header: {kernel_abi}")

    if out.exists() and any(out.iterdir()):
        raise SystemExit(f"output directory is not empty: {out}")
    out.mkdir(parents=True, exist_ok=True)

    patches = patch_files(patch_dir, args.last)
    p0028 = patches[-1]
    hunks = validate_patch_hunks(p0028)
    print(f"HUNK_INTEGRITY_PASS {p0028.name} ({hunks} hunks, sha256={digest(p0028)})")

    if args.mesa_archive:
        source = replay_on_archive(
            args.mesa_archive.resolve(),
            patches,
            out / "extract",
            args.expected_archive_sha256,
        )
        drm_uapi_inc = source / "include"
        mode = "full-mesa-archive"
    else:
        source = out / "reconstructed"
        replay_standalone(patches, patch_dir, source)
        drm_uapi_inc = out / "inc"
        (drm_uapi_inc / "drm-uapi").mkdir(parents=True)
        (drm_uapi_inc / "drm-uapi/drm.h").write_text(STUB_DRM_H, encoding="utf-8")
        (drm_uapi_inc / "drm-uapi/amdgpu_drm.h").write_text(
            STUB_AMDGPU_DRM_H, encoding="utf-8"
        )
        mode = "standalone-reconstructed"

    check_abi_parity(kernel_abi, source / "include/darwin/rdna4_native_abi.h")

    # Verify 0028 actually removed 0021's unconditional return false and added the composite guard.
    pdev_txt = (source / "src/amd/vulkan/radv_physical_device.c").read_text(encoding="utf-8")
    if "RDNA4N has no PRT mappings or sparse binds" in pdev_txt:
        raise SystemExit("0021 unconditional sparse disable comment still present after 0028")
    if "ac_drm_darwin_has_vm_prt" not in pdev_txt:
        raise SystemExit("radv_physical_device.c missing ac_drm_darwin_has_vm_prt check")
    bo_txt = (source / "src/amd/vulkan/winsys/amdgpu/radv_amdgpu_bo.c").read_text(encoding="utf-8")
    if "if (parent->children || (bo && bo->children))" not in bo_txt:
        raise SystemExit("radv_amdgpu_bo.c missing composite child sparse backing guard")

    harness_dir = out / "harness"
    harness_dir.mkdir()
    extract_radv_sparse_enabled(
        source / "src/amd/vulkan/radv_physical_device.c",
        harness_dir / "radv_sparse_enabled_actual.inc",
    )
    extract_acd_drm_sparse(
        source / "src/amd/common/darwin/ac_darwin_drm.c",
        harness_dir / "acd_drm_sparse_actual.inc",
    )
    harness_c = harness_dir / "sparse_prt_host_test.c"
    harness_c.write_text(HOST_HARNESS_C, encoding="utf-8")

    cc = shlex.split(args.cc)
    cc_name = Path(cc[0]).name
    is_gcc = "gcc" in cc_name
    kext_extra = ["-Wno-enum-compare"] if is_gcc else []

    inc_flags = [
        "-I",
        str(drm_uapi_inc),
        "-I",
        str(source / "include/darwin"),
        "-I",
        str(source / "src/amd/common/darwin"),
        "-I",
        str(harness_dir),
    ]
    kext_c = str(source / "src/amd/common/darwin/ac_darwin_kext.c")
    mock_c = str(source / "src/amd/common/darwin/ac_darwin_mock.c")
    acd_test_c = str(source / "src/amd/common/darwin/test/acd_kext_test.c")

    san_env = dict(os.environ)
    san_env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
    san_env["ASAN_OPTIONS"] = (
        "detect_leaks=0:halt_on_error=1"
        if sys.platform == "darwin"
        else "detect_leaks=1:halt_on_error=1"
    )

    builds = (
        ("opt", ["-O2"], None),
        ("asan-ubsan", ["-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"], san_env),
    )

    for label, opt_flags, run_env in builds:
        kext_o = str(harness_dir / f"ac_darwin_kext_{label}.o")
        mock_o = str(harness_dir / f"ac_darwin_mock_{label}.o")
        run(
            cc
            + ["-std=gnu11", "-Wall", "-Wextra", "-Werror"]
            + kext_extra
            + opt_flags
            + inc_flags
            + ["-c", kext_c, "-o", kext_o]
        )
        run(
            cc
            + ["-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter"]
            + opt_flags
            + inc_flags
            + ["-c", mock_c, "-o", mock_o]
        )

        # 1. Host sparse/PRT + mock interval + radv_sparse_enabled harness
        host_bin = str(harness_dir / f"sparse_prt_host_test_{label}")
        run(
            cc
            + ["-std=gnu11", "-Wall", "-Wextra", "-Werror"]
            + opt_flags
            + inc_flags
            + [str(harness_c), kext_o, mock_o, "-pthread", "-o", host_bin]
        )
        run([host_bin], env=run_env)

        # 2. Upstream Darwin kext unit test (acd_kext_test.c with new test_sparse_prt_va)
        kext_bin = str(harness_dir / f"acd_kext_test_{label}")
        run(
            cc
            + ["-std=gnu11", "-Wall", "-Wextra", "-Werror"]
            + opt_flags
            + inc_flags
            + [acd_test_c, kext_o, "-pthread", "-o", kext_bin]
        )
        run([kext_bin], env=run_env)

    print(f"SPARSE_PRT_SERIES_PASS mode={mode} cc={cc[0]} patch={p0028.name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
