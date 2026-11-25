#include "test-vulkan-replay.h"
#include "garnet_indirect_compute.h"
#include "garnet_luma_workgroup.h"
#include <stdbool.h>

static void execute_luma(VkQueue queue, uint32_t family, VkDescriptorSetLayout set,
                         VkPipelineLayout layout, VkPipeline pipeline, bool sum, uint32_t local_x) {
    enum { width = 16, height = 4 };
    const uint32_t uniforms[] = {0, sum ? 0u : (3u << 16) | 13u, sum ? 4u : 0u, sum ? 4u : 0u, 0};
    const float sumInput[] = {1,2,3,1, 4,5,6,1, 7,8,9,1, 10,11,12,1};
    const float expectedSum[] = {22, 26, 30, 4};
    const uint16_t texel[] = {0x3c00, 0x4000, 0x4200, 0x3c00};
    const size_t inputBytes = sum ? sizeof(sumInput) : width * height * sizeof(texel);
    const size_t outputBytes = sizeof(expectedSum) * (sum ? 1 : 2 * height);
    Buffer buffers[] = {new_buffer(sizeof(uniforms)), new_buffer(inputBytes), new_buffer(outputBytes)};
    Image image = {0};
    memcpy(buffers[0].map, uniforms, sizeof(uniforms));
    if (sum) memcpy(buffers[1].map, sumInput, inputBytes);
    else {
        image = new_image(VK_FORMAT_R16G16B16A16_SFLOAT, width, height, VK_IMAGE_ASPECT_COLOR_BIT);
        for (size_t offset = 0; offset < inputBytes; offset += sizeof(texel))
            memcpy((uint8_t *)buffers[1].map + offset, texel, sizeof(texel));
    }
    memset(buffers[2].map, 0xEE, outputBytes);
    VkDescriptorBufferInfo infos[] = {{buffers[0].buffer, 0, sizeof(uniforms)},
        {buffers[1].buffer, 0, inputBytes}, {buffers[2].buffer, 0, outputBytes}};
    const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, sum ? 3u : 2u},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1}};
    VkDescriptorPoolCreateInfo poolInfo = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1, .poolSizeCount = sum ? 1u : 2u, .pPoolSizes = sizes};
    VkDescriptorPool pool;
    CHECK(vkCreateDescriptorPool(device, &poolInfo, NULL, &pool));
    VkDescriptorSetAllocateInfo setInfo = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = pool, .descriptorSetCount = 1, .pSetLayouts = &set};
    VkDescriptorSet descriptors;
    CHECK(vkAllocateDescriptorSets(device, &setInfo, &descriptors));
    VkWriteDescriptorSet writes[3];
    for (uint32_t binding = 0; binding < 3; ++binding) {
        writes[binding] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = descriptors, .dstBinding = binding, .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &infos[binding]};
    }
    const VkDescriptorImageInfo imageInfo = {VK_NULL_HANDLE, image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    if (!sum) writes[1] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = descriptors, .dstBinding = 32, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = &imageInfo};
    vkUpdateDescriptorSets(device, 3, writes, 0, NULL);
    VkCommandPoolCreateInfo commandPoolInfo = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = family};
    VkCommandPool commandPool;
    CHECK(vkCreateCommandPool(device, &commandPoolInfo, NULL, &commandPool));
    VkCommandBufferAllocateInfo commandInfo = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = commandPool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer command;
    CHECK(vkAllocateCommandBuffers(device, &commandInfo, &command));
    const VkCommandBufferBeginInfo beginInfo = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CHECK(vkBeginCommandBuffer(command, &beginInfo));
    if (!sum) {
        VkImageMemoryBarrier layoutBarrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = image.image,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, NULL, 0, NULL, 1, &layoutBarrier);
        const VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .imageExtent = {width, height, 1}};
        vkCmdCopyBufferToImage(command, buffers[1].buffer, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        layoutBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        layoutBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        layoutBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        layoutBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 0, NULL, 0, NULL, 1, &layoutBarrier);
    }
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &descriptors, 0, NULL);
    if (!sum) {
        const uint32_t grid[] = {2 * local_x, height, 1, 0, 0, 0, 0, 0, 0, 2, height, 1};
        vkCmdPushConstants(command, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(grid), grid);
    }
    vkCmdDispatch(command, sum ? 1u : 2u, sum ? 1u : height, 1);
    barrier(command, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
    CHECK(vkEndCommandBuffer(command));
    const VkFenceCreateInfo fenceInfo = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    CHECK(vkCreateFence(device, &fenceInfo, NULL, &fence));
    const VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &command};
    CHECK(vkQueueSubmit(queue, 1, &submit, fence));
    CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_C(10000000000)));
    if (sum) require(!memcmp(buffers[2].map, expectedSum, sizeof(expectedSum)), "luma sum differs from the CPU reference");
    else {
        const float *output = buffers[2].map;
        for (unsigned group = 0; group < 2 * height; ++group) {
            const unsigned start = (group % 2) * local_x * 8;
            const unsigned end = start + local_x * 8 < 13 ? start + local_x * 8 : 13;
            const float count = group / 2 < 3 && start < end ? (float)(end - start) : 0.0f;
            const float expected[] = {count, 2 * count, 3 * count, count};
            require(!memcmp(output + group * 4, expected, sizeof(expected)), "average luma differs from the CPU reference");
        }
        vkDestroyImageView(device, image.view, NULL);
        vkDestroyImage(device, image.image, NULL);
        vkFreeMemory(device, image.allocation, NULL);
    }
    vkDestroyFence(device, fence, NULL);
    vkDestroyCommandPool(device, commandPool, NULL);
    vkDestroyDescriptorPool(device, pool, NULL);
    for (unsigned index = 0; index < 3; ++index) {
        vkUnmapMemory(device, buffers[index].allocation);
        vkDestroyBuffer(device, buffers[index].buffer, NULL);
        vkFreeMemory(device, buffers[index].allocation, NULL);
    }
}

static void replay(const char *path, const char *kernel, uint32_t requested, uint32_t local_x,
                   VkQueue queue, uint32_t family) {
    FILE *file = fopen(path, "rb");
    require(file && !fseek(file, 0, SEEK_END), "luma cache open");
    const long length = ftell(file);
    require(length >= 20 && !(length % 4) && length < 65536, "invalid luma cache size");
    rewind(file);
    uint32_t *original = malloc((size_t)length);
    require(original && fread(original, 1, (size_t)length, file) == (size_t)length, "luma cache read");
    fclose(file);
    const size_t count = (size_t)length / sizeof(*original);
    RDNA4sMod before = {0}, after = {0};
    char why[256];
    require(!rdna4s_parse(original, count, &before, why, sizeof why), "original luma module parse");
    uint32_t *transformed = NULL;
    require(!rdna4_luma_workgroup("unrelated_kernel", &before, original, count, &transformed) && !transformed,
        "unrelated kernel was transformed");
    require(rdna4_luma_workgroup(kernel, &before, original, count, &transformed), "legacy cache cannot honor dynamic threadgroup length");
    require(!rdna4s_parse(transformed, count + 8, &after, why, sizeof why), "specialized luma module parse");
    require(after.bound == before.bound + 1 && !strcmp(before.ep_name, after.ep_name), "entry point changed");

    unsigned arrays = 0, constants = 0, decorations = 0;
    for (uint32_t id = 1; id < before.bound; ++id) {
        if (before.id[id].op != 28 || before.id[id].n == after.id[id].n) continue;
        require(after.id[id].n == before.bound && before.id[before.id[id].n].cval == 512,
                "workgroup length must use the added constant and preserve the old default");
        ++arrays;
    }
    for (size_t pos = 5; pos < count + 8; pos += transformed[pos] >> 16) {
        const uint32_t op = transformed[pos] & 0xffff, size = transformed[pos] >> 16;
        if (op == 50 && size == 4 && transformed[pos + 2] == before.bound && transformed[pos + 3] == 512)
            ++constants;
        if (op == 71 && size == 4 && transformed[pos + 1] == before.bound && transformed[pos + 2] == 1 &&
            transformed[pos + 3] == RDNA4_LUMA_WORKGROUP_SPEC) ++decorations;
    }
    require(arrays == 1 && constants == 1 && decorations == 1, "new specialization must resize exactly one workgroup array");

    RDNA4sRefl *oldLayout = calloc(1, sizeof(*oldLayout)), *newLayout = calloc(1, sizeof(*newLayout));
    require(oldLayout && newLayout && !rdna4s_reflect(&before, 2, oldLayout) && !rdna4s_reflect(&after, 2, newLayout), "luma descriptor reflection");
    require(!memcmp(oldLayout, newLayout, sizeof(*oldLayout)), "luma descriptor or push constant layout changed");
    VkDescriptorSetLayoutBinding bindings[4];
    uint32_t used = 0;
    for (uint32_t index = 0; index < RDNA4S_MAXBIND; ++index) {
        const RDNA4sBind binding = newLayout->bind[0][index];
        if (!binding.used) continue;
        require(used < 4 && binding.count == 1, "unexpected luma descriptors");
        bindings[used++] = (VkDescriptorSetLayoutBinding){index, binding.type, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    }
    VkDescriptorSetLayoutCreateInfo setInfo = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = used, .pBindings = bindings};
    VkDescriptorSetLayout set;
    CHECK(vkCreateDescriptorSetLayout(device, &setInfo, NULL, &set));
    VkPushConstantRange range = {VK_SHADER_STAGE_COMPUTE_BIT, 0, newLayout->pc_size};
    VkPipelineLayoutCreateInfo layoutInfo = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &set, .pushConstantRangeCount = range.size != 0, .pPushConstantRanges = &range};
    VkPipelineLayout layout;
    CHECK(vkCreatePipelineLayout(device, &layoutInfo, NULL, &layout));
    const VkSpecializationMapEntry entries[] = {{0,0,4}, {1,4,4}, {2,8,4}, {RDNA4_LUMA_WORKGROUP_SPEC,12,4}};
    const bool sum = !strcmp(kernel, "compute_sum_luma");
    const uint32_t local[] = {local_x, 1, 1};
    require(rdna4_luma_workgroup_fits(requested, local), "requested workgroup does not cover the local size");
    const bool defaultFits = rdna4_luma_workgroup_fits(512 * RDNA4_LUMA_ELEMENT_BYTES, local);
    for (unsigned indirect = 0; indirect < 2; ++indirect) {
        uint32_t *code = transformed;
        size_t words = count + 8;
        if (indirect) require(!rdna4s_indirect_compute(transformed, words, 0, local, &code, &words, why, sizeof why), why);
        VkShaderModuleCreateInfo shaderInfo = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = words * sizeof(*code), .pCode = code};
        VkShaderModule shader;
        CHECK(vkCreateShaderModule(device, &shaderInfo, NULL, &shader));
        for (unsigned variant = defaultFits ? 0u : 1u; variant < 2; ++variant) {
            const uint32_t data[] = {local[0],1,1,requested / RDNA4_LUMA_ELEMENT_BYTES};
            VkSpecializationInfo specialization = {variant ? 4u : 3u, entries, variant ? 16u : 12u, data};
            VkComputePipelineCreateInfo pipelineInfo = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                    .module = shader, .pName = after.ep_name, .pSpecializationInfo = &specialization}, .layout = layout};
            VkPipeline pipeline;
            CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, NULL, &pipeline));
            if (!indirect) execute_luma(queue, family, set, layout, pipeline, sum, local[0]);
            vkDestroyPipeline(device, pipeline, NULL);
        }
        vkDestroyShaderModule(device, shader, NULL);
        if (indirect) free(code);
    }
    vkDestroyPipelineLayout(device, layout, NULL);
    vkDestroyDescriptorSetLayout(device, set, NULL);
    printf("LUMA_WORKGROUP_PASS kernel=%s requested_bytes=%u local_x=%u default_variant=%d specialized_variant=1 indirect_compiled=1 cpu_oracle=1\n",
        kernel, requested, local_x, defaultFits);
    rdna4s_free(&before); rdna4s_free(&after);
    free(oldLayout); free(newLayout); free(original); free(transformed);
}

int main(int argc, char **argv) {
    if (argc != 4) return 2;
    require(rdna4_luma_workgroup_bytes_ok(16384, 16384) && rdna4_luma_workgroup_bytes_ok(16, 16384) &&
        !rdna4_luma_workgroup_bytes_ok(15, 16384) && !rdna4_luma_workgroup_bytes_ok(16400, 16384), "invalid workgroup length accepted");
    const uint32_t twoThreads[] = {2,1,1};
    require(!rdna4_luma_workgroup_fits(16, twoThreads), "workgroup smaller than the dispatch accepted");
    void *library;
    VkQueue queue;
    uint32_t family;
    VkInstance instance = new_backend(argv[1], 1, &library, &queue, &family);
    replay(argv[2], "compute_average_luma", 16384, 64, queue, family);
    replay(argv[2], "compute_average_luma", 16384, 1024, queue, family);
    replay(argv[3], "compute_sum_luma", 16, 1, queue, family);
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
    dlclose(library);
    return 0;
}
