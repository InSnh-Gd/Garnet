// Replay sampler_mixed from test-sampler-usage.m through the production ABI9 shader.
#include "test-vulkan-replay.h"

static float expected_compare(unsigned function, float depth) {
    switch (function) {
        case 0: return 0;
        case 1: return 0.5f < depth;
        case 2: return 0.5f == depth;
        case 3: return 0.5f <= depth;
        case 4: return 0.5f > depth;
        case 5: return 0.5f != depth;
        case 6: return 0.5f >= depth;
        case 7: return 1;
    }
    require(0, "unknown comparison function"); return -1;
}

int main(int argc, const char **argv) {
    if (argc != 3) { fprintf(stderr, "Usage: mixed-replay VULKAN_LIBRARY MIXED_SPV\n"); return 2; }
    alarm(60); setvbuf(stdout, NULL, _IONBF, 0);
    void *library; VkQueue queue; uint32_t family;
    VkInstance instance = new_backend(argv[1], 1, &library, &queue, &family);
    enum { OUTPUT, ARGUMENTS, ADDRESSES, TOKENS, STAGE, UPLOAD, NBUFFERS };
    Buffer buffers[NBUFFERS];
    for (unsigned i = 0; i < NBUFFERS; ++i) buffers[i] = new_buffer(1024);
    memset(buffers[ARGUMENTS].map, 0xa5, 1024);
    const uint64_t color_token = UINT64_C(0x100000007), depth_token = UINT64_C(0x200000007);
    uint64_t arguments[10] = {color_token, depth_token};
    for (unsigned i = 0; i < 8; ++i) arguments[2 + i] = (UINT64_C(1) << 32) + 100 + i;
    memcpy((uint8_t *)buffers[ARGUMENTS].map + 256, arguments, sizeof arguments);
    rdna4_buffer_table *addresses = buffers[ADDRESSES].map;
    require(rdna4_buffer_table_set(addresses, 0, buffers[OUTPUT].address, 1024, 256, 0) &&
        rdna4_buffer_table_set(addresses, 3, buffers[ARGUMENTS].address, 1024, 256, 1), "BDA table setup");
    // ABI9: seven header words, three token lists including dummy, then coordinate flags.
    uint32_t *tokens = buffers[TOKENS].map;
    tokens[0] = 7; tokens[1] = 3; tokens[2] = 13; tokens[3] = 1; tokens[4] = 15; tokens[5] = 9;
    float lod_limit = 16; memcpy(tokens + 6, &lod_limit, sizeof lod_limit);
    memcpy(tokens + 9, &color_token, 8); memcpy(tokens + 11, &depth_token, 8);
    for (unsigned i = 0; i < 8; ++i) memcpy(tokens + 17 + i * 2, &arguments[2 + i], 8);
    // Coordinate flags are zero for normalized samplers; null shape ordinals follow live images.
    uint32_t *stage = buffers[STAGE].map;
    for (unsigned i = 32; i < 96; ++i) stage[i] = UINT32_MAX;
    stage[32] = 0; stage[33] = 1;
    Image images[4] = {
        new_image(VK_FORMAT_R32G32B32A32_SFLOAT, 1, 1, VK_IMAGE_ASPECT_COLOR_BIT),
        new_image(VK_FORMAT_R32G32B32A32_SFLOAT, 1, 1, VK_IMAGE_ASPECT_COLOR_BIT),
        new_image(VK_FORMAT_D32_SFLOAT, 2, 2, VK_IMAGE_ASPECT_DEPTH_BIT),
        new_image(VK_FORMAT_D32_SFLOAT, 2, 2, VK_IMAGE_ASPECT_DEPTH_BIT)};
    const float color[4] = {0.375f, 0.625f, 0.875f, 1};
    const float depths[4] = {0.25f, 0.75f, 0.75f, 0.25f};
    memcpy((uint8_t *)buffers[UPLOAD].map + 16, color, sizeof color);
    memcpy((uint8_t *)buffers[UPLOAD].map + 32, depths, sizeof depths);
    VkSampler samplers[18];
    for (unsigned i = 0; i < 18; ++i) {
        unsigned logical = i % 9;
        VkSamplerCreateInfo info = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
            .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .compareEnable = i >= 9, .compareOp = logical ? (VkCompareOp)(logical - 1) : VK_COMPARE_OP_NEVER };
        CHECK(vkCreateSampler(device, &info, NULL, &samplers[i]));
    }
    VkDescriptorSetLayout layouts[5];
    const uint32_t binding_numbers[] = {640, 672, 674, 707};
    VkDescriptorSetLayoutBinding bindings[4];
    for (unsigned i = 0; i < 4; ++i) bindings[i] = (VkDescriptorSetLayoutBinding){
        binding_numbers[i], VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    for (unsigned i = 0; i < 5; ++i) {
        VkDescriptorSetLayoutBinding array = {0, i == 4 ? VK_DESCRIPTOR_TYPE_SAMPLER : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            i == 4 ? 18 : 5, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
        VkDescriptorBindingFlags variable = VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT;
        VkDescriptorSetLayoutBindingFlagsCreateInfo flags = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
            .bindingCount = 1, .pBindingFlags = &variable };
        VkDescriptorSetLayoutCreateInfo info = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .pNext = i == 2 || i == 4 ? &flags : NULL, .bindingCount = i == 0 ? 4 : i == 2 || i == 4 ? 1 : 0,
            .pBindings = i == 0 ? bindings : &array };
        CHECK(vkCreateDescriptorSetLayout(device, &info, NULL, &layouts[i]));
    }
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4}, {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 5}, {VK_DESCRIPTOR_TYPE_SAMPLER, 18}};
    VkDescriptorPoolCreateInfo pool_info = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 5, .poolSizeCount = 3, .pPoolSizes = sizes };
    VkDescriptorPool pool; CHECK(vkCreateDescriptorPool(device, &pool_info, NULL, &pool));
    uint32_t variable_counts[] = {0, 0, 5, 0, 18};
    VkDescriptorSetVariableDescriptorCountAllocateInfo variable_info = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO,
        .descriptorSetCount = 5, .pDescriptorCounts = variable_counts };
    VkDescriptorSetAllocateInfo sets_info = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .pNext = &variable_info,
        .descriptorPool = pool, .descriptorSetCount = 5, .pSetLayouts = layouts };
    VkDescriptorSet sets[5]; CHECK(vkAllocateDescriptorSets(device, &sets_info, sets));
    const unsigned buffer_indices[] = {ADDRESSES, TOKENS, STAGE, ARGUMENTS};
    const unsigned buffer_lengths[] = {768, 168, 384, 80};
    VkDescriptorBufferInfo buffer_info[4]; VkWriteDescriptorSet writes[6];
    for (unsigned i = 0; i < 4; ++i) {
        buffer_info[i] = (VkDescriptorBufferInfo){buffers[buffer_indices[i]].buffer, i == 3 ? 256 : 0, buffer_lengths[i]};
        writes[i] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sets[0],
            .dstBinding = binding_numbers[i], .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo = &buffer_info[i] };
    }
    VkDescriptorImageInfo image_info[5], sampler_info[18];
    const unsigned image_indices[] = {0, 1, 2, 0, 3};
    for (unsigned i = 0; i < 5; ++i)
        image_info[i] = (VkDescriptorImageInfo){VK_NULL_HANDLE, images[image_indices[i]].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    for (unsigned i = 0; i < 18; ++i)
        sampler_info[i] = (VkDescriptorImageInfo){samplers[i], VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
    writes[4] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sets[2], .descriptorCount = 5,
        .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = image_info };
    writes[5] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sets[4], .descriptorCount = 18,
        .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER, .pImageInfo = sampler_info };
    vkUpdateDescriptorSets(device, 6, writes, 0, NULL);
    VkPushConstantRange push_range = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 48};
    VkPipelineLayoutCreateInfo layout_info = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 5, .pSetLayouts = layouts, .pushConstantRangeCount = 1, .pPushConstantRanges = &push_range };
    VkPipelineLayout layout; CHECK(vkCreatePipelineLayout(device, &layout_info, NULL, &layout));
    VkPipelineCacheCreateInfo cache_info = { .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
    VkPipelineCache cache; CHECK(vkCreatePipelineCache(device, &cache_info, NULL, &cache));
    VkCommandPoolCreateInfo command_pool_info = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = family };
    VkCommandPool command_pool; CHECK(vkCreateCommandPool(device, &command_pool_info, NULL, &command_pool));
    for (unsigned round = 0; round < 8; ++round) {
        unsigned mode = round / 2;
        unsigned output_count = mode == 0 ? 64 : mode == 1 ? 31 : 0;
        addresses->length[0] = mode == 1 ? 31 * sizeof(float) : mode == 3 ? 3 : 768;
        addresses->access[0] = RDNA4_BUFFER_READ | (mode == 2 ? 0 : RDNA4_BUFFER_WRITE);
        if (round && !(round & 1)) {
            vkDestroyPipelineCache(device, cache, NULL);
            CHECK(vkCreatePipelineCache(device, &cache_info, NULL, &cache));
        }
        VkPipeline consumer = pipeline(argv[2], layout, cache);
        memset(buffers[OUTPUT].map, 0xcd, 1024);
        VkCommandBufferAllocateInfo command_info = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = command_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
        VkCommandBuffer command; CHECK(vkAllocateCommandBuffers(device, &command_info, &command));
        VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        CHECK(vkBeginCommandBuffer(command, &begin));
        barrier(command, VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT);
        if (!round) for (unsigned i = 0; i < 4; ++i) {
            VkImageAspectFlags aspect = i >= 2 ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
            VkImageMemoryBarrier transition = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = images[i].image,
                .subresourceRange = {aspect, 0, 1, 0, 1} };
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &transition);
            VkBufferImageCopy copy = { .bufferOffset = i >= 2 ? 32 : i * 16,
                .imageSubresource = {aspect, 0, 0, 1}, .imageExtent = {i >= 2 ? 2 : 1, i >= 2 ? 2 : 1, 1} };
            vkCmdCopyBufferToImage(command, buffers[UPLOAD].buffer, images[i].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            transition.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; transition.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            transition.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; transition.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &transition);
        }
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 5, sets, 0, NULL);
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, consumer);
        uint32_t push[12] = {32, 1, 1, 0, 0, 0, 0, 0, 0, 1, 1, 1};
        vkCmdPushConstants(command, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, push);
        vkCmdDispatch(command, 1, 1, 1);
        barrier(command, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
        CHECK(vkEndCommandBuffer(command));
        VkFenceCreateInfo fence_info = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        VkFence fence; CHECK(vkCreateFence(device, &fence_info, NULL, &fence));
        VkSubmitInfo submit = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &command };
        CHECK(vkQueueSubmit(queue, 1, &submit, fence));
        CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_C(10000000000)));
        vkDestroyFence(device, fence, NULL);
        const float *output = (float *)((uint8_t *)buffers[OUTPUT].map + 256);
        for (unsigned i = 0; i < output_count; ++i) {
            float expected = i & 1 ? expected_compare(i / 8, depths[(i / 2) % 4]) : 0.375f;
            if (output[i] != expected)
                fprintf(stderr, "round=%u word=%u value=%g expected=%g\n", round, i, output[i], expected);
            require(output[i] == expected, "mixed sample output mismatch");
        }
        for (unsigned i = 0; i < 1024; ++i) {
            if (i < 256 || i >= 256 + output_count * sizeof(float))
                require(((uint8_t *)buffers[OUTPUT].map)[i] == 0xcd, "output guard modified");
            uint8_t expected = i >= 256 && i < 336 ? ((uint8_t *)arguments)[i - 256] : 0xa5;
            require(((uint8_t *)buffers[ARGUMENTS].map)[i] == expected, "arguments or guard modified");
        }
        printf("MIXED_SAMPLER_REPLAY_ROUND_PASS mode=%u pipeline=%s outputs=%u guards=1\n",
            mode, round & 1 ? "warm" : "cold", output_count);
        vkFreeCommandBuffers(device, command_pool, 1, &command);
        vkDestroyPipeline(device, consumer, NULL);
    }
    vkDestroyCommandPool(device, command_pool, NULL); vkDestroyPipelineCache(device, cache, NULL);
    vkDestroyPipelineLayout(device, layout, NULL); vkDestroyDescriptorPool(device, pool, NULL);
    for (unsigned i = 0; i < 5; ++i) vkDestroyDescriptorSetLayout(device, layouts[i], NULL);
    for (unsigned i = 0; i < 18; ++i) vkDestroySampler(device, samplers[i], NULL);
    for (unsigned i = 0; i < 4; ++i) {
        vkDestroyImageView(device, images[i].view, NULL); vkDestroyImage(device, images[i].image, NULL);
        vkFreeMemory(device, images[i].allocation, NULL);
    }
    for (unsigned i = 0; i < NBUFFERS; ++i) {
        vkUnmapMemory(device, buffers[i].allocation); vkDestroyBuffer(device, buffers[i].buffer, NULL);
        vkFreeMemory(device, buffers[i].allocation, NULL);
    }
    vkDestroyDevice(device, NULL); vkDestroyInstance(instance, NULL); dlclose(library);
    puts("MIXED_SAMPLER_REPLAY_PASS cpu_backend=1 rounds=8 outputs=190 bounds=4");
    return 0;
}
