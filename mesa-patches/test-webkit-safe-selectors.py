#!/usr/bin/env python3
"""Verify Mesa RADV Darwin patch 0033 (ABI 1.14 WebKit-silent APP selectors).

Replays 0001..0033 cleanly with `patch -p1 --fuzz=0` on Darwin-added files,
checks ABI 1.14 constant parity with src/garnet/src/GarnetNativeABI.h, and
compiles + runs ac_darwin_kext.c under -Wall -Wextra -Werror with ASan+UBSan
against a fake transport verifying:
  - GarnetAccelerator + ABI >= 14 routes 3->30, 21->31, 27->39 (and keeps 28)
  - GarnetAccelerator + ABI <= 13 falls back automatically to 3, 21, 27
  - GarnetDriver route always uses 3, 21, 27 even on ABI >= 14
  - Selector 29 is never emitted (reserved for VCN 5.0 ABI 1.15)
  - acd_kr_to_errno maps RDNA4N_SEL_BUFFER_COPY_APP UNSUPPORTED/NOT_PRIVILEGED -> -ENOSYS
"""
import glob
import os
import pathlib
import re
import shutil
import subprocess
import sys


TARGET_PREFIXES = ("include/darwin/", "src/amd/common/darwin/")


def filter_patch(text: str) -> str:
    lines = text.splitlines(keepends=True)
    out = []
    i = 0
    while i < len(lines):
        if lines[i].startswith("--- ") and i + 1 < len(lines) and lines[i + 1].startswith("+++ "):
            a_path = lines[i][4:].strip().split("\t")[0]
            b_path = lines[i + 1][4:].strip().split("\t")[0]
            p = b_path[2:] if b_path.startswith("b/") else (a_path[2:] if a_path.startswith("a/") else b_path)
            keep = any(p.startswith(pref) for pref in TARGET_PREFIXES)
            j = i + 2
            while j < len(lines) and not lines[j].startswith("--- ") and not lines[j].startswith("diff --git "):
                j += 1
            if keep:
                out.extend(lines[i:j])
            i = j
            continue
        i += 1
    return "".join(out)


HARNESS_C = r"""
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "ac_darwin_kext.h"

int acd_mock_open(struct acd_kext **out) { (void)out; return -ENOSYS; }
int acd_kext_open_iokit(struct acd_kext **out) { (void)out; return -ENOSYS; }

struct fake_tx {
   uint32_t minor;
   uint32_t info_flags;
   uint32_t last_sel;
   int call_count;
};

static int fake_call(void *ctx, uint32_t sel, const uint64_t *sin, uint32_t nsin,
                     const void *stin, size_t stinsz, uint64_t *sout, uint32_t *nsout,
                     void *stout, size_t *stoutsz) {
   struct fake_tx *f = ctx;
   (void)sin; (void)nsin; (void)stin; (void)stinsz;
   f->last_sel = sel;
   f->call_count++;
   if (sel == ACD_SEL_HELLO) {
      sout[0] = RDNA4N_ABI_VERSION | ((uint64_t)f->minor << 16);
      sout[1] = 700;
      sout[2] = RDNA4N_MAX_IBS;
      sout[3] = 9;
      *nsout = 4;
      return ACD_KR_SUCCESS;
   }
   if (sel == ACD_SEL_QUERY_INFO) {
      struct rdna4n_info info;
      memset(&info, 0, sizeof(info));
      info.abi_version = RDNA4N_ABI_VERSION;
      info.flags = f->info_flags;
      info.reserved[2] = f->minor;
      memcpy(stout, &info, sizeof(info));
      *stoutsz = sizeof(info);
      return ACD_KR_SUCCESS;
   }
   if (sel == ACD_SEL_BO_CREATE || sel == ACD_SEL_BO_CREATE_APP ||
       sel == ACD_SEL_BO_IMPORT_HOST || sel == ACD_SEL_BO_IMPORT_HOST_APP) {
      sout[0] = 7;
      sout[1] = 4096;
      sout[2] = RDNA4N_GEM_DOMAIN_VRAM;
      sout[3] = 0;
      *nsout = 4;
      return ACD_KR_SUCCESS;
   }
   if (sel == RDNA4N_SEL_BUFFER_COPY || sel == RDNA4N_SEL_BUFFER_COPY_APP ||
       sel == RDNA4N_SEL_BUFFER_FILL) {
      sout[0] = 42;
      *nsout = 1;
      return ACD_KR_SUCCESS;
   }
   return ACD_KR_BAD_ARGUMENT;
}

static void run_case(bool accel_route, uint32_t minor,
                     uint32_t want_bo, uint32_t want_imp, uint32_t want_copy) {
   struct fake_tx f = { .minor = minor, .info_flags = RDNA4N_INFO_BUFFER_OPS | RDNA4N_INFO_READY };
   struct acd_kext_ops ops = { .call = fake_call };
   struct acd_kext *k = acd_kext_create(&ops, &f);
   assert(k != NULL);
   acd_kext_set_accel_route(k, accel_route);

   struct acd_hello h;
   assert(acd_kext_hello(k, &h) == 0);
   assert(h.minor == minor);

   struct rdna4n_gem_create_in ci = { .bo_size = 4096, .alignment = 4096, .domains = RDNA4N_GEM_DOMAIN_VRAM };
   struct acd_bo_created bo;
   assert(acd_kext_bo_create(k, &ci, &bo) == 0);
   assert(f.last_sel == want_bo && bo.handle == 7);

   struct acd_bo_imported imp;
   assert(acd_kext_bo_import_host(k, (void *)(uintptr_t)0x100000, 4096, &imp) == 0);
   assert(f.last_sel == want_imp && imp.handle == 7);

   struct rdna4n_buffer_op copy_op = { .src_handle = 1, .dst_handle = 2, .length = 16 };
   uint64_t seq = 0;
   assert(acd_kext_buffer_op(k, &copy_op, false, &seq) == 0);
   assert(f.last_sel == want_copy && seq == 42);

   struct rdna4n_buffer_op fill_op = { .dst_handle = 2, .pattern = 0xdeadbeefu, .length = 16 };
   assert(acd_kext_buffer_op(k, &fill_op, true, &seq) == 0);
   assert(f.last_sel == RDNA4N_SEL_BUFFER_FILL && seq == 42);

   acd_kext_destroy(k);
}

int main(void) {
   assert(RDNA4N_ABI_MINOR == 14u);
   assert(ACD_SEL_BO_CREATE_APP == 30 && ACD_SEL_BO_IMPORT_HOST_APP == 31 && ACD_SEL_BUFFER_COPY_APP == 39);
   assert(RDNA4N_INFO_READY == (1u << 8) && RDNA4N_INFO_DISPLAY_ACTIVE == (1u << 9) && RDNA4N_INFO_KILL_SWITCH == (1u << 10));

   /* Pure selector mapping checks */
   assert(acd_kext_select_app_selector(ACD_SEL_BO_CREATE, true, 14) == 30);
   assert(acd_kext_select_app_selector(ACD_SEL_BO_IMPORT_HOST, true, 14) == 31);
   assert(acd_kext_select_app_selector(RDNA4N_SEL_BUFFER_COPY, true, 14) == 39);
   assert(acd_kext_select_app_selector(RDNA4N_SEL_BUFFER_FILL, true, 14) == 28);
   assert(acd_kext_select_app_selector(ACD_SEL_BO_CREATE, true, 13) == 3);
   assert(acd_kext_select_app_selector(ACD_SEL_BO_IMPORT_HOST, true, 13) == 21);
   assert(acd_kext_select_app_selector(RDNA4N_SEL_BUFFER_COPY, true, 13) == 27);
   assert(acd_kext_select_app_selector(ACD_SEL_BO_CREATE, false, 14) == 3);
   assert(acd_kext_select_app_selector(ACD_SEL_BO_IMPORT_HOST, false, 14) == 21);
   assert(acd_kext_select_app_selector(RDNA4N_SEL_BUFFER_COPY, false, 14) == 27);

   /* Error mapping checks for alias 39 */
   assert(acd_kr_to_errno(RDNA4N_SEL_BUFFER_COPY_APP, (int)ACD_KR_UNSUPPORTED) == -ENOSYS);
   assert(acd_kr_to_errno(RDNA4N_SEL_BUFFER_COPY_APP, (int)ACD_KR_NOT_PRIVILEGED) == -ENOSYS);

   /* Transport end-to-end checks */
   run_case(true,  14, 30, 31, 39);
   run_case(true,  15, 30, 31, 39);
   run_case(true,  13,  3, 21, 27);
   run_case(false, 14,  3, 21, 27);
   printf("test-webkit-safe-selectors: ALL CHECKS PASSED\n");
   return 0;
}
"""


MIN_DRM_UAPI = r"""
#ifndef AMDGPU_DRM_H_STUB
#define AMDGPU_DRM_H_STUB
#include <stdint.h>
#define AMDGPU_GEM_DOMAIN_GTT  0x2
#define AMDGPU_GEM_DOMAIN_VRAM 0x4
#define AMDGPU_GEM_CREATE_NO_CPU_ACCESS (1ull << 1)
#define AMDGPU_GEM_CREATE_CP_MQD_GFX9   (1ull << 8)
#define AMDGPU_GEM_CREATE_ENCRYPTED     (1ull << 10)
#define AMDGPU_GEM_CREATE_PREEMPTIBLE   (1ull << 11)
#define AMDGPU_VA_OP_MAP     1
#define AMDGPU_VA_OP_UNMAP   2
#define AMDGPU_VA_OP_CLEAR   3
#define AMDGPU_VA_OP_REPLACE 4
#define AMDGPU_VM_DELAY_UPDATE    (1u << 0)
#define AMDGPU_VM_PAGE_READABLE   (1u << 1)
#define AMDGPU_VM_PAGE_WRITEABLE  (1u << 2)
#define AMDGPU_VM_PAGE_EXECUTABLE (1u << 3)
#define AMDGPU_VM_PAGE_PRT        (1u << 4)
#define AMDGPU_VM_MTYPE_MASK      (0xfu << 5)
#define AMDGPU_VM_MTYPE_UC        (4u << 5)
#define AMDGPU_VM_MTYPE_RW        (5u << 5)
#define AMDGPU_VM_PAGE_NOALLOC    (1u << 9)
#define AMDGPU_CTX_OP_ALLOC_CTX    1
#define AMDGPU_CTX_OP_FREE_CTX     2
#define AMDGPU_CTX_OP_QUERY_STATE2 4
#define AMDGPU_HW_IP_GFX     0
#define AMDGPU_HW_IP_COMPUTE 1
#define AMDGPU_IB_FLAG_CE                  (1u << 0)
#define AMDGPU_IB_FLAG_PREAMBLE            (1u << 1)
#define AMDGPU_IB_FLAG_TC_WB_NOT_INVALIDATE (1u << 3)
#define AMDGPU_IB_FLAGS_SECURE             (1u << 5)
#define AMDGPU_IB_FLAG_EMIT_MEM_SYNC       (1u << 6)
#define AMDGPU_CHUNK_ID_IB 0x01
#define AMDGPU_CHUNK_ID_FENCE 0x02
#define AMDGPU_CHUNK_ID_DEPENDENCIES 0x03
#define AMDGPU_CHUNK_ID_SYNCOBJ_IN 0x04
#define AMDGPU_CHUNK_ID_SYNCOBJ_OUT 0x05
#define AMDGPU_CHUNK_ID_BO_HANDLES 0x06
#define AMDGPU_CHUNK_ID_SCHEDULED_DEPENDENCIES 0x07
#define AMDGPU_CHUNK_ID_SYNCOBJ_TIMELINE_WAIT 0x08
#define AMDGPU_CHUNK_ID_SYNCOBJ_TIMELINE_SIGNAL 0x09
#define AMDGPU_CHUNK_ID_CP_GFX_SHADOW 0x0a
struct drm_amdgpu_gem_create_in { uint64_t bo_size, alignment, domains, domain_flags; };
struct drm_amdgpu_gem_va {
   uint32_t handle, _pad, operation, flags;
   uint64_t va_address, offset_in_bo, map_size;
   uint64_t vm_timeline_point;
   uint32_t vm_timeline_syncobj_out, num_syncobj_handles;
   uint64_t input_fence_syncobj_handles;
};
union drm_amdgpu_ctx { struct { uint32_t op, flags, ctx_id; int32_t priority; } in; };
struct drm_amdgpu_cs_chunk { uint32_t chunk_id, length_dw; uint64_t chunk_data; };
struct drm_amdgpu_cs_chunk_ib { uint32_t _pad, flags; uint64_t va_start; uint32_t ib_bytes, ip_type, ip_instance, ring; };
struct drm_amdgpu_cs_chunk_fence { uint32_t handle, offset; };
struct drm_amdgpu_cs_chunk_sem { uint32_t handle; };
struct drm_amdgpu_cs_chunk_syncobj { uint32_t handle, flags; uint64_t point; };
#endif
"""


def main() -> int:
    repo = pathlib.Path(__file__).resolve().parent.parent
    out = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else repo / "temp/scratch/webkit-sel-test"
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)

    patches = sorted(glob.glob(str(repo / "mesa-patches/00*.patch")))
    assert any("0033-darwin-webkit-safe-app-selectors.patch" in p for p in patches), "missing patch 0033"

    for pf in patches:
        sub = filter_patch(pathlib.Path(pf).read_text())
        if not sub.strip():
            continue
        r = subprocess.run(["patch", "-p1", "--fuzz=0", "-d", str(out)], input=sub, text=True, capture_output=True)
        if r.returncode != 0:
            sys.stderr.write(f"FAIL applying {pf}:\n{r.stdout}\n{r.stderr}\n")
            return 1

    # Check zero .rej files
    rejs = list(out.rglob("*.rej"))
    assert not rejs, f"reject files found: {rejs}"

    # Verify ABI 1.14 parity between kext GarnetNativeABI.h and Mesa rdna4_native_abi.h
    kext_abi = (repo / "src/garnet/src/GarnetNativeABI.h").read_text()
    mesa_abi = (out / "include/darwin/rdna4_native_abi.h").read_text()
    for token in [
        "#define RDNA4N_ABI_MINOR     14u",
        "RDNA4N_SEL_BOCREATE_APP       = 30",
        "RDNA4N_SEL_BO_IMPORT_HOST_APP = 31",
        "RDNA4N_SEL_BUFFER_COPY_APP    = 39",
        "#define RDNA4N_INFO_READY         (1u << 8)",
        "#define RDNA4N_INFO_DISPLAY_ACTIVE (1u << 9)",
        "#define RDNA4N_INFO_KILL_SWITCH   (1u << 10)",
    ]:
        assert token in kext_abi, f"missing in kext ABI: {token}"
        assert token in mesa_abi, f"missing in mesa ABI: {token}"
    # Verify iokit.c marks accel route
    iokit_src = (out / "src/amd/common/darwin/ac_darwin_iokit.c").read_text()
    assert "acd_kext_set_accel_route(*out, true)" in iokit_src

    # Build and execute ac_darwin_kext.c + harness with ASan+UBSan
    stub_dir = out / "stub_inc"
    (stub_dir / "drm-uapi").mkdir(parents=True)
    (stub_dir / "drm-uapi/amdgpu_drm.h").write_text(MIN_DRM_UAPI)
    harness = out / "harness.c"
    harness.write_text(HARNESS_C)
    exe = out / "webkit_sel_test"
    cc = os.environ.get("CC", "clang")
    cmd = [
        cc, "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
        f"-I{out / 'include/darwin'}", f"-I{out / 'src/amd/common/darwin'}", f"-I{stub_dir}",
        str(out / "src/amd/common/darwin/ac_darwin_kext.c"), str(harness),
        "-o", str(exe),
    ]
    subprocess.run(cmd, check=True)
    subprocess.run([str(exe)], check=True)
    print(f"Replayed {len(patches)} patches cleanly and passed ABI 1.14 WebKit-safe selector tests.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
