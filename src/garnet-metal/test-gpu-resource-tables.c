// Execute translated AIR after GPU-only token updates, independently of Metal descriptor setup.
#include "test-vulkan-replay.h"
#include "garnet_buffer_registry.h"

int main(int argc, const char **argv) {
    if (argc != 4) { fprintf(stderr, "Usage: test-gpu-resource-tables VULKAN_LIBRARY CONSUMER_SPV PRODUCER_SPV\n"); return 2; }
    alarm(60); setvbuf(stdout, NULL, _IONBF, 0);
    void *library; VkQueue queue; uint32_t family;
    VkInstance instance = new_backend(argv[1], 0, &library, &queue, &family);
    enum { OUTPUT, CONTROL, ARGUMENTS, SEED, ADDRESSES, TOKENS, DATA0, DATA1, UPLOAD, STAGE, NBUFFERS };
    Buffer buffers[NBUFFERS]; for (unsigned i = 0; i < NBUFFERS; ++i) buffers[i] = new_buffer(1024);
    uint64_t seed[] = { UINT64_C(0x100000007), UINT64_C(0x200000007),
        UINT64_C(0x100000009), UINT64_C(0x200000009), buffers[DATA0].address, buffers[DATA1].address };
    memcpy(buffers[SEED].map, seed, sizeof seed);
    for (unsigned i = 0; i < 32; ++i) {
        ((uint32_t *)buffers[DATA0].map)[i] = 1000 + i;
        ((uint32_t *)buffers[DATA1].map)[i] = 2000 + i;
    }
    rdna4_buffer_table *addresses = buffers[ADDRESSES].map;
    require(rdna4_buffer_table_set(addresses, 0, buffers[OUTPUT].address, 1024, 16, 0) &&
        rdna4_buffer_table_set(addresses, 1, buffers[CONTROL].address, 1024, 0, 1) &&
        rdna4_buffer_table_set(addresses, 3, buffers[ARGUMENTS].address, 1024, 256, 1), "BDA table setup");
    rdna4_buffer_table direct = *addresses;
    size_t registry_bytes;
    require(rdna4_buffer_registry_size(2, 1024, &registry_bytes) &&
        rdna4_buffer_registry_init(addresses, registry_bytes, &direct, 2, 1024) &&
        rdna4_buffer_registry_set(addresses, registry_bytes, 0, buffers[DATA0].address, 1024, 1) &&
        rdna4_buffer_registry_set(addresses, registry_bytes, 1, buffers[DATA1].address, 1024, 1), "BDA registry setup");
    uint32_t table[] = {7, 3, 13, 1, 15, 3, 0, 0, 0, 7, 1, 7, 2, 0, 0, 0, 0, 9, 1, 9, 2};
    memcpy(buffers[TOKENS].map, table, sizeof table);
    uint32_t *stage = buffers[STAGE].map;
    for (unsigned i = 32; i < 96; ++i) stage[i] = UINT32_MAX;
    stage[32] = 0;
    Image images[3]; for (unsigned i = 0; i < 3; ++i) images[i] = new_image(VK_FORMAT_R32G32B32A32_SFLOAT, 2, 1, VK_IMAGE_ASPECT_COLOR_BIT);
    const uint32_t pixels[3][2] = {{0, 0}, {11, 29}, {47, 83}};
    float *upload = buffers[UPLOAD].map;
    for (unsigned i = 0; i < 3; ++i) for (unsigned x = 0; x < 2; ++x) upload[i * 8 + x * 4] = (float)pixels[i][x];
    VkSampler samplers[3];
    for (unsigned i = 0; i < 3; ++i) {
        VkSamplerCreateInfo sampler = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
            .addressModeU = i == 2 ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE };
        CHECK(vkCreateSampler(device, &sampler, NULL, &samplers[i]));
    }
    VkDescriptorSetLayout layouts[5];
    const uint32_t binding_numbers[] = {1, 3, 5, 640, 672, 674, 707};
    VkDescriptorSetLayoutBinding bindings[7];
    for (unsigned i = 0; i < 7; ++i) bindings[i] = (VkDescriptorSetLayoutBinding){
        binding_numbers[i], VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    for (unsigned i = 0; i < 5; ++i) {
        VkDescriptorSetLayoutBinding array = {0, i == 4 ? VK_DESCRIPTOR_TYPE_SAMPLER : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            i == 4 ? 3 : 4, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
        VkDescriptorBindingFlags variable = VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT;
        VkDescriptorSetLayoutBindingFlagsCreateInfo flags = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
            .bindingCount = 1, .pBindingFlags = &variable };
        VkDescriptorSetLayoutCreateInfo info = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .pNext = i == 2 || i == 4 ? &flags : NULL, .bindingCount = i == 0 ? 7 : i == 2 || i == 4 ? 1 : 0,
            .pBindings = i == 0 ? bindings : &array };
        CHECK(vkCreateDescriptorSetLayout(device, &info, NULL, &layouts[i]));
    }
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 7}, {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 4}, {VK_DESCRIPTOR_TYPE_SAMPLER, 3}};
    VkDescriptorPoolCreateInfo pool_info = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 5, .poolSizeCount = 3, .pPoolSizes = sizes };
    VkDescriptorPool pool; CHECK(vkCreateDescriptorPool(device, &pool_info, NULL, &pool));
    uint32_t variable_counts[] = {0, 0, 4, 0, 3};
    VkDescriptorSetVariableDescriptorCountAllocateInfo variable_info = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO,
        .descriptorSetCount = 5, .pDescriptorCounts = variable_counts };
    VkDescriptorSetAllocateInfo sets_info = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .pNext = &variable_info,
        .descriptorPool = pool, .descriptorSetCount = 5, .pSetLayouts = layouts };
    VkDescriptorSet sets[5]; CHECK(vkAllocateDescriptorSets(device, &sets_info, sets));
    const unsigned buffer_indices[] = {CONTROL, ARGUMENTS, SEED, ADDRESSES, TOKENS, STAGE, ARGUMENTS};
    VkDescriptorBufferInfo buffer_info[7]; VkWriteDescriptorSet writes[9];
    for (unsigned i = 0; i < 7; ++i) {
        buffer_info[i] = (VkDescriptorBufferInfo){buffers[buffer_indices[i]].buffer, i == 1 || i == 6 ? 256 : 0, i == 1 || i == 6 ? 512 : 1024};
        if (buffer_indices[i] == ADDRESSES) buffer_info[i].range = registry_bytes;
        if (buffer_indices[i] == TOKENS) buffer_info[i].range = sizeof table;
        if (buffer_indices[i] == STAGE) buffer_info[i].range = 384;
        writes[i] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sets[0],
            .dstBinding = binding_numbers[i], .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo = &buffer_info[i] };
    }
    VkDescriptorImageInfo image_info[4], sampler_info[3];
    for (unsigned i = 0; i < 4; ++i)
        image_info[i] = (VkDescriptorImageInfo){VK_NULL_HANDLE, images[i == 3 ? 0 : i].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    for (unsigned i = 0; i < 3; ++i) {
        sampler_info[i] = (VkDescriptorImageInfo){samplers[i], VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
    }
    writes[7] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sets[2], .descriptorCount = 4,
        .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = image_info };
    writes[8] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sets[4], .descriptorCount = 3,
        .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER, .pImageInfo = sampler_info };
    vkUpdateDescriptorSets(device, 9, writes, 0, NULL);
    VkPushConstantRange push_range = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 48};
    VkPipelineLayoutCreateInfo layout_info = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 5, .pSetLayouts = layouts, .pushConstantRangeCount = 1, .pPushConstantRanges = &push_range };
    VkPipelineLayout layout; CHECK(vkCreatePipelineLayout(device, &layout_info, NULL, &layout));
    VkPipeline consumer = pipeline(argv[2], layout, VK_NULL_HANDLE), producer = pipeline(argv[3], layout, VK_NULL_HANDLE);
    VkCommandPoolCreateInfo command_pool_info = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = family };
    VkCommandPool command_pool; CHECK(vkCreateCommandPool(device, &command_pool_info, NULL, &command_pool));
    for (unsigned round = 0; round < 4; ++round) {
        memset(buffers[OUTPUT].map, 0xcd, 1024); memset(buffers[ARGUMENTS].map, 0xa5, 1024);
        *(uint32_t *)buffers[CONTROL].map = round;
        VkCommandBufferAllocateInfo command_info = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = command_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
        VkCommandBuffer command; CHECK(vkAllocateCommandBuffers(device, &command_info, &command));
        VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        CHECK(vkBeginCommandBuffer(command, &begin));
        barrier(command, VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT);
        if (!round) for (unsigned i = 0; i < 3; ++i) {
            VkImageMemoryBarrier transition = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = images[i].image,
                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1} };
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &transition);
            VkBufferImageCopy copy = { .bufferOffset = i * 8 * sizeof(float),
                .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {2, 1, 1} };
            vkCmdCopyBufferToImage(command, buffers[UPLOAD].buffer, images[i].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            transition.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; transition.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            transition.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; transition.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &transition);
        }
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 5, sets, 0, NULL);
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, producer); vkCmdDispatch(command, 1, 1, 1);
        barrier(command, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
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
        for (unsigned i = 0; i < 256; ++i) {
            uint32_t expected = 0xcdcdcdcdu;
            if (i >= 4 && i < 36) {
                unsigned thread = i - 4, slot = (thread ^ round) & 1u;
                unsigned image = (slot ^ (round & 1u)) + 1;
                if ((round == 2 && !slot) || (round == 3 && slot)) image = 0;
                unsigned clamp = !((thread >> 1) & 1u);
                expected = (slot ? 2000 : 1000) + thread + pixels[image][clamp];
            }
            uint32_t actual = ((uint32_t *)buffers[OUTPUT].map)[i];
            if (actual != expected) fprintf(stderr, "round=%u word=%u actual=%u expected=%u\n", round, i, actual, expected);
            require(actual == expected, "GPU resource selection or output guard mismatch");
        }
        for (unsigned i = 0; i < 1024; ++i) if (i < 256 || i >= 304)
            require(((uint8_t *)buffers[ARGUMENTS].map)[i] == 0xa5, "argument guard modified");
        printf("GPU_RESOURCE_TABLES_ROUND_PASS round=%u threads=32 cpu_token_readback=0\n", round);
    }
    vkDestroyCommandPool(device, command_pool, NULL); vkDestroyPipeline(device, producer, NULL);
    vkDestroyPipeline(device, consumer, NULL); vkDestroyPipelineLayout(device, layout, NULL);
    vkDestroyDescriptorPool(device, pool, NULL);
    for (unsigned i = 0; i < 5; ++i) vkDestroyDescriptorSetLayout(device, layouts[i], NULL);
    for (unsigned i = 0; i < 3; ++i) {
        vkDestroySampler(device, samplers[i], NULL); vkDestroyImageView(device, images[i].view, NULL);
        vkDestroyImage(device, images[i].image, NULL); vkFreeMemory(device, images[i].allocation, NULL);
    }
    for (unsigned i = 0; i < NBUFFERS; ++i) {
        vkUnmapMemory(device, buffers[i].allocation); vkDestroyBuffer(device, buffers[i].buffer, NULL);
        vkFreeMemory(device, buffers[i].allocation, NULL);
    }
    vkDestroyDevice(device, NULL); vkDestroyInstance(instance, NULL); dlclose(library);
    puts("GPU_RESOURCE_TABLES_PASS rounds=4 high_word_collision=1 missing_token=1 null_token=1 guards=1");
    return 0;
}
