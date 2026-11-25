#ifndef TEST_VULKAN_REPLAY_H
#define TEST_VULKAN_REPLAY_H
// Vulkan helpers shared by the standalone translated-AIR execution probes.
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "garnet_buffer_ranges.h"

#define FUNCTIONS(X) \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceFeatures2) X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkCreateDevice) X(vkDestroyDevice) \
    X(vkGetDeviceQueue) X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) \
    X(vkAllocateMemory) X(vkFreeMemory) X(vkBindBufferMemory) X(vkMapMemory) X(vkUnmapMemory) \
    X(vkCreateBufferView) X(vkDestroyBufferView) \
    X(vkGetBufferDeviceAddress) X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) \
    X(vkBindImageMemory) X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateSampler) X(vkDestroySampler) \
    X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) X(vkCreateDescriptorPool) \
    X(vkDestroyDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) \
    X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) X(vkCreateShaderModule) X(vkDestroyShaderModule) \
    X(vkCreatePipelineCache) X(vkDestroyPipelineCache) X(vkFreeCommandBuffers) X(vkCreateComputePipelines) X(vkDestroyPipeline) X(vkCreateCommandPool) X(vkDestroyCommandPool) \
    X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkCmdPipelineBarrier) \
    X(vkCmdCopyBufferToImage) X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdPushConstants) \
    X(vkCmdDispatch) X(vkCreateFence) X(vkDestroyFence) X(vkQueueSubmit) X(vkWaitForFences)
#define DECLARE(name) static PFN_##name name;
FUNCTIONS(DECLARE)
#undef DECLARE
static VkDevice device;
static VkPhysicalDeviceMemoryProperties memory;

static void require(int ok, const char *message) {
    if (!ok) { fprintf(stderr, "VULKAN_REPLAY_FAIL %s\n", message); exit(1); }
}
#define CHECK(call) do { VkResult result = (call); if (result != VK_SUCCESS) { \
    fprintf(stderr, "VULKAN_REPLAY_FAIL %s = %d\n", #call, result); exit(1); } } while (0)
static uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags flags) {
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & flags) == flags) return i;
    require(0, "required memory type unavailable"); return 0;
}
typedef struct { VkBuffer buffer; VkDeviceMemory allocation; void *map; VkDeviceAddress address; } Buffer;
static Buffer new_buffer_usage(size_t bytes, VkBufferUsageFlags usage) {
    Buffer b = {0};
    VkBufferCreateInfo info = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = bytes,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    CHECK(vkCreateBuffer(device, &info, NULL, &b.buffer));
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(device, b.buffer, &req);
    VkMemoryAllocateFlagsInfo flags = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
        .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT };
    VkMemoryAllocateInfo allocation = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &flags,
        .allocationSize = req.size, .memoryTypeIndex = memory_type(req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    CHECK(vkAllocateMemory(device, &allocation, NULL, &b.allocation));
    CHECK(vkBindBufferMemory(device, b.buffer, b.allocation, 0));
    CHECK(vkMapMemory(device, b.allocation, 0, VK_WHOLE_SIZE, 0, &b.map));
    VkBufferDeviceAddressInfo address = { .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = b.buffer };
    b.address = vkGetBufferDeviceAddress(device, &address);
    require(b.address != 0, "buffer device address unavailable");
    memset(b.map, 0, bytes);
    return b;
}
static Buffer new_buffer(size_t bytes) { return new_buffer_usage(bytes, 0); }
typedef struct { VkImage image; VkDeviceMemory allocation; VkImageView view; } Image;
static Image new_image(VkFormat format, uint32_t width, uint32_t height, VkImageAspectFlags aspect) {
    Image image = {0};
    VkImageCreateInfo info = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = format,
        .extent = {width, height, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    CHECK(vkCreateImage(device, &info, NULL, &image.image));
    VkMemoryRequirements req; vkGetImageMemoryRequirements(device, image.image, &req);
    VkMemoryAllocateInfo allocation = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size, .memoryTypeIndex = memory_type(req.memoryTypeBits, 0) };
    CHECK(vkAllocateMemory(device, &allocation, NULL, &image.allocation));
    CHECK(vkBindImageMemory(device, image.image, image.allocation, 0));
    VkImageViewCreateInfo view = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = image.image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = info.format,
        .subresourceRange = {aspect, 0, 1, 0, 1} };
    CHECK(vkCreateImageView(device, &view, NULL, &image.view));
    return image;
}
static VkPipeline pipeline(const char *path, VkPipelineLayout layout, VkPipelineCache cache) {
    FILE *file = fopen(path, "rb"); require(file != NULL, "shader file unavailable");
    require(!fseek(file, 0, SEEK_END), "shader seek failed"); long size = ftell(file);
    require(size > 0 && size <= 16 * 1024 * 1024 && !(size % 4), "shader size invalid");
    rewind(file); uint32_t *bytes = malloc((size_t)size); require(bytes != NULL, "shader allocation failed");
    require(fread(bytes, 1, (size_t)size, file) == (size_t)size, "shader read failed"); fclose(file);
    VkShaderModuleCreateInfo module_info = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = (size_t)size, .pCode = bytes };
    VkShaderModule module; CHECK(vkCreateShaderModule(device, &module_info, NULL, &module)); free(bytes);
    VkComputePipelineCreateInfo info = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = "main" }, .layout = layout };
    VkPipeline pipeline; CHECK(vkCreateComputePipelines(device, cache, 1, &info, NULL, &pipeline));
    vkDestroyShaderModule(device, module, NULL); return pipeline;
}
static void barrier(VkCommandBuffer command, VkAccessFlags source, VkAccessFlags destination) {
    VkMemoryBarrier memory_barrier = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = source, .dstAccessMask = destination };
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &memory_barrier, 0, NULL, 0, NULL);
}

static VkInstance new_backend_robust_texel(const char *path, int cpu_only, int robust_texel,
    void **library_out, VkQueue *queue_out, uint32_t *family_out) {
    void *library = dlopen(path, RTLD_NOW | RTLD_LOCAL); require(library != NULL, "Vulkan library unavailable");
    PFN_vkGetInstanceProcAddr get = (PFN_vkGetInstanceProcAddr)dlsym(library, "vkGetInstanceProcAddr");
    require(get != NULL, "Vulkan loader unavailable");
    PFN_vkCreateInstance create = (PFN_vkCreateInstance)get(NULL, "vkCreateInstance");
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_2 };
    VkInstanceCreateInfo instance_info = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    VkInstance instance; CHECK(create(&instance_info, NULL, &instance));
#define LOAD(name) name = (PFN_##name)get(instance, #name); require(name != NULL, #name " unavailable");
    FUNCTIONS(LOAD)
#undef LOAD
    uint32_t count = 1; VkPhysicalDevice physical; CHECK(vkEnumeratePhysicalDevices(instance, &count, &physical));
    require(count != 0, "no physical device");
    VkPhysicalDeviceProperties properties; vkGetPhysicalDeviceProperties(physical, &properties);
    printf("VULKAN_REPLAY_BACKEND %s type=%u api=%u\n", properties.deviceName, properties.deviceType, properties.apiVersion);
    require(!cpu_only || properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU, "this replay requires a CPU Vulkan device");
    VkPhysicalDeviceRobustness2FeaturesEXT robustness = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
    VkPhysicalDeviceVulkan12Features f12 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .pNext = robust_texel ? &robustness : NULL };
    VkPhysicalDeviceVulkan11Features f11 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &f12 };
    VkPhysicalDeviceFeatures2 features = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f11 };
    vkGetPhysicalDeviceFeatures2(physical, &features);
    require(!robust_texel || (features.features.robustBufferAccess && robustness.robustBufferAccess2),
        "texture-buffer replay requires robustBufferAccess2");
    robustness.robustImageAccess2 = robustness.nullDescriptor = VK_FALSE;
    require(features.features.shaderInt64 && f12.shaderInt8 && f12.bufferDeviceAddress && f12.runtimeDescriptorArray && f12.descriptorBindingVariableDescriptorCount &&
        f12.shaderSampledImageArrayNonUniformIndexing, "required GPU argument features unavailable");
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    count = 0; vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, NULL);
    VkQueueFamilyProperties *families = calloc(count, sizeof *families); require(families != NULL, "queue allocation failed");
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families);
    uint32_t family = 0; while (family < count && !(families[family].queueFlags & VK_QUEUE_COMPUTE_BIT)) family++;
    free(families); require(family < count, "no compute queue"); float priority = 1;
    VkDeviceQueueCreateInfo queue_info = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority };
    const char *robust_extension = VK_EXT_ROBUSTNESS_2_EXTENSION_NAME;
    VkDeviceCreateInfo device_info = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &features,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue_info,
        .enabledExtensionCount = robust_texel ? 1 : 0, .ppEnabledExtensionNames = robust_texel ? &robust_extension : NULL };
    CHECK(vkCreateDevice(physical, &device_info, NULL, &device));
    VkQueue queue; vkGetDeviceQueue(device, family, 0, &queue);
    *library_out = library; *queue_out = queue; *family_out = family;
    return instance;
}
static VkInstance new_backend(const char *path, int cpu_only, void **library_out,
    VkQueue *queue_out, uint32_t *family_out) {
    return new_backend_robust_texel(path, cpu_only, 0, library_out, queue_out, family_out);
}
#endif
