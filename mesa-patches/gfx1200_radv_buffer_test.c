#include "winsys/amdgpu/radv_darwin_scanout.h"
#include "winsys/amdgpu/radv_amdgpu_bo.h"
#include "radv_device.h"
#include "radv_device_memory.h"
#include "common/darwin/ac_darwin_kext.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

static struct acd_kext *connection;
static ac_drm_device *expected_dev;
static unsigned calls;
static struct rdna4n_buffer_op last_op;
static uint32_t last_selector;
static int transport_error;
static bool busy_reply;

struct acd_kext *ac_drm_darwin_kext(ac_drm_device *dev, uint32_t *minor);
struct acd_kext *
ac_drm_darwin_kext(ac_drm_device *dev, uint32_t *minor)
{
   assert(dev == expected_dev);
   if (minor) *minor = 12;
   return connection;
}
int acd_kext_open_iokit(struct acd_kext **out) { (void)out; return -ENODEV; }
int acd_mock_open(struct acd_kext **out) { (void)out; return -ENODEV; }

static int
call(void *ctx, uint32_t sel, const uint64_t *sin, uint32_t nsin,
     const void *input, size_t size, uint64_t *output, uint32_t *count,
     void *stout, size_t *stoutsz)
{
   assert(ctx == &calls);
   (void)stout; (void)stoutsz;
   ++calls;
   last_selector = sel;
   if (transport_error) return transport_error;
   if (sel == ACD_SEL_QUERY_INFO) {
      assert(*stoutsz == sizeof(struct rdna4n_info));
      memset(stout, 0, *stoutsz);
      ((struct rdna4n_info *)stout)->flags = RDNA4N_INFO_BUFFER_OPS;
      return 0;
   }
   if (sel == ACD_SEL_WAIT_SEQ) {
      assert(nsin == 3 && sin[0] == 23 && sin[1] == 10 && *count == 3);
      output[0] = busy_reply;
      output[1] = busy_reply ? 22 : 23;
      output[2] = 23;
   } else {
      assert(nsin == 0 && size == sizeof(last_op) && *count == 1);
      memcpy(&last_op, input, size);
      output[0] = 23;
   }
   return 0;
}

int main(void)
{
   struct radv_device device = {0}, foreign = {0};
   struct radv_amdgpu_winsys ws = {0};
   struct radv_amdgpu_winsys_bo src_bo = {.base.size = 4096, .bo_handle = 7};
   struct radv_amdgpu_winsys_bo dst_bo = {.base.size = 4096, .bo_handle = 8};
   struct radv_device_memory src = {.base = {.device = &device.vk}, .bo = &src_bo.base, .alloc_size = 1024};
   struct radv_device_memory dst = {.base = {.device = &device.vk}, .bo = &dst_bo.base, .alloc_size = 2048};
   device.vk.base.type = VK_OBJECT_TYPE_DEVICE;
   src.base.type = dst.base.type = VK_OBJECT_TYPE_DEVICE_MEMORY;
   device.ws = &ws.base;
   expected_dev = ws.dev = (ac_drm_device *)&ws;
   VkDevice dev = radv_device_to_handle(&device);
   VkDeviceMemory sm = radv_device_memory_to_handle(&src), dm = radv_device_memory_to_handle(&dst);
   const struct acd_kext_ops ops = {.call = call};
   connection = acd_kext_create(&ops, &calls);
   assert(connection);
   uint64_t seq = 0;
   assert(radv_darwin_buffer_copy(dev, sm, 16, dm, 32, 64, &seq) == 0 && seq == 23);
   assert(last_selector == RDNA4N_SEL_BUFFER_COPY && last_op.src_handle == 7 && last_op.dst_handle == 8);
   assert(last_op.src_offset == 16 && last_op.dst_offset == 32 && last_op.length == 64);
   assert(radv_darwin_buffer_fill(dev, dm, 2044, 4, 0x12345678, &seq) == 0);
   assert(last_selector == RDNA4N_SEL_BUFFER_FILL && !last_op.src_handle && last_op.pattern == 0x12345678);
   unsigned before = calls;
   for (unsigned i = 0; i < 11; ++i) {
      struct radv_device_memory saved = dst;
      struct radv_amdgpu_winsys_bo saved_bo = dst_bo;
      uint64_t offset = 0, length = 4;
      VkDeviceMemory target = dm;
      switch (i) {
      case 0: dst.base.device = &foreign.vk; break;
      case 1: dst.bo = NULL; break;
      case 2: dst_bo.base.is_virtual = true; break;
      case 3: dst.user_ptr = &dst; break;
      case 4: dst.import_handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT; break;
      case 5: offset = 2048; break;
      case 6: offset = UINT64_MAX - 3; break;
      case 7: dst_bo.base.size = 2; break;
      case 8: dst_bo.bo_handle = 0; break;
      case 9: length = 3; break;
      case 10: target = VK_NULL_HANDLE; break;
      }
      seq = 99;
      assert(radv_darwin_buffer_fill(dev, target, offset, length, 0, &seq) == -EINVAL && seq == 0);
      assert(calls == before);
      dst = saved; dst_bo = saved_bo;
   }
   assert(radv_darwin_buffer_copy(dev, sm, 1020, dm, 0, 8, &seq) == -EINVAL);
   assert(radv_darwin_buffer_copy(dev, dm, 0, dm, 4, 16, &seq) == -EINVAL);
   assert(calls == before);
   device.vk._lost.lost = 1;
   assert(radv_darwin_buffer_fill(dev, dm, 0, 4, 0, &seq) == -ECANCELED && calls == before);
   device.vk._lost.lost = 0;
   transport_error = ACD_KR_UNSUPPORTED;
   assert(radv_darwin_buffer_copy(dev, sm, 0, dm, 0, 4, &seq) == -ENOSYS && seq == 0);
   transport_error = ACD_KR_TIMEOUT;
   assert(radv_darwin_buffer_fill(dev, dm, 0, 4, 0, &seq) == -ETIME && seq == 0);
   transport_error = 0;
   bool busy = false;
   busy_reply = true;
   assert(radv_darwin_buffer_wait(dev, 23, 10, &busy) == 0 && busy);
   busy_reply = false;
   assert(radv_darwin_buffer_wait(dev, 23, 10, &busy) == 0 && !busy);
   transport_error = ACD_KR_ABORTED;
   assert(radv_darwin_buffer_wait(dev, 23, 10, &busy) == -ECANCELED && busy);
   acd_kext_destroy(connection);
   connection = NULL;
   assert(radv_darwin_buffer_fill(dev, dm, 0, 4, 0, &seq) == -ENOSYS && seq == 0);
   puts("RADV buffer bridge: real RADV objects/translation unit, same connection, device/bounds/import/overlap no-call, fallback/loss/wait PASS (mock transport, no GPU)");
   return 0;
}
