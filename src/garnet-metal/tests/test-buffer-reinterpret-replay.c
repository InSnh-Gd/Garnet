// Execute descriptor-backed uint/float helper views with exact bits and guards on a CPU ICD.
#include "test-vulkan-replay.h"

int main(int argc, char **argv) {
    int constant_helper = argc == 4 && !strcmp(argv[3], "--constant-helper");
    if (argc != 3 && !constant_helper) return 2;
    alarm(60);
    (void)new_image;
    void *library; VkQueue queue; uint32_t family;
    VkInstance instance = new_backend(argv[1], 1, &library, &queue, &family);
    Buffer buffers[] = {new_buffer(1024), new_buffer(1024), new_buffer(1024)};
    rdna4_buffer_table *table = buffers[2].map;
    require(rdna4_buffer_table_set(table, 0, buffers[0].address, 1024, 256, 1) &&
        rdna4_buffer_table_set(table, 1, buffers[1].address, 1024, 256, 0), "reinterpret address table");
    const uint32_t binding_numbers[] = {0, 1, 640};
    VkDescriptorSetLayoutBinding bindings[3];
    for (unsigned i = 0; i < 3; ++i) bindings[i] = (VkDescriptorSetLayoutBinding){
        binding_numbers[i], VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo set_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 3, .pBindings = bindings};
    VkDescriptorSetLayout set_layout; CHECK(vkCreateDescriptorSetLayout(device, &set_info, NULL, &set_layout));
    VkDescriptorPoolSize size = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3};
    VkDescriptorPoolCreateInfo pool_info = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &size};
    VkDescriptorPool pool; CHECK(vkCreateDescriptorPool(device, &pool_info, NULL, &pool));
    VkDescriptorSetAllocateInfo allocate = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = pool, .descriptorSetCount = 1, .pSetLayouts = &set_layout};
    VkDescriptorSet set; CHECK(vkAllocateDescriptorSets(device, &allocate, &set));
    VkDescriptorBufferInfo buffer_info[3]; VkWriteDescriptorSet writes[3];
    for (unsigned i = 0; i < 3; ++i) {
        buffer_info[i] = (VkDescriptorBufferInfo){buffers[i].buffer, i < 2 ? 256 : 0, i < 2 ? 512 : 768};
        writes[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
            .dstBinding = binding_numbers[i], .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo = &buffer_info[i]};
    }
    vkUpdateDescriptorSets(device, 3, writes, 0, NULL);
    VkPushConstantRange push_range = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 48};
    VkPipelineLayoutCreateInfo layout_info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &push_range};
    VkPipelineLayout layout; CHECK(vkCreatePipelineLayout(device, &layout_info, NULL, &layout));
    VkPipelineCacheCreateInfo cache_info = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    VkPipelineCache cache; CHECK(vkCreatePipelineCache(device, &cache_info, NULL, &cache));
    VkCommandPoolCreateInfo command_pool_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = family};
    VkCommandPool command_pool; CHECK(vkCreateCommandPool(device, &command_pool_info, NULL, &command_pool));
    for (unsigned round = 0; round < 4; ++round) {
        if (round == 2) {
            vkDestroyPipelineCache(device, cache, NULL);
            CHECK(vkCreatePipelineCache(device, &cache_info, NULL, &cache));
        }
        memset(buffers[0].map, 0xa5, 1024); memset(buffers[1].map, 0xcd, 1024);
        uint32_t expected[32];
        for (unsigned i = 0; i < (constant_helper ? 64u : 32u); ++i) {
            float value = (round & 1 ? -1.0f : 1.0f) * ((float)(round * 32 + i) + 0.25f);
            uint32_t bits; memcpy(&bits, &value, sizeof bits);
            memcpy((uint8_t *)buffers[0].map + 256 + 4 * i, &bits, sizeof bits);
            if (i < 32) expected[i] = bits ^ UINT32_C(0x80000000);
        }
        size_t expected_bytes = sizeof expected;
        if (constant_helper) {
            const unsigned offsets[] = {14, 15, 22, 0, 28, 29, 36, 0};
            for (unsigned index = 0; index < 8; ++index) {
                expected[index] = 0;
                if (index != 3 && index != 7)
                    memcpy(&expected[index], (uint8_t *)buffers[0].map + 256 + 4 * offsets[index], 4);
            }
            expected_bytes = 8 * sizeof *expected;
        }
        uint8_t input[1024], addresses[1024];
        memcpy(input, buffers[0].map, sizeof input); memcpy(addresses, buffers[2].map, sizeof addresses);
        VkPipeline consumer = pipeline(argv[2], layout, cache);
        VkCommandBufferAllocateInfo command_info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = command_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
        VkCommandBuffer command; CHECK(vkAllocateCommandBuffers(device, &command_info, &command));
        VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        CHECK(vkBeginCommandBuffer(command, &begin));
        barrier(command, VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, NULL);
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, consumer);
        uint32_t push[12] = {32, 1, 1, 0, 0, 0, 0, 0, 0, 1, 1, 1};
        vkCmdPushConstants(command, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, push);
        vkCmdDispatch(command, 1, 1, 1);
        barrier(command, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
        CHECK(vkEndCommandBuffer(command));
        VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VkFence fence; CHECK(vkCreateFence(device, &fence_info, NULL, &fence));
        VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &command};
        CHECK(vkQueueSubmit(queue, 1, &submit, fence));
        CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_C(10000000000)));
        require(!memcmp((uint8_t *)buffers[1].map + 256, expected, expected_bytes), "float bits were numerically converted or written at the wrong offset");
        require(!memcmp(input, buffers[0].map, sizeof input), "input or input guards modified");
        require(!memcmp(addresses, buffers[2].map, sizeof addresses), "address table modified");
        for (unsigned i = 0; i < 1024; ++i)
            if (i < 256 || i >= 256 + expected_bytes)
                require(((uint8_t *)buffers[1].map)[i] == 0xcd, "output guards modified");
        printf("%s_REPLAY_ROUND_PASS pipeline=%s outputs=%u exact_bits=1 guards=1\n",
            constant_helper ? "CONSTANT_HELPER" : "BUFFER_REINTERPRET",
            round & 1 ? "warm" : "cold", constant_helper ? 8u : 32u);
        vkDestroyFence(device, fence, NULL); vkFreeCommandBuffers(device, command_pool, 1, &command);
        vkDestroyPipeline(device, consumer, NULL);
    }
    vkDestroyCommandPool(device, command_pool, NULL); vkDestroyPipelineCache(device, cache, NULL);
    vkDestroyPipelineLayout(device, layout, NULL); vkDestroyDescriptorPool(device, pool, NULL);
    vkDestroyDescriptorSetLayout(device, set_layout, NULL);
    for (unsigned i = 0; i < 3; ++i) {
        vkUnmapMemory(device, buffers[i].allocation); vkDestroyBuffer(device, buffers[i].buffer, NULL);
        vkFreeMemory(device, buffers[i].allocation, NULL);
    }
    vkDestroyDevice(device, NULL); vkDestroyInstance(instance, NULL); dlclose(library);
    printf("%s_REPLAY_PASS cpu_backend=1 rounds=4 outputs=%u exact_bits=1 guards=1\n",
        constant_helper ? "CONSTANT_HELPER" : "BUFFER_REINTERPRET", constant_helper ? 32u : 128u);
    return 0;
}
