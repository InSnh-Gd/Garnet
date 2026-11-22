// Execute actual translated texture-buffer AIR on an explicitly selected CPU Vulkan ICD.
#include "test-vulkan-replay.h"

static uint32_t bits(unsigned kind, int value) {
    if (kind) return (uint32_t)(kind == 2 ? -value : value);
    float f = (float)value; uint32_t result; memcpy(&result, &f, 4); return result;
}
int main(int argc, char **argv) {
    if (argc < 4 || argc > 6) return 2;
    unsigned kind = (unsigned)strtoul(argv[3], NULL, 10); require(kind < 3, "invalid numeric class");
    unsigned format = argc >= 5 ? (unsigned)strtoul(argv[4], NULL, 10) : kind;
    int read_write = argc == 6 && atoi(argv[5]) != 0;
    require(format < 7 && (format < 3 ? format == kind : kind == 0), "format disagrees with shader numeric class");
    alarm(60); (void)new_image; (void)new_buffer; (void)new_backend;
    void *library; VkQueue queue; uint32_t family;
    VkInstance instance = new_backend_robust_texel(argv[1], 1, 1, &library, &queue, &family);
    Buffer buffers[] = {new_buffer_usage(1024, VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT),
        new_buffer_usage(1024, VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT), new_buffer(1024)};
    const VkFormat formats[] = {VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32_UINT, VK_FORMAT_R32_SINT,
        VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R16G16B16A16_SFLOAT};
    const unsigned pixelBytes[] = {4,4,4,1,2,4,8};
    unsigned stride = pixelBytes[format], payload = stride * 7;
    VkBufferView views[2];
    for (unsigned i = 0; i < 2; ++i) {
        VkBufferViewCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,
            .buffer = buffers[i].buffer, .format = formats[format], .offset = 256, .range = payload};
        CHECK(vkCreateBufferView(device, &info, NULL, &views[i]));
    }
    const uint32_t numbers[] = {32, 481, 1};
    const VkDescriptorType types[] = {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
        VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
    VkDescriptorSetLayoutBinding bindings[3]; VkDescriptorPoolSize sizes[3];
    for (unsigned i = 0; i < 3; ++i) {
        bindings[i] = (VkDescriptorSetLayoutBinding){numbers[i], types[i], 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
        sizes[i] = (VkDescriptorPoolSize){types[i], 1};
    }
    VkDescriptorSetLayoutCreateInfo setInfo = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 3, .pBindings = bindings};
    VkDescriptorSetLayout setLayout; CHECK(vkCreateDescriptorSetLayout(device, &setInfo, NULL, &setLayout));
    VkDescriptorPoolCreateInfo poolInfo = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1, .poolSizeCount = 3, .pPoolSizes = sizes};
    VkDescriptorPool pool; CHECK(vkCreateDescriptorPool(device, &poolInfo, NULL, &pool));
    VkDescriptorSetAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = pool, .descriptorSetCount = 1, .pSetLayouts = &setLayout};
    VkDescriptorSet set; CHECK(vkAllocateDescriptorSets(device, &allocation, &set));
    VkDescriptorBufferInfo output = {buffers[2].buffer, 256, 64};
    VkWriteDescriptorSet writes[3];
    for (unsigned i = 0; i < 3; ++i) writes[i] = (VkWriteDescriptorSet){
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = numbers[i],
        .descriptorCount = 1, .descriptorType = types[i], .pTexelBufferView = i < 2 ? &views[i] : NULL,
        .pBufferInfo = i == 2 ? &output : NULL};
    vkUpdateDescriptorSets(device, 3, writes, 0, NULL);
    VkPushConstantRange push = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 48};
    VkPipelineLayoutCreateInfo layoutInfo = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &setLayout, .pushConstantRangeCount = 1, .pPushConstantRanges = &push};
    VkPipelineLayout layout; CHECK(vkCreatePipelineLayout(device, &layoutInfo, NULL, &layout));
    VkPipelineCacheCreateInfo cacheInfo = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    VkPipelineCache cache; CHECK(vkCreatePipelineCache(device, &cacheInfo, NULL, &cache));
    VkCommandPoolCreateInfo commandInfo = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = family};
    VkCommandPool commands; CHECK(vkCreateCommandPool(device, &commandInfo, NULL, &commands));
    for (unsigned round = 0; round < 4; ++round) {
        for (unsigned i = 0; i < 3; ++i) memset(buffers[i].map, 0xa5, 1024);
        uint32_t *source = (uint32_t *)((uint8_t *)buffers[0].map + 256);
        uint32_t *destination = (uint32_t *)((uint8_t *)buffers[1].map + 256);
        for (unsigned x = 0; x < 7; ++x) {
            if (format < 3) {
                source[x] = bits(kind, (int)(round * 100 + x + 1));
                destination[x] = bits(kind, (int)(round * 100 + x + 201));
            } else for (unsigned c = 0; c < (format == 6 ? 4 : stride); ++c) {
                unsigned one = (round + x + c) & 1;
                if (format == 6) {
                    ((uint16_t *)source)[x*4+c] = one ? 0x3c00 : 0;
                    ((uint16_t *)destination)[x*4+c] = one ? 0 : 0x3c00;
                } else {
                    ((uint8_t *)source)[x*stride+c] = one ? 255 : 0;
                    ((uint8_t *)destination)[x*stride+c] = one ? 0 : 255;
                }
            }
        }
        uint8_t original[1024]; memcpy(original, buffers[0].map, 1024);
        VkPipeline consumer = pipeline(argv[2], layout, cache);
        VkCommandBufferAllocateInfo commandAllocation = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = commands, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
        VkCommandBuffer command; CHECK(vkAllocateCommandBuffers(device, &commandAllocation, &command));
        VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        CHECK(vkBeginCommandBuffer(command, &begin));
        barrier(command, VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, consumer);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, NULL);
        uint32_t dispatch[12] = {1,1,1,0,0,0,0,0,0,1,1,1};
        vkCmdPushConstants(command, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof dispatch, dispatch);
        vkCmdDispatch(command, 1,1,1); barrier(command, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
        CHECK(vkEndCommandBuffer(command));
        VkFenceCreateInfo fenceInfo = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VkFence fence; CHECK(vkCreateFence(device, &fenceInfo, NULL, &fence));
        VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &command};
        CHECK(vkQueueSubmit(queue, 1, &submit, fence));
        CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_C(10000000000)));
        uint32_t alpha = format >= 5 ? 0 : kind ? 1 : bits(0,1);
        uint32_t expected[] = {bits(kind, (int)(round * 100 + 2)),0,0,alpha,7,0,0,0,
            0,0,0,alpha,0,0,0,alpha};
        if (format >= 3) for (unsigned c = 0; c < 4; ++c) {
            unsigned components = format == 6 ? 4 : stride;
            expected[c] = bits(0, c < components ? (int)((round + 1 + c) & 1) : c == 3 ? 1 : 0);
        }
        if (read_write) for (unsigned c = 0; c < 4; ++c) {
            unsigned components = format < 3 ? 1 : format == 6 ? 4 : stride;
            expected[8+c] = c < components ? bits(kind, format >= 3 && format <= 5 ? 1 : 73) :
                c == 3 ? kind ? 1 : bits(0,1) : 0;
        }
        const uint32_t *actual = (const uint32_t *)((uint8_t *)buffers[2].map + 256);
        for (unsigned i = 0; i < 16; ++i) if (actual[i] != expected[i]) {
            fprintf(stderr, "TEXEL_REPLAY_FAIL round=%u word=%u expected=%u actual=%u\n", round, i, expected[i], actual[i]); return 1;
        }
        require(!memcmp(original, buffers[0].map, 1024), "source or its guards changed");
        for (unsigned x = 0; x < 7; ++x) {
            if (format < 3)
                require(destination[x] == bits(kind, x == 3 ? 73 : (int)(round * 100 + x + 201)), "write affected the wrong texel");
            else for (unsigned c = 0; c < (format == 6 ? 4 : stride); ++c) {
                unsigned one = (round + x + c) & 1;
                if (format == 6) require(((uint16_t *)destination)[x*4+c] ==
                    (x == 3 ? 0x5490 : one ? 0 : 0x3c00), "half conversion or untouched texel differs");
                else require(((uint8_t *)destination)[x*stride+c] ==
                    (x == 3 ? 255 : one ? 0 : 255), "normalized conversion or untouched texel differs");
            }
        }
        for (unsigned i = 0; i < 1024; ++i) {
            if (i < 256 || i >= 256 + payload) require(((uint8_t *)buffers[1].map)[i] == 0xa5, "write guard changed");
            if (i < 256 || i >= 320) require(((uint8_t *)buffers[2].map)[i] == 0xa5, "output guard changed");
        }
        printf("TEXEL_REPLAY_ROUND_PASS kind=%u format=%u round=%u outputs=16 exact_bits=1 guards=1\n", kind, format, round);
        vkDestroyFence(device, fence, NULL); vkFreeCommandBuffers(device, commands, 1, &command);
        vkDestroyPipeline(device, consumer, NULL);
    }
    vkDestroyCommandPool(device, commands, NULL); vkDestroyPipelineCache(device, cache, NULL);
    vkDestroyPipelineLayout(device, layout, NULL); vkDestroyDescriptorPool(device, pool, NULL);
    vkDestroyDescriptorSetLayout(device, setLayout, NULL);
    for (unsigned i = 0; i < 3; ++i) {
        if (i < 2) vkDestroyBufferView(device, views[i], NULL);
        vkUnmapMemory(device, buffers[i].allocation); vkDestroyBuffer(device, buffers[i].buffer, NULL);
        vkFreeMemory(device, buffers[i].allocation, NULL);
    }
    vkDestroyDevice(device, NULL); vkDestroyInstance(instance, NULL); dlclose(library);
    puts("TEXEL_REPLAY_PASS cpu_backend=1 rounds=4 outputs=64 exact_bits=1 guards=1 native_driver=untested");
    return 0;
}
