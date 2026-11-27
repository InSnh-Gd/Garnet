/*
 * SPDX-License-Identifier: MIT
 *
 * Standalone Vulkan 1.x/1.4 Sparse Resources & GFX12 PRT residency test.
 *
 * Verifies:
 *   - Physical device sparse capability & property reporting
 *   - Sparse buffer residency: non-resident zero-read / discard-write,
 *     resident bit-exact readback, and atomic rebind/unbind transitions
 *   - Sparse 2D image residency, non-resident zero-read / discard-write, and rebind
 *   - Sparse 3D image residency, non-resident zero-read / discard-write, and unbind
 *   - Sparse MSAA (2x / 4x / 8x) image residency, clear+resolve, and unbind
 *
 * Build (Linux/macOS with Vulkan headers + loader):
 *   cc -std=c11 -O2 -Wall -Wextra -Werror mesa-patches/tests/vulkan_sparse_prt_test.c \
 *      -I"$VK_INC" -L"$VK_LIB" -lvulkan -o vulkan_sparse_prt_test
 *
 * Options:
 *   --expect-unsupported   Exit 0 only if the selected device does NOT advertise
 *                          sparseBinding (useful for ABI <= 1.12 regression checks).
 *   --require-strict       Fail if residencyNonResidentStrict == 0 (for GFX12 PRT
 *                          hardware acceptance where zero-read/discard-write is mandatory).
 *   --device-index N       Select physical device N (default 0).
 */

#include <vulkan/vulkan.h>

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EXIT_SKIP_UNSUPPORTED 77

#define VK_CHECK(expr)                                                                             \
   do {                                                                                            \
      VkResult _r = (expr);                                                                        \
      if (_r != VK_SUCCESS) {                                                                      \
         fprintf(stderr, "FAIL %s:%d: %s returned VkResult %d\n", __FILE__, __LINE__, #expr,       \
                 (int)_r);                                                                         \
         exit(1);                                                                                  \
      }                                                                                            \
   } while (0)

#define REQUIRE(cond, msg)                                                                         \
   do {                                                                                            \
      if (!(cond)) {                                                                               \
         fprintf(stderr, "FAIL %s:%d: %s (%s)\n", __FILE__, __LINE__, (msg), #cond);               \
         exit(1);                                                                                  \
      }                                                                                            \
   } while (0)

struct vk_ctx {
   VkInstance instance;
   VkPhysicalDevice phys;
   VkPhysicalDeviceProperties props;
   VkPhysicalDeviceFeatures feats;
   VkPhysicalDeviceMemoryProperties mem_props;
   VkDevice device;
   uint32_t gfx_qf;
   uint32_t sparse_qf;
   VkQueue gfx_queue;
   VkQueue sparse_queue;
   VkCommandPool cmd_pool;
   VkCommandBuffer cmd;
   VkFence fence;
};

static uint32_t
find_memory_type(const struct vk_ctx *c, uint32_t type_bits, VkMemoryPropertyFlags want)
{
   for (uint32_t i = 0; i < c->mem_props.memoryTypeCount; i++) {
      if ((type_bits & (1u << i)) && (c->mem_props.memoryTypes[i].propertyFlags & want) == want)
         return i;
   }
   /* Fallback: any compatible memory type if DEVICE_LOCAL was requested on a software ICD. */
   if (want == VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
      for (uint32_t i = 0; i < c->mem_props.memoryTypeCount; i++) {
         if (type_bits & (1u << i))
            return i;
      }
   }
   fprintf(stderr, "FAIL: no memory type matching bits=0x%x flags=0x%x\n", type_bits, want);
   exit(1);
}

static VkDeviceMemory
alloc_mem(const struct vk_ctx *c, VkDeviceSize size, uint32_t type_bits, VkMemoryPropertyFlags flags)
{
   VkMemoryAllocateInfo ai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = size,
      .memoryTypeIndex = find_memory_type(c, type_bits, flags),
   };
   VkDeviceMemory mem = VK_NULL_HANDLE;
   VK_CHECK(vkAllocateMemory(c->device, &ai, NULL, &mem));
   return mem;
}

struct staging_buf {
   VkBuffer buf;
   VkDeviceMemory mem;
   void *ptr;
   VkDeviceSize size;
};

static struct staging_buf
staging_create(const struct vk_ctx *c, VkDeviceSize size)
{
   struct staging_buf s = {.size = size};
   VkBufferCreateInfo bci = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = size,
      .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   VK_CHECK(vkCreateBuffer(c->device, &bci, NULL, &s.buf));
   VkMemoryRequirements req;
   vkGetBufferMemoryRequirements(c->device, s.buf, &req);
   s.mem = alloc_mem(c, req.size, req.memoryTypeBits,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
   VK_CHECK(vkBindBufferMemory(c->device, s.buf, s.mem, 0));
   VK_CHECK(vkMapMemory(c->device, s.mem, 0, size, 0, &s.ptr));
   return s;
}

static void
staging_destroy(const struct vk_ctx *c, struct staging_buf *s)
{
   if (s->ptr)
      vkUnmapMemory(c->device, s->mem);
   vkDestroyBuffer(c->device, s->buf, NULL);
   vkFreeMemory(c->device, s->mem, NULL);
   memset(s, 0, sizeof(*s));
}

static void
begin_cmd(const struct vk_ctx *c)
{
   VK_CHECK(vkResetCommandBuffer(c->cmd, 0));
   VkCommandBufferBeginInfo bi = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
   };
   VK_CHECK(vkBeginCommandBuffer(c->cmd, &bi));
}

static void
submit_and_wait(const struct vk_ctx *c)
{
   VK_CHECK(vkEndCommandBuffer(c->cmd));
   VK_CHECK(vkResetFences(c->device, 1, &c->fence));
   VkSubmitInfo si = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &c->cmd,
   };
   VK_CHECK(vkQueueSubmit(c->gfx_queue, 1, &si, c->fence));
   VK_CHECK(vkWaitForFences(c->device, 1, &c->fence, VK_TRUE, UINT64_MAX));
}

static void
bind_sparse_and_wait(const struct vk_ctx *c, const VkBindSparseInfo *bsi)
{
   VK_CHECK(vkResetFences(c->device, 1, &c->fence));
   VK_CHECK(vkQueueBindSparse(c->sparse_queue, 1, bsi, c->fence));
   VK_CHECK(vkWaitForFences(c->device, 1, &c->fence, VK_TRUE, UINT64_MAX));
}

static void
cmd_img_barrier(VkCommandBuffer cmd, VkImage img, VkImageLayout old_l, VkImageLayout new_l,
                VkAccessFlags src_a, VkAccessFlags dst_a, VkPipelineStageFlags src_s,
                VkPipelineStageFlags dst_s)
{
   VkImageMemoryBarrier b = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .srcAccessMask = src_a,
      .dstAccessMask = dst_a,
      .oldLayout = old_l,
      .newLayout = new_l,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = img,
      .subresourceRange = {
         .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
         .baseMipLevel = 0,
         .levelCount = 1,
         .baseArrayLayer = 0,
         .layerCount = 1,
      },
   };
   vkCmdPipelineBarrier(cmd, src_s, dst_s, 0, 0, NULL, 0, NULL, 1, &b);
}

static uint64_t
align_up_u64(uint64_t v, uint64_t a)
{
   return a ? ((v + a - 1) / a) * a : v;
}

static uint32_t
max_u32(uint32_t a, uint32_t b)
{
   return a > b ? a : b;
}

static uint32_t
pattern_dw(uint32_t tag, uint32_t idx)
{
   uint32_t v = tag ^ (idx * 2654435761u + 0x13579bdfu);
   return v ? v : 0x1u;
}

static void
verify_dwords_zero(const uint32_t *p, uint32_t count, const char *label)
{
   for (uint32_t i = 0; i < count; i++) {
      if (p[i] != 0u) {
         fprintf(stderr, "FAIL %s: expected 0 at dword %u, got 0x%08x\n", label, i, p[i]);
         exit(1);
      }
   }
}

static void
verify_dwords_nonresident(const struct vk_ctx *c, const uint32_t *p, uint32_t count, const char *label)
{
   if (!c->props.sparseProperties.residencyNonResidentStrict)
      return;
   verify_dwords_zero(p, count, label);
}

static void
verify_dwords_pattern(const uint32_t *p, uint32_t count, uint32_t tag, const char *label)
{
   for (uint32_t i = 0; i < count; i++) {
      uint32_t want = pattern_dw(tag, i);
      if (p[i] != want) {
         fprintf(stderr, "FAIL %s: mismatch at dword %u: want 0x%08x got 0x%08x\n", label, i, want,
                 p[i]);
         exit(1);
      }
   }
}

/* ---- Test 1: Sparse Buffer residency, non-resident zero-read/discard-write, and rebinding ---- */
static void
test_sparse_buffer(const struct vk_ctx *c)
{
   if (!c->feats.sparseResidencyBuffer) {
      printf("SKIP sparse_buffer: sparseResidencyBuffer=0\n");
      return;
   }

   /* Probe alignment using a 256 KiB sparse buffer first. */
   VkBufferCreateInfo bci = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .flags = VK_BUFFER_CREATE_SPARSE_BINDING_BIT | VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT,
      .size = 256u * 1024u,
      .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
   };
   VkBuffer probe = VK_NULL_HANDLE;
   VK_CHECK(vkCreateBuffer(c->device, &bci, NULL, &probe));
   VkMemoryRequirements req;
   vkGetBufferMemoryRequirements(c->device, probe, &req);
   vkDestroyBuffer(c->device, probe, NULL);

   const VkDeviceSize tile = align_up_u64(65536u, req.alignment ? req.alignment : 65536u);
   const VkDeviceSize total = tile * 4u;
   bci.size = total;

   VkBuffer sbuf = VK_NULL_HANDLE;
   VK_CHECK(vkCreateBuffer(c->device, &bci, NULL, &sbuf));
   vkGetBufferMemoryRequirements(c->device, sbuf, &req);

   VkDeviceMemory memA = alloc_mem(c, tile, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
   VkDeviceMemory memB = alloc_mem(c, tile, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
   struct staging_buf src = staging_create(c, total);
   struct staging_buf dst = staging_create(c, total);
   const uint32_t tile_dws = (uint32_t)(tile / sizeof(uint32_t));

   /* Phase 1: bind block 1 -> memA; leave blocks 0, 2, 3 non-resident. */
   VkSparseMemoryBind bind1 = {
      .resourceOffset = tile * 1u,
      .size = tile,
      .memory = memA,
      .memoryOffset = 0,
   };
   VkSparseBufferMemoryBindInfo bbi1 = {.buffer = sbuf, .bindCount = 1, .pBinds = &bind1};
   VkBindSparseInfo bsi1 = {
      .sType = VK_STRUCTURE_TYPE_BIND_SPARSE_INFO,
      .bufferBindCount = 1,
      .pBufferBinds = &bbi1,
   };
   bind_sparse_and_wait(c, &bsi1);

   uint32_t *sp = src.ptr;
   uint32_t *dp = dst.ptr;
   for (uint32_t b = 0; b < 4u; b++) {
      for (uint32_t i = 0; i < tile_dws; i++)
         sp[b * tile_dws + i] = pattern_dw(0xA1000000u | (b << 16), i);
   }
   memset(dp, 0xDE, (size_t)total);

   begin_cmd(c);
   VkBufferCopy cp = {.srcOffset = 0, .dstOffset = 0, .size = total};
   vkCmdCopyBuffer(c->cmd, src.buf, sbuf, 1, &cp);
   VkMemoryBarrier mb = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
   };
   vkCmdPipelineBarrier(c->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                        1, &mb, 0, NULL, 0, NULL);
   vkCmdCopyBuffer(c->cmd, sbuf, dst.buf, 1, &cp);
   submit_and_wait(c);

   verify_dwords_nonresident(c, dp + 0u * tile_dws, tile_dws, "sparse_buffer phase1 block0 non-resident");
   verify_dwords_pattern(dp + 1u * tile_dws, tile_dws, 0xA1010000u, "sparse_buffer phase1 block1 resident");
   verify_dwords_nonresident(c, dp + 2u * tile_dws, tile_dws, "sparse_buffer phase1 block2 non-resident");
   verify_dwords_nonresident(c, dp + 3u * tile_dws, tile_dws, "sparse_buffer phase1 block3 non-resident");

   /* Phase 2: unbind block 1 -> NULL (PRT), rebind block 2 -> memA, bind block 3 -> memB. */
   VkSparseMemoryBind binds2[3] = {
      {.resourceOffset = tile * 1u, .size = tile, .memory = VK_NULL_HANDLE, .memoryOffset = 0},
      {.resourceOffset = tile * 2u, .size = tile, .memory = memA, .memoryOffset = 0},
      {.resourceOffset = tile * 3u, .size = tile, .memory = memB, .memoryOffset = 0},
   };
   VkSparseBufferMemoryBindInfo bbi2 = {.buffer = sbuf, .bindCount = 3, .pBinds = binds2};
   VkBindSparseInfo bsi2 = {
      .sType = VK_STRUCTURE_TYPE_BIND_SPARSE_INFO,
      .bufferBindCount = 1,
      .pBufferBinds = &bbi2,
   };
   bind_sparse_and_wait(c, &bsi2);

   /* Write to block 1 (now non-resident -> must discard) and block 3 (memB -> must persist). */
   for (uint32_t i = 0; i < tile_dws; i++) {
      sp[1u * tile_dws + i] = pattern_dw(0x55AA0001u, i);
      sp[3u * tile_dws + i] = pattern_dw(0x55AA0003u, i);
   }
   memset(dp, 0xDE, (size_t)total);

   begin_cmd(c);
   VkBufferCopy cp_w[2] = {
      {.srcOffset = tile * 1u, .dstOffset = tile * 1u, .size = tile},
      {.srcOffset = tile * 3u, .dstOffset = tile * 3u, .size = tile},
   };
   vkCmdCopyBuffer(c->cmd, src.buf, sbuf, 2, cp_w);
   vkCmdPipelineBarrier(c->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                        1, &mb, 0, NULL, 0, NULL);
   vkCmdCopyBuffer(c->cmd, sbuf, dst.buf, 1, &cp);
   submit_and_wait(c);

   verify_dwords_nonresident(c, dp + 0u * tile_dws, tile_dws, "sparse_buffer phase2 block0 non-resident");
   verify_dwords_nonresident(c, dp + 1u * tile_dws, tile_dws, "sparse_buffer phase2 block1 unbound PRT");
   verify_dwords_pattern(dp + 2u * tile_dws, tile_dws, 0xA1010000u, "sparse_buffer phase2 block2 rebound memA");
   verify_dwords_pattern(dp + 3u * tile_dws, tile_dws, 0x55AA0003u, "sparse_buffer phase2 block3 bound memB");

   /* Phase 3: unbind remaining blocks cleanly. */
   VkSparseMemoryBind binds3[2] = {
      {.resourceOffset = tile * 2u, .size = tile, .memory = VK_NULL_HANDLE, .memoryOffset = 0},
      {.resourceOffset = tile * 3u, .size = tile, .memory = VK_NULL_HANDLE, .memoryOffset = 0},
   };
   VkSparseBufferMemoryBindInfo bbi3 = {.buffer = sbuf, .bindCount = 2, .pBinds = binds3};
   VkBindSparseInfo bsi3 = {
      .sType = VK_STRUCTURE_TYPE_BIND_SPARSE_INFO,
      .bufferBindCount = 1,
      .pBufferBinds = &bbi3,
   };
   bind_sparse_and_wait(c, &bsi3);

   staging_destroy(c, &dst);
   staging_destroy(c, &src);
   vkDestroyBuffer(c->device, sbuf, NULL);
   vkFreeMemory(c->device, memB, NULL);
   vkFreeMemory(c->device, memA, NULL);
   printf("PASS sparse_buffer (tile=%" PRIu64 " B, strict=%u, resident/rebind%s)\n", (uint64_t)tile,
          c->props.sparseProperties.residencyNonResidentStrict,
          c->props.sparseProperties.residencyNonResidentStrict ? " + zero-read/discard-write"
                                                               : " [nonResidentStrict=0]");
}

/* ---- Test 2: Sparse 2D Image residency, non-resident zero-read/discard-write, and rebinding ---- */
static void
test_sparse_image_2d(const struct vk_ctx *c)
{
   if (!c->feats.sparseResidencyImage2D) {
      printf("SKIP sparse_image_2d: sparseResidencyImage2D=0\n");
      return;
   }

   const VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
   const VkImageUsageFlags usage =
      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
   uint32_t fmt_cnt = 0;
   vkGetPhysicalDeviceSparseImageFormatProperties(c->phys, fmt, VK_IMAGE_TYPE_2D,
                                                  VK_SAMPLE_COUNT_1_BIT, usage,
                                                  VK_IMAGE_TILING_OPTIMAL, &fmt_cnt, NULL);
   REQUIRE(fmt_cnt > 0, "sparse 2D R8G8B8A8_UNORM format properties must exist");
   VkSparseImageFormatProperties fmt_props[4];
   if (fmt_cnt > 4)
      fmt_cnt = 4;
   vkGetPhysicalDeviceSparseImageFormatProperties(c->phys, fmt, VK_IMAGE_TYPE_2D,
                                                  VK_SAMPLE_COUNT_1_BIT, usage,
                                                  VK_IMAGE_TILING_OPTIMAL, &fmt_cnt, fmt_props);
   const VkExtent3D gran = fmt_props[0].imageGranularity;
   REQUIRE(gran.width > 0 && gran.height > 0 && gran.depth == 1, "valid 2D sparse tile granularity");

   const uint32_t img_w = (uint32_t)align_up_u64(max_u32(512u, gran.width * 4u), gran.width);
   const uint32_t img_h = (uint32_t)align_up_u64(max_u32(512u, gran.height * 4u), gran.height);

   VkImageCreateInfo ici = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .flags = VK_IMAGE_CREATE_SPARSE_BINDING_BIT | VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = fmt,
      .extent = {img_w, img_h, 1u},
      .mipLevels = 1u,
      .arrayLayers = 1u,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = usage,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   VkImage img = VK_NULL_HANDLE;
   VK_CHECK(vkCreateImage(c->device, &ici, NULL, &img));

   VkMemoryRequirements mem_req;
   vkGetImageMemoryRequirements(c->device, img, &mem_req);
   uint32_t sreq_cnt = 0;
   vkGetImageSparseMemoryRequirements(c->device, img, &sreq_cnt, NULL);
   REQUIRE(sreq_cnt > 0, "2D sparse memory requirements must exist");
   VkSparseImageMemoryRequirements sreq[4];
   if (sreq_cnt > 4)
      sreq_cnt = 4;
   vkGetImageSparseMemoryRequirements(c->device, img, &sreq_cnt, sreq);
   REQUIRE(sreq[0].imageMipTailFirstLod >= 1u, "mip 0 of 4x4-tile 2D image must not be in mip tail");

   const VkDeviceSize tile_bytes = align_up_u64(65536u, mem_req.alignment ? mem_req.alignment : 65536u);
   VkDeviceMemory tile_mem =
      alloc_mem(c, tile_bytes, mem_req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

   const uint32_t tile_px = gran.width * gran.height;
   const VkDeviceSize copy_bytes = (VkDeviceSize)tile_px * 4u;
   struct staging_buf src = staging_create(c, copy_bytes * 2u);
   struct staging_buf dst = staging_create(c, copy_bytes * 2u);
   uint32_t *sp = src.ptr;
   uint32_t *dp = dst.ptr;
   for (uint32_t i = 0; i < tile_px; i++) {
      sp[i] = pattern_dw(0x2D000000u, i);
      sp[tile_px + i] = pattern_dw(0x2D010000u, i);
   }

   /* Phase 1: bind tile (1, 0), leave tile (0, 0) non-resident. */
   VkSparseImageMemoryBind ibind1 = {
      .subresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .arrayLayer = 0},
      .offset = {(int32_t)gran.width, 0, 0},
      .extent = gran,
      .memory = tile_mem,
      .memoryOffset = 0,
   };
   VkSparseImageMemoryBindInfo ibbi1 = {.image = img, .bindCount = 1, .pBinds = &ibind1};
   VkBindSparseInfo bsi1 = {
      .sType = VK_STRUCTURE_TYPE_BIND_SPARSE_INFO,
      .imageBindCount = 1,
      .pImageBinds = &ibbi1,
   };
   bind_sparse_and_wait(c, &bsi1);

   VkBufferImageCopy regions[2] = {
      {
         .bufferOffset = 0,
         .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1},
         .imageOffset = {0, 0, 0},
         .imageExtent = gran,
      },
      {
         .bufferOffset = copy_bytes,
         .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1},
         .imageOffset = {(int32_t)gran.width, 0, 0},
         .imageExtent = gran,
      },
   };

   memset(dp, 0xDE, (size_t)(copy_bytes * 2u));
   begin_cmd(c);
   cmd_img_barrier(c->cmd, img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                   VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                   VK_PIPELINE_STAGE_TRANSFER_BIT);
   vkCmdCopyBufferToImage(c->cmd, src.buf, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 2, regions);
   cmd_img_barrier(c->cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                   VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                   VK_PIPELINE_STAGE_TRANSFER_BIT);
   vkCmdCopyImageToBuffer(c->cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.buf, 2, regions);
   submit_and_wait(c);

   verify_dwords_nonresident(c, dp, tile_px, "sparse_image_2d phase1 tile(0,0) non-resident");
   verify_dwords_pattern(dp + tile_px, tile_px, 0x2D010000u, "sparse_image_2d phase1 tile(1,0) resident");

   /* Phase 2: unbind tile (1, 0) -> NULL, rebind tile (0, 0) -> tile_mem. */
   VkSparseImageMemoryBind ibinds2[2] = {
      {
         .subresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .arrayLayer = 0},
         .offset = {(int32_t)gran.width, 0, 0},
         .extent = gran,
         .memory = VK_NULL_HANDLE,
      },
      {
         .subresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .arrayLayer = 0},
         .offset = {0, 0, 0},
         .extent = gran,
         .memory = tile_mem,
      },
   };
   VkSparseImageMemoryBindInfo ibbi2 = {.image = img, .bindCount = 2, .pBinds = ibinds2};
   VkBindSparseInfo bsi2 = {
      .sType = VK_STRUCTURE_TYPE_BIND_SPARSE_INFO,
      .imageBindCount = 1,
      .pImageBinds = &ibbi2,
   };
   bind_sparse_and_wait(c, &bsi2);

   memset(dp, 0xDE, (size_t)(copy_bytes * 2u));
   begin_cmd(c);
   vkCmdCopyImageToBuffer(c->cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.buf, 2, regions);
   submit_and_wait(c);

   verify_dwords_pattern(dp, tile_px, 0x2D010000u, "sparse_image_2d phase2 tile(0,0) rebound mem");
   verify_dwords_nonresident(c, dp + tile_px, tile_px, "sparse_image_2d phase2 tile(1,0) unbound PRT");

   /* Clean unbind */
   ibinds2[1].memory = VK_NULL_HANDLE;
   ibbi2.bindCount = 1;
   ibbi2.pBinds = &ibinds2[1];
   bind_sparse_and_wait(c, &bsi2);

   staging_destroy(c, &dst);
   staging_destroy(c, &src);
   vkDestroyImage(c->device, img, NULL);
   vkFreeMemory(c->device, tile_mem, NULL);
   printf("PASS sparse_image_2d (%ux%u, gran=%ux%u, strict=%u, resident/rebind%s)\n", img_w, img_h,
          gran.width, gran.height, c->props.sparseProperties.residencyNonResidentStrict,
          c->props.sparseProperties.residencyNonResidentStrict ? " + zero-read/discard-write"
                                                               : " [nonResidentStrict=0]");
}

/* ---- Test 3: Sparse 3D Image residency, non-resident zero-read/discard-write, and unbind ---- */
static void
test_sparse_image_3d(const struct vk_ctx *c)
{
   if (!c->feats.sparseResidencyImage3D) {
      printf("SKIP sparse_image_3d: sparseResidencyImage3D=0\n");
      return;
   }

   const VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
   const VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
   uint32_t fmt_cnt = 0;
   vkGetPhysicalDeviceSparseImageFormatProperties(c->phys, fmt, VK_IMAGE_TYPE_3D,
                                                  VK_SAMPLE_COUNT_1_BIT, usage,
                                                  VK_IMAGE_TILING_OPTIMAL, &fmt_cnt, NULL);
   REQUIRE(fmt_cnt > 0, "sparse 3D R8G8B8A8_UNORM format properties must exist");
   VkSparseImageFormatProperties fmt_props[4];
   if (fmt_cnt > 4)
      fmt_cnt = 4;
   vkGetPhysicalDeviceSparseImageFormatProperties(c->phys, fmt, VK_IMAGE_TYPE_3D,
                                                  VK_SAMPLE_COUNT_1_BIT, usage,
                                                  VK_IMAGE_TILING_OPTIMAL, &fmt_cnt, fmt_props);
   const VkExtent3D gran = fmt_props[0].imageGranularity;
   REQUIRE(gran.width > 0 && gran.height > 0 && gran.depth > 0, "valid 3D sparse tile granularity");

   const uint32_t img_w = (uint32_t)align_up_u64(max_u32(128u, gran.width * 4u), gran.width);
   const uint32_t img_h = (uint32_t)align_up_u64(max_u32(128u, gran.height * 4u), gran.height);
   const uint32_t img_d = (uint32_t)align_up_u64(max_u32(64u, gran.depth * 4u), gran.depth);

   VkImageCreateInfo ici = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .flags = VK_IMAGE_CREATE_SPARSE_BINDING_BIT | VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT,
      .imageType = VK_IMAGE_TYPE_3D,
      .format = fmt,
      .extent = {img_w, img_h, img_d},
      .mipLevels = 1u,
      .arrayLayers = 1u,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = usage,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   VkImage img = VK_NULL_HANDLE;
   VK_CHECK(vkCreateImage(c->device, &ici, NULL, &img));

   VkMemoryRequirements mem_req;
   vkGetImageMemoryRequirements(c->device, img, &mem_req);
   uint32_t sreq_cnt = 0;
   vkGetImageSparseMemoryRequirements(c->device, img, &sreq_cnt, NULL);
   REQUIRE(sreq_cnt > 0, "3D sparse memory requirements must exist");
   VkSparseImageMemoryRequirements sreq[4];
   if (sreq_cnt > 4)
      sreq_cnt = 4;
   vkGetImageSparseMemoryRequirements(c->device, img, &sreq_cnt, sreq);
   REQUIRE(sreq[0].imageMipTailFirstLod >= 1u, "mip 0 of 3D sparse image must not be in mip tail");

   const VkDeviceSize tile_bytes = align_up_u64(65536u, mem_req.alignment ? mem_req.alignment : 65536u);
   VkDeviceMemory tile_mem =
      alloc_mem(c, tile_bytes, mem_req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

   const uint32_t tile_vox = gran.width * gran.height * gran.depth;
   const VkDeviceSize copy_bytes = (VkDeviceSize)tile_vox * 4u;
   struct staging_buf src = staging_create(c, copy_bytes * 2u);
   struct staging_buf dst = staging_create(c, copy_bytes * 2u);
   uint32_t *sp = src.ptr;
   uint32_t *dp = dst.ptr;
   for (uint32_t i = 0; i < tile_vox; i++) {
      sp[i] = pattern_dw(0x3D000000u, i);
      sp[tile_vox + i] = pattern_dw(0x3D010000u, i);
   }

   VkSparseImageMemoryBind ibind = {
      .subresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .arrayLayer = 0},
      .offset = {(int32_t)gran.width, 0, 0},
      .extent = gran,
      .memory = tile_mem,
   };
   VkSparseImageMemoryBindInfo ibbi = {.image = img, .bindCount = 1, .pBinds = &ibind};
   VkBindSparseInfo bsi = {
      .sType = VK_STRUCTURE_TYPE_BIND_SPARSE_INFO,
      .imageBindCount = 1,
      .pImageBinds = &ibbi,
   };
   bind_sparse_and_wait(c, &bsi);

   VkBufferImageCopy regions[2] = {
      {
         .bufferOffset = 0,
         .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1},
         .imageOffset = {0, 0, 0},
         .imageExtent = gran,
      },
      {
         .bufferOffset = copy_bytes,
         .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1},
         .imageOffset = {(int32_t)gran.width, 0, 0},
         .imageExtent = gran,
      },
   };

   memset(dp, 0xDE, (size_t)(copy_bytes * 2u));
   begin_cmd(c);
   cmd_img_barrier(c->cmd, img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                   VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                   VK_PIPELINE_STAGE_TRANSFER_BIT);
   vkCmdCopyBufferToImage(c->cmd, src.buf, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 2, regions);
   cmd_img_barrier(c->cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                   VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                   VK_PIPELINE_STAGE_TRANSFER_BIT);
   vkCmdCopyImageToBuffer(c->cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.buf, 2, regions);
   submit_and_wait(c);

   verify_dwords_nonresident(c, dp, tile_vox, "sparse_image_3d tile(0,0,0) non-resident");
   verify_dwords_pattern(dp + tile_vox, tile_vox, 0x3D010000u, "sparse_image_3d tile(1,0,0) resident");

   /* Unbind tile (1, 0, 0) and confirm non-resident zero-read. */
   ibind.memory = VK_NULL_HANDLE;
   bind_sparse_and_wait(c, &bsi);
   memset(dp, 0xDE, (size_t)(copy_bytes * 2u));
   begin_cmd(c);
   vkCmdCopyImageToBuffer(c->cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.buf, 2, regions);
   submit_and_wait(c);
   verify_dwords_nonresident(c, dp + tile_vox, tile_vox, "sparse_image_3d tile(1,0,0) unbound PRT");

   staging_destroy(c, &dst);
   staging_destroy(c, &src);
   vkDestroyImage(c->device, img, NULL);
   vkFreeMemory(c->device, tile_mem, NULL);
   printf("PASS sparse_image_3d (%ux%ux%u, gran=%ux%ux%u, strict=%u, resident/unbind%s)\n", img_w,
          img_h, img_d, gran.width, gran.height, gran.depth,
          c->props.sparseProperties.residencyNonResidentStrict,
          c->props.sparseProperties.residencyNonResidentStrict ? " + zero-read/discard-write"
                                                               : " [nonResidentStrict=0]");
}

/* ---- Test 4: Sparse MSAA (2x/4x/8x) Image residency via clear + hardware resolve ---- */
static void
test_sparse_image_msaa_one(const struct vk_ctx *c, VkSampleCountFlagBits samples, uint32_t n)
{
   const VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
   const VkImageUsageFlags msaa_usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
   uint32_t fmt_cnt = 0;
   vkGetPhysicalDeviceSparseImageFormatProperties(c->phys, fmt, VK_IMAGE_TYPE_2D, samples,
                                                  msaa_usage, VK_IMAGE_TILING_OPTIMAL, &fmt_cnt,
                                                  NULL);
   if (fmt_cnt == 0) {
      printf("SKIP sparse_image_msaa_%ux: format properties unavailable\n", n);
      return;
   }
   VkSparseImageFormatProperties fmt_props[4];
   if (fmt_cnt > 4)
      fmt_cnt = 4;
   vkGetPhysicalDeviceSparseImageFormatProperties(c->phys, fmt, VK_IMAGE_TYPE_2D, samples,
                                                  msaa_usage, VK_IMAGE_TILING_OPTIMAL, &fmt_cnt,
                                                  fmt_props);
   const VkExtent3D gran = fmt_props[0].imageGranularity;
   REQUIRE(gran.width > 0 && gran.height > 0, "valid MSAA sparse tile granularity");

   const uint32_t img_w = (uint32_t)align_up_u64(max_u32(256u, gran.width * 4u), gran.width);
   const uint32_t img_h = (uint32_t)align_up_u64(max_u32(256u, gran.height * 4u), gran.height);

   VkImageCreateInfo msaa_ci = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .flags = VK_IMAGE_CREATE_SPARSE_BINDING_BIT | VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = fmt,
      .extent = {img_w, img_h, 1u},
      .mipLevels = 1u,
      .arrayLayers = 1u,
      .samples = samples,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = msaa_usage,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   VkImage msaa_img = VK_NULL_HANDLE;
   VK_CHECK(vkCreateImage(c->device, &msaa_ci, NULL, &msaa_img));

   VkMemoryRequirements mem_req;
   vkGetImageMemoryRequirements(c->device, msaa_img, &mem_req);
   const VkDeviceSize tile_bytes = align_up_u64(65536u, mem_req.alignment ? mem_req.alignment : 65536u);
   VkDeviceMemory tile_mem =
      alloc_mem(c, tile_bytes, mem_req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

   /* Non-sparse 1x resolve target */
   VkImageCreateInfo res_ci = msaa_ci;
   res_ci.flags = 0;
   res_ci.samples = VK_SAMPLE_COUNT_1_BIT;
   res_ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
   VkImage res_img = VK_NULL_HANDLE;
   VK_CHECK(vkCreateImage(c->device, &res_ci, NULL, &res_img));
   VkMemoryRequirements res_req;
   vkGetImageMemoryRequirements(c->device, res_img, &res_req);
   VkDeviceMemory res_mem =
      alloc_mem(c, res_req.size, res_req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
   VK_CHECK(vkBindImageMemory(c->device, res_img, res_mem, 0));

   VkImageViewCreateInfo vci = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = fmt,
      .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1},
   };
   VkImageView msaa_view = VK_NULL_HANDLE, res_view = VK_NULL_HANDLE;
   vci.image = msaa_img;
   VK_CHECK(vkCreateImageView(c->device, &vci, NULL, &msaa_view));
   vci.image = res_img;
   VK_CHECK(vkCreateImageView(c->device, &vci, NULL, &res_view));

   VkAttachmentDescription atts[2] = {
      {
         .format = fmt,
         .samples = samples,
         .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
         .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
         .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
         .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
         .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
      },
      {
         .format = fmt,
         .samples = VK_SAMPLE_COUNT_1_BIT,
         .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
         .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
         .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
         .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
         .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      },
   };
   VkAttachmentReference color_ref = {.attachment = 0, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
   VkAttachmentReference res_ref = {.attachment = 1, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
   VkSubpassDescription subpass = {
      .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
      .colorAttachmentCount = 1,
      .pColorAttachments = &color_ref,
      .pResolveAttachments = &res_ref,
   };
   VkSubpassDependency dep = {
      .srcSubpass = 0,
      .dstSubpass = VK_SUBPASS_EXTERNAL,
      .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
      .dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT,
      .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
   };
   VkRenderPassCreateInfo rpci = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
      .attachmentCount = 2,
      .pAttachments = atts,
      .subpassCount = 1,
      .pSubpasses = &subpass,
      .dependencyCount = 1,
      .pDependencies = &dep,
   };
   VkRenderPass rp = VK_NULL_HANDLE;
   VK_CHECK(vkCreateRenderPass(c->device, &rpci, NULL, &rp));

   VkImageView fb_views[2] = {msaa_view, res_view};
   VkFramebufferCreateInfo fbci = {
      .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
      .renderPass = rp,
      .attachmentCount = 2,
      .pAttachments = fb_views,
      .width = img_w,
      .height = img_h,
      .layers = 1u,
   };
   VkFramebuffer fb = VK_NULL_HANDLE;
   VK_CHECK(vkCreateFramebuffer(c->device, &fbci, NULL, &fb));

   const uint32_t tile_px = gran.width * gran.height;
   const VkDeviceSize copy_bytes = (VkDeviceSize)tile_px * 4u;
   struct staging_buf dst = staging_create(c, copy_bytes * 2u);
   uint8_t *dp = dst.ptr;

   VkBufferImageCopy regions[2] = {
      {
         .bufferOffset = 0,
         .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1},
         .imageOffset = {0, 0, 0},
         .imageExtent = {gran.width, gran.height, 1u},
      },
      {
         .bufferOffset = copy_bytes,
         .imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1},
         .imageOffset = {(int32_t)gran.width, 0, 0},
         .imageExtent = {gran.width, gran.height, 1u},
      },
   };

   /* Phase 1: bind tile (1, 0), leave tile (0, 0) non-resident; clear MSAA and resolve. */
   VkSparseImageMemoryBind ibind = {
      .subresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .arrayLayer = 0},
      .offset = {(int32_t)gran.width, 0, 0},
      .extent = {gran.width, gran.height, 1u},
      .memory = tile_mem,
   };
   VkSparseImageMemoryBindInfo ibbi = {.image = msaa_img, .bindCount = 1, .pBinds = &ibind};
   VkBindSparseInfo bsi = {
      .sType = VK_STRUCTURE_TYPE_BIND_SPARSE_INFO,
      .imageBindCount = 1,
      .pImageBinds = &ibbi,
   };
   bind_sparse_and_wait(c, &bsi);

   VkClearValue clr = {.color = {.float32 = {0.25f, 0.50f, 0.75f, 1.00f}}};
   VkRenderPassBeginInfo rpbi = {
      .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
      .renderPass = rp,
      .framebuffer = fb,
      .renderArea = {.offset = {0, 0}, .extent = {img_w, img_h}},
      .clearValueCount = 1,
      .pClearValues = &clr,
   };

   memset(dp, 0xDE, (size_t)(copy_bytes * 2u));
   begin_cmd(c);
   vkCmdBeginRenderPass(c->cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
   vkCmdEndRenderPass(c->cmd);
   vkCmdCopyImageToBuffer(c->cmd, res_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.buf, 2, regions);
   submit_and_wait(c);

   verify_dwords_nonresident(c, (const uint32_t *)dp, tile_px, "sparse_msaa tile(0,0) non-resident");
   const uint8_t want_rgba[4] = {64u, 128u, 191u, 255u};
   const uint8_t *t1 = dp + copy_bytes;
   for (uint32_t i = 0; i < tile_px; i++) {
      for (uint32_t ch = 0; ch < 4u; ch++) {
         int diff = (int)t1[i * 4u + ch] - (int)want_rgba[ch];
         if (diff < -1 || diff > 1) {
            fprintf(stderr, "FAIL sparse_msaa_%ux tile(1,0) px %u ch %u: want %u got %u\n", n, i,
                    ch, want_rgba[ch], t1[i * 4u + ch]);
            exit(1);
         }
      }
   }

   /* Phase 2: unbind tile (1, 0), clear + resolve again -> tile (1, 0) must now be all zero. */
   ibind.memory = VK_NULL_HANDLE;
   bind_sparse_and_wait(c, &bsi);

   memset(dp, 0xDE, (size_t)(copy_bytes * 2u));
   begin_cmd(c);
   vkCmdBeginRenderPass(c->cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
   vkCmdEndRenderPass(c->cmd);
   vkCmdCopyImageToBuffer(c->cmd, res_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.buf, 2, regions);
   submit_and_wait(c);

   verify_dwords_nonresident(c, (const uint32_t *)(dp + copy_bytes), tile_px,
                             "sparse_msaa tile(1,0) unbound PRT");

   staging_destroy(c, &dst);
   vkDestroyFramebuffer(c->device, fb, NULL);
   vkDestroyRenderPass(c->device, rp, NULL);
   vkDestroyImageView(c->device, res_view, NULL);
   vkDestroyImageView(c->device, msaa_view, NULL);
   vkDestroyImage(c->device, res_img, NULL);
   vkFreeMemory(c->device, res_mem, NULL);
   vkDestroyImage(c->device, msaa_img, NULL);
   vkFreeMemory(c->device, tile_mem, NULL);
   printf("PASS sparse_image_msaa_%ux (%ux%u, gran=%ux%u, clear+resolve + unbind)\n", n, img_w,
          img_h, gran.width, gran.height);
}

static void
test_sparse_images_msaa(const struct vk_ctx *c)
{
   if (c->feats.sparseResidency2Samples)
      test_sparse_image_msaa_one(c, VK_SAMPLE_COUNT_2_BIT, 2u);
   else
      printf("SKIP sparse_image_msaa_2x: sparseResidency2Samples=0\n");

   if (c->feats.sparseResidency4Samples)
      test_sparse_image_msaa_one(c, VK_SAMPLE_COUNT_4_BIT, 4u);
   else
      printf("SKIP sparse_image_msaa_4x: sparseResidency4Samples=0\n");

   if (c->feats.sparseResidency8Samples)
      test_sparse_image_msaa_one(c, VK_SAMPLE_COUNT_8_BIT, 8u);
   else
      printf("SKIP sparse_image_msaa_8x: sparseResidency8Samples=0\n");
}

int
main(int argc, char **argv)
{
   bool expect_unsupported = false;
   bool require_strict = false;
   uint32_t device_index = 0;
   for (int i = 1; i < argc; i++) {
      if (strcmp(argv[i], "--expect-unsupported") == 0)
         expect_unsupported = true;
      else if (strcmp(argv[i], "--require-strict") == 0)
         require_strict = true;
      else if (strcmp(argv[i], "--device-index") == 0 && i + 1 < argc)
         device_index = (uint32_t)strtoul(argv[++i], NULL, 10);
      else {
         fprintf(stderr, "usage: %s [--expect-unsupported] [--require-strict] [--device-index N]\n",
                 argv[0]);
         return 2;
      }
   }

   struct vk_ctx c;
   memset(&c, 0, sizeof(c));

   VkApplicationInfo app = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "vulkan_sparse_prt_test",
      .apiVersion = VK_API_VERSION_1_1,
   };
   VkInstanceCreateInfo ici = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &app,
   };
   VK_CHECK(vkCreateInstance(&ici, NULL, &c.instance));

   uint32_t dev_count = 0;
   VK_CHECK(vkEnumeratePhysicalDevices(c.instance, &dev_count, NULL));
   REQUIRE(dev_count > 0, "at least one Vulkan physical device is required");
   REQUIRE(device_index < dev_count, "requested --device-index out of range");
   VkPhysicalDevice *devs = calloc(dev_count, sizeof(*devs));
   REQUIRE(devs != NULL, "alloc physical device list");
   VK_CHECK(vkEnumeratePhysicalDevices(c.instance, &dev_count, devs));
   c.phys = devs[device_index];
   free(devs);

   vkGetPhysicalDeviceProperties(c.phys, &c.props);
   vkGetPhysicalDeviceFeatures(c.phys, &c.feats);
   vkGetPhysicalDeviceMemoryProperties(c.phys, &c.mem_props);

   printf("device[%u]: %s (api=%u.%u.%u vendor=0x%04x device=0x%04x)\n", device_index,
          c.props.deviceName, VK_API_VERSION_MAJOR(c.props.apiVersion),
          VK_API_VERSION_MINOR(c.props.apiVersion), VK_API_VERSION_PATCH(c.props.apiVersion),
          c.props.vendorID, c.props.deviceID);
   printf("sparse features: binding=%u buf=%u img2D=%u img3D=%u 2x=%u 4x=%u 8x=%u aliased=%u\n",
          c.feats.sparseBinding, c.feats.sparseResidencyBuffer, c.feats.sparseResidencyImage2D,
          c.feats.sparseResidencyImage3D, c.feats.sparseResidency2Samples,
          c.feats.sparseResidency4Samples, c.feats.sparseResidency8Samples,
          c.feats.sparseResidencyAliased);
   printf("sparse props: addressSpaceSize=%" PRIu64 " nonResidentStrict=%u\n",
          (uint64_t)c.props.limits.sparseAddressSpaceSize,
          c.props.sparseProperties.residencyNonResidentStrict);

   if (!c.feats.sparseBinding) {
      printf("SPARSE_UNSUPPORTED: device '%s' does not expose sparseBinding\n", c.props.deviceName);
      vkDestroyInstance(c.instance, NULL);
      return expect_unsupported ? 0 : EXIT_SKIP_UNSUPPORTED;
   }
   if (expect_unsupported) {
      fprintf(stderr, "FAIL: --expect-unsupported was set, but device advertises sparseBinding=1\n");
      vkDestroyInstance(c.instance, NULL);
      return 1;
   }
   if (require_strict && !c.props.sparseProperties.residencyNonResidentStrict) {
      fprintf(stderr,
              "FAIL: --require-strict was set, but device reports residencyNonResidentStrict=0\n");
      vkDestroyInstance(c.instance, NULL);
      return 1;
   }

   REQUIRE(c.props.limits.sparseAddressSpaceSize > 0,
           "sparseAddressSpaceSize must be nonzero when sparseBinding=1");

   uint32_t qf_count = 0;
   vkGetPhysicalDeviceQueueFamilyProperties(c.phys, &qf_count, NULL);
   VkQueueFamilyProperties *qfs = calloc(qf_count, sizeof(*qfs));
   REQUIRE(qfs != NULL && qf_count > 0, "queue families available");
   vkGetPhysicalDeviceQueueFamilyProperties(c.phys, &qf_count, qfs);

   c.gfx_qf = UINT32_MAX;
   c.sparse_qf = UINT32_MAX;
   for (uint32_t i = 0; i < qf_count; i++) {
      if (c.gfx_qf == UINT32_MAX && (qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT))
         c.gfx_qf = i;
      if ((qfs[i].queueFlags & VK_QUEUE_SPARSE_BINDING_BIT) &&
          (c.sparse_qf == UINT32_MAX || i == c.gfx_qf))
         c.sparse_qf = i;
   }
   free(qfs);
   REQUIRE(c.gfx_qf != UINT32_MAX, "graphics queue family required");
   REQUIRE(c.sparse_qf != UINT32_MAX, "VK_QUEUE_SPARSE_BINDING_BIT queue family required");

   float prio = 1.0f;
   VkDeviceQueueCreateInfo qcis[2];
   uint32_t qci_count = 0;
   qcis[qci_count++] = (VkDeviceQueueCreateInfo){
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = c.gfx_qf,
      .queueCount = 1u,
      .pQueuePriorities = &prio,
   };
   if (c.sparse_qf != c.gfx_qf) {
      qcis[qci_count++] = (VkDeviceQueueCreateInfo){
         .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
         .queueFamilyIndex = c.sparse_qf,
         .queueCount = 1u,
         .pQueuePriorities = &prio,
      };
   }

   VkPhysicalDeviceFeatures enable = {
      .sparseBinding = c.feats.sparseBinding,
      .sparseResidencyBuffer = c.feats.sparseResidencyBuffer,
      .sparseResidencyImage2D = c.feats.sparseResidencyImage2D,
      .sparseResidencyImage3D = c.feats.sparseResidencyImage3D,
      .sparseResidency2Samples = c.feats.sparseResidency2Samples,
      .sparseResidency4Samples = c.feats.sparseResidency4Samples,
      .sparseResidency8Samples = c.feats.sparseResidency8Samples,
      .sparseResidencyAliased = c.feats.sparseResidencyAliased,
   };
   VkDeviceCreateInfo dci = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = qci_count,
      .pQueueCreateInfos = qcis,
      .pEnabledFeatures = &enable,
   };
   VK_CHECK(vkCreateDevice(c.phys, &dci, NULL, &c.device));
   vkGetDeviceQueue(c.device, c.gfx_qf, 0, &c.gfx_queue);
   vkGetDeviceQueue(c.device, c.sparse_qf, 0, &c.sparse_queue);

   VkCommandPoolCreateInfo cpci = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
      .queueFamilyIndex = c.gfx_qf,
   };
   VK_CHECK(vkCreateCommandPool(c.device, &cpci, NULL, &c.cmd_pool));
   VkCommandBufferAllocateInfo cbai = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = c.cmd_pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1u,
   };
   VK_CHECK(vkAllocateCommandBuffers(c.device, &cbai, &c.cmd));
   VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   VK_CHECK(vkCreateFence(c.device, &fci, NULL, &c.fence));

   test_sparse_buffer(&c);
   test_sparse_image_2d(&c);
   test_sparse_image_3d(&c);
   test_sparse_images_msaa(&c);

   VK_CHECK(vkDeviceWaitIdle(c.device));
   vkDestroyFence(c.device, c.fence, NULL);
   vkDestroyCommandPool(c.device, c.cmd_pool, NULL);
   vkDestroyDevice(c.device, NULL);
   vkDestroyInstance(c.instance, NULL);

   printf("VULKAN_SPARSE_PRT_TEST_PASS device='%s'\n", c.props.deviceName);
   return 0;
}
