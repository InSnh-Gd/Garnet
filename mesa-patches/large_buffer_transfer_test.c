#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define HAVE_AMDGPU_DARWIN 1
#define MIN2(a, b) ((a) < (b) ? (a) : (b))
#define DIV_ROUND_UP(a, b) (((a) + (b) - 1) / (b))

#define RADV_BUFFER_OPS_CS_THRESHOLD 4096
#define RADV_BUFFER_OPS_GTT_CP_DMA_MAX_BYTES 65536
#define GFX10 10

#define AC_BARRIER_SYNC_CS (1u << 0)
#define AC_BARRIER_INV_VMEM (1u << 1)

#define RADV_QUEUE_COMPUTE 0
#define RADV_QUEUE_TRANSFER 1

#define VK_SUCCESS 0
#define VK_ERROR_OUT_OF_DEVICE_MEMORY (-2)
#define VK_ERROR_DEVICE_LOST (-4)

#define VK_ADDRESS_COPY_DEVICE_LOCAL_BIT_KHR (1u << 0)
#define VK_ADDRESS_COPY_SPARSE_BIT_KHR (1u << 1)

#define VK_COMMAND_POOL_CREATE_PROTECTED_BIT (1u << 0)
#define VK_SHADER_STAGE_COMPUTE_BIT (1u << 0)
#define VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT UINT64_C(1)
#define VK_ACCESS_2_SHADER_WRITE_BIT UINT64_C(1)

#define VK_NULL_HANDLE 0
#define FILL_PIPELINE ((VkPipeline)1)
#define COPY_PIPELINE ((VkPipeline)2)
#define FILL_LAYOUT ((VkPipelineLayout)1)
#define COPY_LAYOUT ((VkPipelineLayout)2)

typedef int32_t VkResult;
typedef uint32_t VkAddressCopyFlagsKHR;
typedef uintptr_t VkPipeline;
typedef uintptr_t VkPipelineLayout;
typedef uint32_t VkShaderStageFlags;

struct radv_cmd_stream {
   int unused;
};

struct radv_image {
   int unused;
};

struct vk_command_pool {
   uint32_t flags;
};

struct vk_command_buffer {
   VkResult record_result;
   struct vk_command_pool *pool;
};

struct radv_cmd_buffer {
   struct vk_command_buffer vk;
   int qf;
   struct radv_cmd_stream *cs;
};

struct radv_device {
   int unused;
};

struct radv_physical_device {
   struct {
      unsigned gfx_level;
      bool has_dedicated_vram;
      bool cp_dma_supports_sparse;
   } info;
};

struct fill_constants {
   uint64_t addr;
   uint32_t max_offset;
   uint32_t data;
};

struct copy_constants {
   uint64_t src_addr;
   uint64_t dst_addr;
   uint32_t max_offset;
};

enum transfer_backend {
   BACKEND_NONE,
   BACKEND_COMPUTE_COPY,
   BACKEND_COMPUTE_FILL,
   BACKEND_CP_COPY,
   BACKEND_CP_FILL,
};

struct emitted_transfer {
   enum transfer_backend backend;
   uint64_t src_addr;
   uint64_t dst_addr;
   uint64_t size;
   uint64_t max_offset;
   uint32_t dim_x;
   uint32_t value;
   bool use_16B_copy;
};

static struct radv_device test_device;
static struct radv_physical_device test_pdev = {
   .info = {
      .gfx_level = 12,
      .has_dedicated_vram = true,
      .cp_dma_supports_sparse = false,
   },
};

static struct emitted_transfer emitted[8];
static unsigned emitted_count;
static unsigned pipeline_lookup_count;
static unsigned pipeline_failure;
static unsigned flush_call_count;
static unsigned sdma_count;
static bool cpu_simulation;

static uint8_t *copy_src_range;
static uint8_t *copy_dst_range;
static uint8_t *fill_range;
static size_t copy_src_range_size;
static size_t copy_dst_range_size;
static size_t fill_range_size;

static enum transfer_backend bound_backend;
static enum transfer_backend pending_backend;
static uint64_t pending_size;
static bool pending_use_16B_copy;
static uint32_t pending_dim_x;
static struct fill_constants pending_fill;
static struct copy_constants pending_copy;

static struct radv_device *
radv_cmd_buffer_device(struct radv_cmd_buffer *cmd_buffer)
{
   (void)cmd_buffer;
   return &test_device;
}

static const struct radv_physical_device *
radv_device_physical(const struct radv_device *device)
{
   (void)device;
   return &test_pdev;
}

static bool
test_is_aligned(uint64_t src_va, uint64_t dst_va, uint64_t size)
{
   return !(size & 3) && !(src_va & 3) && !(dst_va & 3);
}

static VkResult
get_fill_memory_pipeline(struct radv_device *device, uint64_t size, VkPipeline *pipeline_out,
                         VkPipelineLayout *layout_out)
{
   (void)device;
   pipeline_lookup_count++;
   if (pipeline_failure && pipeline_lookup_count == pipeline_failure)
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;

   pending_backend = BACKEND_COMPUTE_FILL;
   pending_size = size;
   pending_use_16B_copy = size >= 16;
   *pipeline_out = FILL_PIPELINE;
   *layout_out = FILL_LAYOUT;
   return VK_SUCCESS;
}

static VkResult
get_copy_memory_pipeline(struct radv_device *device, uint64_t src_va, uint64_t dst_va, uint64_t size,
                         VkPipeline *pipeline_out, VkPipelineLayout *layout_out)
{
   (void)device;
   pipeline_lookup_count++;
   if (pipeline_failure && pipeline_lookup_count == pipeline_failure)
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;

   pending_backend = BACKEND_COMPUTE_COPY;
   pending_size = size;
   pending_use_16B_copy = size >= 16 && test_is_aligned(src_va, dst_va, size);
   *pipeline_out = COPY_PIPELINE;
   *layout_out = COPY_LAYOUT;
   return VK_SUCCESS;
}

static void
radv_meta_bind_compute_pipeline(struct radv_cmd_buffer *cmd_buffer, VkPipeline pipeline)
{
   (void)cmd_buffer;
   assert((pipeline == FILL_PIPELINE && pending_backend == BACKEND_COMPUTE_FILL) ||
          (pipeline == COPY_PIPELINE && pending_backend == BACKEND_COMPUTE_COPY));
   bound_backend = pending_backend;
}

static void
radv_meta_push_constants(struct radv_cmd_buffer *cmd_buffer, VkPipelineLayout layout, VkShaderStageFlags stage,
                         uint32_t offset, uint32_t size, const void *data)
{
   (void)cmd_buffer;
   assert(stage == VK_SHADER_STAGE_COMPUTE_BIT);
   assert(offset == 0);

   if (pending_backend == BACKEND_COMPUTE_FILL) {
      assert(layout == FILL_LAYOUT);
      assert(size == sizeof(pending_fill));
      memcpy(&pending_fill, data, sizeof(pending_fill));
   } else {
      assert(pending_backend == BACKEND_COMPUTE_COPY);
      assert(layout == COPY_LAYOUT);
      assert(size == sizeof(pending_copy));
      memcpy(&pending_copy, data, sizeof(pending_copy));
   }
}

static void
record_emitted(enum transfer_backend backend)
{
   assert(emitted_count < sizeof(emitted) / sizeof(emitted[0]));
   struct emitted_transfer *event = &emitted[emitted_count++];

   memset(event, 0, sizeof(*event));
   event->backend = backend;
   event->size = pending_size;
   event->use_16B_copy = pending_use_16B_copy;

   if (backend == BACKEND_COMPUTE_FILL) {
      event->dst_addr = pending_fill.addr;
      event->max_offset = pending_fill.max_offset;
      event->value = pending_fill.data;
   } else if (backend == BACKEND_COMPUTE_COPY) {
      event->src_addr = pending_copy.src_addr;
      event->dst_addr = pending_copy.dst_addr;
      event->max_offset = pending_copy.max_offset;
   }
}

static bool
range_contains(uint8_t *base, size_t capacity, uint64_t address, uint64_t size)
{
   if (!base || address < (uint64_t)(uintptr_t)base)
      return false;
   const uint64_t offset = address - (uint64_t)(uintptr_t)base;
   return offset <= capacity && size <= capacity - offset;
}

static void
simulate_compute_fill(void)
{
   if (!cpu_simulation || !range_contains(fill_range, fill_range_size, pending_fill.addr, pending_size))
      return;

   uint8_t *dst = (uint8_t *)(uintptr_t)pending_fill.addr;
   const uint32_t bytes_per_invocation = pending_use_16B_copy ? 16 : 4;
   for (uint32_t i = 0; i < pending_dim_x; i++) {
      const uint64_t offset = MIN2((uint64_t)i * bytes_per_invocation, pending_fill.max_offset);
      if (offset + bytes_per_invocation > pending_size)
         continue;
      for (uint32_t j = 0; j < bytes_per_invocation; j++)
         dst[offset + j] = (pending_fill.data >> ((j & 3) * 8)) & 0xff;
   }
}

static void
simulate_compute_copy(void)
{
   if (!cpu_simulation ||
       !range_contains(copy_src_range, copy_src_range_size, pending_copy.src_addr, pending_size) ||
       !range_contains(copy_dst_range, copy_dst_range_size, pending_copy.dst_addr, pending_size))
      return;

   const uint8_t *src = (const uint8_t *)(uintptr_t)pending_copy.src_addr;
   uint8_t *dst = (uint8_t *)(uintptr_t)pending_copy.dst_addr;
   const uint32_t bytes_per_invocation = pending_use_16B_copy ? 16 : 1;
   for (uint32_t i = 0; i < pending_dim_x; i++) {
      const uint64_t offset = MIN2((uint64_t)i * bytes_per_invocation, pending_copy.max_offset);
      if (offset + bytes_per_invocation > pending_size)
         continue;
      memcpy(dst + offset, src + offset, bytes_per_invocation);
   }
}

static void
validate_dispatch(uint32_t dim_x)
{
   assert(dim_x > 0);
   assert((uint64_t)dim_x + 63 <= UINT32_MAX);

   const uint64_t workgroups = ((uint64_t)dim_x + 63) / 64;
   const uint64_t aligned = workgroups * 64;
   const uint32_t remainder = (uint32_t)((uint64_t)dim_x + 64 - aligned);

   assert(workgroups > 0 && workgroups <= UINT32_MAX);
   assert(remainder >= 1 && remainder <= 64);
}

static void
radv_unaligned_dispatch(struct radv_cmd_buffer *cmd_buffer, uint32_t x, uint32_t y, uint32_t z)
{
   (void)cmd_buffer;
   assert(y == 1 && z == 1);
   assert(bound_backend == pending_backend);
   assert(pending_backend == BACKEND_COMPUTE_COPY || pending_backend == BACKEND_COMPUTE_FILL);

   validate_dispatch(x);
   pending_dim_x = x;
   record_emitted(pending_backend);
   emitted[emitted_count - 1].dim_x = x;

   if (pending_backend == BACKEND_COMPUTE_FILL)
      simulate_compute_fill();
   else
      simulate_compute_copy();

   bound_backend = BACKEND_NONE;
}

static void
radv_cp_dma_copy_memory(struct radv_cmd_buffer *cmd_buffer, uint64_t src_va, uint64_t dst_va, uint64_t size)
{
   (void)cmd_buffer;
   record_emitted(BACKEND_CP_COPY);
   emitted[emitted_count - 1].src_addr = src_va;
   emitted[emitted_count - 1].dst_addr = dst_va;
   emitted[emitted_count - 1].size = size;
}

static void
radv_cp_dma_fill_memory(struct radv_cmd_buffer *cmd_buffer, uint64_t dst_va, uint64_t size, uint32_t value)
{
   (void)cmd_buffer;
   record_emitted(BACKEND_CP_FILL);
   emitted[emitted_count - 1].dst_addr = dst_va;
   emitted[emitted_count - 1].size = size;
   emitted[emitted_count - 1].value = value;
}

static void
radv_sdma_copy_memory(struct radv_device *device, struct radv_cmd_stream *cs, uint64_t src_va,
                      uint64_t dst_va, uint64_t size, bool protected_copy)
{
   (void)device;
   (void)cs;
   (void)src_va;
   (void)dst_va;
   (void)size;
   (void)protected_copy;
   sdma_count++;
}

static void
radv_sdma_fill_memory(struct radv_device *device, struct radv_cmd_stream *cs, uint64_t dst_va,
                      uint64_t size, uint32_t value)
{
   (void)device;
   (void)cs;
   (void)dst_va;
   (void)size;
   (void)value;
   sdma_count++;
}

static uint32_t
radv_src_access_flush(struct radv_cmd_buffer *cmd_buffer, uint64_t stage, uint64_t access, uint32_t flags,
                      const struct radv_image *image, const void *view)
{
   (void)cmd_buffer;
   (void)stage;
   (void)access;
   (void)flags;
   (void)image;
   (void)view;
   flush_call_count++;
   return 0x10u << flush_call_count;
}

static void
vk_command_buffer_set_error(struct vk_command_buffer *command_buffer, VkResult error)
{
   assert(error != VK_SUCCESS);
   if (command_buffer->record_result == VK_SUCCESS)
      command_buffer->record_result = error;
}

#define vk_command_buffer_has_error(command_buffer) ((command_buffer)->record_result != VK_SUCCESS)

static void
radv_utrace_begin_compute_copy_memory(struct radv_cmd_buffer *cmd_buffer, uint64_t size)
{
   (void)cmd_buffer;
   (void)size;
}

static void
radv_utrace_end_compute_copy_memory(struct radv_cmd_buffer *cmd_buffer)
{
   (void)cmd_buffer;
}

#include "large_buffer_transfer_actual.inc"

static struct radv_cmd_stream test_cs;
static struct vk_command_pool test_pool = {
   .flags = VK_COMMAND_POOL_CREATE_PROTECTED_BIT,
};
static struct radv_cmd_buffer cmd;

static void
begin_case(int qf)
{
   memset(emitted, 0, sizeof(emitted));
   emitted_count = 0;
   pipeline_lookup_count = 0;
   pipeline_failure = 0;
   flush_call_count = 0;
   sdma_count = 0;
   cpu_simulation = false;
   bound_backend = BACKEND_NONE;
   pending_backend = BACKEND_NONE;
   pending_size = 0;
   pending_use_16B_copy = false;
   pending_dim_x = 0;
   memset(&pending_fill, 0, sizeof(pending_fill));
   memset(&pending_copy, 0, sizeof(pending_copy));
   memset(&cmd, 0, sizeof(cmd));
   cmd.vk.pool = &test_pool;
   cmd.qf = qf;
   cmd.cs = &test_cs;
}

static void
check_compute_copy_chunks(uint64_t src, uint64_t dst, uint64_t size, VkAddressCopyFlagsKHR flags,
                          const uint64_t *chunks, unsigned chunk_count)
{
   flags |= VK_ADDRESS_COPY_SPARSE_BIT_KHR;
   begin_case(RADV_QUEUE_COMPUTE);
   radv_copy_memory(&cmd, src, dst, size, flags, flags);

   assert(emitted_count == chunk_count);
   uint64_t src_offset = 0;
   uint64_t dst_offset = 0;
   uint64_t total = 0;
   for (unsigned i = 0; i < chunk_count; i++) {
      const struct emitted_transfer *event = &emitted[i];
      assert(event->backend == BACKEND_COMPUTE_COPY);
      assert(event->src_addr == src + src_offset);
      assert(event->dst_addr == dst + dst_offset);
      assert(event->size == chunks[i]);
      assert(event->use_16B_copy == (chunks[i] >= 16 && test_is_aligned(src + src_offset,
                                                                       dst + dst_offset, chunks[i])));
      if (event->use_16B_copy) {
         assert(event->max_offset == chunks[i] - 16);
         assert(event->dim_x == DIV_ROUND_UP(chunks[i], 16));
      } else {
         assert(event->max_offset == chunks[i]);
         assert(event->dim_x == chunks[i]);
      }
      assert(event->max_offset <= UINT32_MAX);
      src_offset += chunks[i];
      dst_offset += chunks[i];
      total += chunks[i];
   }
   assert(total == size);
}

static void
check_compute_fill_chunks(uint64_t dst, uint64_t size, VkAddressCopyFlagsKHR flags,
                          const uint64_t *chunks, unsigned chunk_count)
{
   flags |= VK_ADDRESS_COPY_SPARSE_BIT_KHR;
   begin_case(RADV_QUEUE_COMPUTE);
   const uint32_t returned_flush = radv_fill_memory(&cmd, dst, size, UINT32_C(0xa5a5c3c3), flags);

   assert(emitted_count == chunk_count);
   uint64_t dst_offset = 0;
   uint64_t total = 0;
   uint32_t expected_flush = AC_BARRIER_SYNC_CS | AC_BARRIER_INV_VMEM;
   for (unsigned i = 0; i < chunk_count; i++) {
      const struct emitted_transfer *event = &emitted[i];
      assert(event->backend == BACKEND_COMPUTE_FILL);
      assert(event->dst_addr == dst + dst_offset);
      assert(event->size == chunks[i]);
      assert(event->use_16B_copy == (chunks[i] >= 16));
      assert(event->max_offset == chunks[i] - (event->use_16B_copy ? 16 : 4));
      assert(event->dim_x == DIV_ROUND_UP(chunks[i], event->use_16B_copy ? 16 : 4));
      assert(event->max_offset <= UINT32_MAX);
      expected_flush |= 0x10u << (i + 1);
      dst_offset += chunks[i];
      total += chunks[i];
   }
   assert(total == size);
   assert(returned_flush == expected_flush);
}

static void
test_small_cpu_transfers(void)
{
   uint8_t src[128];
   uint8_t dst[128];
   uint8_t fill[128];

   memset(src, 0x5a, sizeof(src));
   memset(dst, 0xa5, sizeof(dst));
   memset(fill, 0xa5, sizeof(fill));
   for (unsigned i = 0; i < 64; i++)
      src[16 + i] = (uint8_t)(i + 1);

   begin_case(RADV_QUEUE_COMPUTE);
   cpu_simulation = true;
   memset(dst, 0xa5, sizeof(dst));
   copy_src_range = src;
   copy_src_range_size = sizeof(src);
   copy_dst_range = dst;
   copy_dst_range_size = sizeof(dst);
   radv_copy_memory(&cmd, (uint64_t)(uintptr_t)&src[16], (uint64_t)(uintptr_t)&dst[24], 64,
                    VK_ADDRESS_COPY_SPARSE_BIT_KHR, VK_ADDRESS_COPY_SPARSE_BIT_KHR);
   assert(emitted_count == 1);
   assert(emitted[0].backend == BACKEND_COMPUTE_COPY);
   assert(emitted[0].use_16B_copy);
   assert(memcmp(&dst[24], &src[16], 64) == 0);
   for (unsigned i = 0; i < 24; i++)
      assert(dst[i] == 0xa5);
   for (unsigned i = 88; i < sizeof(dst); i++)
      assert(dst[i] == 0xa5);

   begin_case(RADV_QUEUE_COMPUTE);
   cpu_simulation = true;
   fill_range = fill;
   fill_range_size = sizeof(fill);
   const uint32_t returned_flush =
      radv_fill_memory(&cmd, (uint64_t)(uintptr_t)&fill[8], 64, UINT32_C(0xdeadbeef),
                       VK_ADDRESS_COPY_SPARSE_BIT_KHR);
   assert(emitted_count == 1);
   assert(emitted[0].backend == BACKEND_COMPUTE_FILL);
   assert(emitted[0].use_16B_copy);
   assert(returned_flush == (AC_BARRIER_SYNC_CS | AC_BARRIER_INV_VMEM | 0x20));
   for (unsigned i = 8; i < 72; i += 4) {
      assert(fill[i + 0] == 0xef);
      assert(fill[i + 1] == 0xbe);
      assert(fill[i + 2] == 0xad);
      assert(fill[i + 3] == 0xde);
   }
   for (unsigned i = 0; i < 8; i++)
      assert(fill[i] == 0xa5);
   for (unsigned i = 72; i < sizeof(fill); i++)
      assert(fill[i] == 0xa5);

   begin_case(RADV_QUEUE_COMPUTE);
   cpu_simulation = true;
   memset(dst, 0xa5, sizeof(dst));
   copy_src_range = src;
   copy_src_range_size = sizeof(src);
   copy_dst_range = dst;
   copy_dst_range_size = sizeof(dst);
   radv_copy_memory(&cmd, (uint64_t)(uintptr_t)&src[17], (uint64_t)(uintptr_t)&dst[41], 13,
                    VK_ADDRESS_COPY_SPARSE_BIT_KHR, VK_ADDRESS_COPY_SPARSE_BIT_KHR);
   assert(emitted_count == 1);
   assert(emitted[0].backend == BACKEND_COMPUTE_COPY);
   assert(!emitted[0].use_16B_copy);
   assert(memcmp(&dst[41], &src[17], 13) == 0);
   for (unsigned i = 0; i < 41; i++)
      assert(dst[i] == 0xa5);
   for (unsigned i = 54; i < sizeof(dst); i++)
      assert(dst[i] == 0xa5);
}

static void
test_empty_and_sticky_error(void)
{
   begin_case(RADV_QUEUE_COMPUTE);
   radv_copy_memory(&cmd, UINT64_C(0x100000000), UINT64_C(0x200000000), 0,
                    VK_ADDRESS_COPY_DEVICE_LOCAL_BIT_KHR, VK_ADDRESS_COPY_DEVICE_LOCAL_BIT_KHR);
   radv_fill_memory(&cmd, UINT64_C(0x100000000), 0, 0x12345678,
                    VK_ADDRESS_COPY_DEVICE_LOCAL_BIT_KHR);
   assert(emitted_count == 0);
   assert(sdma_count == 0);

   begin_case(RADV_QUEUE_TRANSFER);
   cmd.vk.record_result = VK_ERROR_DEVICE_LOST;
   radv_copy_memory(&cmd, UINT64_C(0x100000000), UINT64_C(0x200000000), UINT64_C(1) << 33,
                    VK_ADDRESS_COPY_DEVICE_LOCAL_BIT_KHR, VK_ADDRESS_COPY_DEVICE_LOCAL_BIT_KHR);
   radv_fill_memory(&cmd, UINT64_C(0x100000000), UINT64_C(1) << 33, 0x12345678,
                    VK_ADDRESS_COPY_DEVICE_LOCAL_BIT_KHR);
   assert(sdma_count == 0);
   assert(emitted_count == 0);
   assert(cmd.vk.record_result == VK_ERROR_DEVICE_LOST);
}

static void
test_large_copy_chunks(void)
{
   const uint64_t four_gib = UINT64_C(1) << 32;
   const uint64_t eight_gib = UINT64_C(1) << 33;
   const uint64_t src = UINT64_C(0x123456789ab0);
   const uint64_t dst = UINT64_C(0x223456789ac0);
   const uint64_t exact[] = {RADV_DARWIN_TRANSFER_CHUNK_SIZE};
   const uint64_t plus_tail[] = {RADV_DARWIN_TRANSFER_CHUNK_SIZE, 16};
   const uint64_t four_gib_chunks[] = {RADV_DARWIN_TRANSFER_CHUNK_SIZE, 16};
   const uint64_t eight_gib_chunks[] = {
      RADV_DARWIN_TRANSFER_CHUNK_SIZE,
      RADV_DARWIN_TRANSFER_CHUNK_SIZE,
      0x20,
   };

   check_compute_copy_chunks(src, dst, RADV_DARWIN_TRANSFER_CHUNK_SIZE,
                             VK_ADDRESS_COPY_DEVICE_LOCAL_BIT_KHR, exact, 1);
   check_compute_copy_chunks(src, dst, RADV_DARWIN_TRANSFER_CHUNK_SIZE + 16,
                             VK_ADDRESS_COPY_DEVICE_LOCAL_BIT_KHR, plus_tail, 2);
   check_compute_copy_chunks(src, dst, four_gib, VK_ADDRESS_COPY_DEVICE_LOCAL_BIT_KHR,
                             four_gib_chunks, 2);
   check_compute_copy_chunks(src, dst, eight_gib, VK_ADDRESS_COPY_DEVICE_LOCAL_BIT_KHR,
                             eight_gib_chunks, 3);

   const uint64_t byte_exact[] = {RADV_DARWIN_BYTE_COPY_CHUNK_SIZE, 0x30};
   const uint64_t byte_eight_gib_chunks[] = {
      RADV_DARWIN_BYTE_COPY_CHUNK_SIZE,
      RADV_DARWIN_BYTE_COPY_CHUNK_SIZE,
      0x80,
   };
   check_compute_copy_chunks(src + 1, dst + 3, RADV_DARWIN_TRANSFER_CHUNK_SIZE,
                             VK_ADDRESS_COPY_SPARSE_BIT_KHR, byte_exact, 2);
   check_compute_copy_chunks(src + 1, dst + 3, eight_gib,
                             VK_ADDRESS_COPY_SPARSE_BIT_KHR, byte_eight_gib_chunks, 3);

   assert((uint64_t)RADV_DARWIN_TRANSFER_CHUNK_SIZE + 63 > UINT32_MAX);
   assert((uint64_t)RADV_DARWIN_BYTE_COPY_CHUNK_SIZE + 63 == UINT32_MAX);
}

static void
test_large_fill_chunks(void)
{
   const uint64_t four_gib = UINT64_C(1) << 32;
   const uint64_t eight_gib = UINT64_C(1) << 33;
   const uint64_t dst = UINT64_C(0x323456789ac0);
   const uint64_t exact[] = {RADV_DARWIN_TRANSFER_CHUNK_SIZE};
   const uint64_t four_gib_chunks[] = {RADV_DARWIN_TRANSFER_CHUNK_SIZE, 16};
   const uint64_t eight_gib_chunks[] = {
      RADV_DARWIN_TRANSFER_CHUNK_SIZE,
      RADV_DARWIN_TRANSFER_CHUNK_SIZE,
      0x20,
   };

   check_compute_fill_chunks(dst, RADV_DARWIN_TRANSFER_CHUNK_SIZE,
                             VK_ADDRESS_COPY_DEVICE_LOCAL_BIT_KHR, exact, 1);
   check_compute_fill_chunks(dst, four_gib, VK_ADDRESS_COPY_DEVICE_LOCAL_BIT_KHR,
                             four_gib_chunks, 2);
   check_compute_fill_chunks(dst, eight_gib, VK_ADDRESS_COPY_DEVICE_LOCAL_BIT_KHR,
                             eight_gib_chunks, 3);
}

static void
test_tail_backend_selection(void)
{
   const uint64_t eight_gib = UINT64_C(1) << 33;
   const uint64_t src = UINT64_C(0x123456789ab0);
   const uint64_t dst = UINT64_C(0x223456789ac0);

   begin_case(RADV_QUEUE_COMPUTE);
   radv_copy_memory(&cmd, src, dst, eight_gib, 0, 0);
   assert(emitted_count == 3);
   assert(emitted[0].backend == BACKEND_COMPUTE_COPY);
   assert(emitted[0].size == RADV_DARWIN_TRANSFER_CHUNK_SIZE);
   assert(emitted[1].backend == BACKEND_COMPUTE_COPY);
   assert(emitted[1].size == RADV_DARWIN_TRANSFER_CHUNK_SIZE);
   assert(emitted[2].backend == BACKEND_CP_COPY);
   assert(emitted[2].size == 0x20);
   assert(emitted[2].src_addr == src + 2 * RADV_DARWIN_TRANSFER_CHUNK_SIZE);
   assert(emitted[2].dst_addr == dst + 2 * RADV_DARWIN_TRANSFER_CHUNK_SIZE);

   begin_case(RADV_QUEUE_COMPUTE);
   const uint32_t returned_flush = radv_fill_memory(&cmd, dst, eight_gib, 0x10203040, 0);
   assert(emitted_count == 3);
   assert(emitted[0].backend == BACKEND_COMPUTE_FILL);
   assert(emitted[0].size == RADV_DARWIN_TRANSFER_CHUNK_SIZE);
   assert(emitted[1].backend == BACKEND_COMPUTE_FILL);
   assert(emitted[1].size == RADV_DARWIN_TRANSFER_CHUNK_SIZE);
   assert(emitted[2].backend == BACKEND_CP_FILL);
   assert(emitted[2].size == 0x20);
   assert(returned_flush == (AC_BARRIER_SYNC_CS | AC_BARRIER_INV_VMEM | 0x20 | 0x40));
}

static void
test_midstream_failure(void)
{
   const uint64_t eight_gib = UINT64_C(1) << 33;
   const uint64_t src = UINT64_C(0x123456789ab0);
   const uint64_t dst = UINT64_C(0x223456789ac0);

   begin_case(RADV_QUEUE_COMPUTE);
   pipeline_failure = 2;
   radv_copy_memory(&cmd, src, dst, eight_gib,
                    VK_ADDRESS_COPY_DEVICE_LOCAL_BIT_KHR, VK_ADDRESS_COPY_DEVICE_LOCAL_BIT_KHR);
   assert(pipeline_lookup_count == 2);
   assert(emitted_count == 1);
   assert(emitted[0].size == RADV_DARWIN_TRANSFER_CHUNK_SIZE);
   assert(cmd.vk.record_result == VK_ERROR_OUT_OF_DEVICE_MEMORY);

   radv_copy_memory(&cmd, src, dst, eight_gib,
                    VK_ADDRESS_COPY_DEVICE_LOCAL_BIT_KHR, VK_ADDRESS_COPY_DEVICE_LOCAL_BIT_KHR);
   assert(pipeline_lookup_count == 2);
   assert(emitted_count == 1);

   begin_case(RADV_QUEUE_COMPUTE);
   pipeline_failure = 2;
   radv_fill_memory(&cmd, dst, eight_gib, 0xabcdef01,
                    VK_ADDRESS_COPY_DEVICE_LOCAL_BIT_KHR);
   assert(pipeline_lookup_count == 2);
   assert(emitted_count == 1);
   assert(emitted[0].size == RADV_DARWIN_TRANSFER_CHUNK_SIZE);
   assert(cmd.vk.record_result == VK_ERROR_OUT_OF_DEVICE_MEMORY);

   radv_fill_memory(&cmd, dst, eight_gib, 0xabcdef01,
                    VK_ADDRESS_COPY_DEVICE_LOCAL_BIT_KHR);
   assert(pipeline_lookup_count == 2);
   assert(emitted_count == 1);
}

int
main(void)
{
   test_small_cpu_transfers();
   test_empty_and_sticky_error();
   test_large_copy_chunks();
   test_large_fill_chunks();
   test_tail_backend_selection();
   test_midstream_failure();
   puts("large-buffer-transfer: production chunking, 64-bit addresses, shader limits, tails and failure PASS");
   return 0;
}
