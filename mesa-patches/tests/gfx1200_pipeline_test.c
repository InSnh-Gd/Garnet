// SPDX-License-Identifier: MIT
// Exercise the real Darwin ICD and ACO with mock BOs; optional submissions never execute GPU work.
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void require(int ok, const char *what)
{
   if (!ok) {
      fprintf(stderr, "FAIL: %s\n", what);
      exit(1);
   }
}

static void result(VkResult r, const char *what)
{
   printf("%s -> %d\n", what, r);
   require(r == VK_SUCCESS, what);
}

#define CHECK(call) result((call), #call)
#define INSTANCE_PROC(name) \
   PFN_##name name = (PFN_##name)gipa(instance, #name); require(name != NULL, #name)
#define DEVICE_PROC(name) \
   PFN_##name name = (PFN_##name)vkGetDeviceProcAddr(device, #name); require(name != NULL, #name)

static void submit_cases(PFN_vkGetInstanceProcAddr gipa, VkInstance instance,
                         VkPhysicalDevice physical, VkDevice device, uint32_t family,
                         VkPipeline pipeline, VkPipelineLayout layout, VkDescriptorSetLayout set_layout,
                         int hang_mode)
{
   INSTANCE_PROC(vkGetDeviceProcAddr);
   INSTANCE_PROC(vkGetPhysicalDeviceMemoryProperties);
   DEVICE_PROC(vkGetDeviceQueue);
   DEVICE_PROC(vkCreateBuffer);
   DEVICE_PROC(vkGetBufferMemoryRequirements);
   DEVICE_PROC(vkAllocateMemory);
   DEVICE_PROC(vkBindBufferMemory);
   DEVICE_PROC(vkMapMemory);
   DEVICE_PROC(vkUnmapMemory);
   DEVICE_PROC(vkCreateDescriptorPool);
   DEVICE_PROC(vkAllocateDescriptorSets);
   DEVICE_PROC(vkUpdateDescriptorSets);
   DEVICE_PROC(vkCreateCommandPool);
   DEVICE_PROC(vkAllocateCommandBuffers);
   DEVICE_PROC(vkBeginCommandBuffer);
   DEVICE_PROC(vkCmdPipelineBarrier);
   DEVICE_PROC(vkCmdBindPipeline);
   DEVICE_PROC(vkCmdBindDescriptorSets);
   DEVICE_PROC(vkCmdDispatch);
   DEVICE_PROC(vkEndCommandBuffer);
   DEVICE_PROC(vkCreateFence);
   DEVICE_PROC(vkGetFenceStatus);
   DEVICE_PROC(vkResetFences);
   DEVICE_PROC(vkWaitForFences);
   DEVICE_PROC(vkQueueSubmit);
   DEVICE_PROC(vkQueueWaitIdle);
   DEVICE_PROC(vkCreateSemaphore);
   DEVICE_PROC(vkGetSemaphoreCounterValue);
   DEVICE_PROC(vkDestroySemaphore);
   DEVICE_PROC(vkDestroyFence);
   DEVICE_PROC(vkDestroyCommandPool);
   DEVICE_PROC(vkDestroyDescriptorPool);
   DEVICE_PROC(vkDestroyBuffer);
   DEVICE_PROC(vkFreeMemory);
   VkQueue queue;
   vkGetDeviceQueue(device, family, 0, &queue);
   VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = 256, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
   VkBuffer buffer;
   CHECK(vkCreateBuffer(device, &bci, NULL, &buffer));
   VkMemoryRequirements requirements;
   vkGetBufferMemoryRequirements(device, buffer, &requirements);
   VkPhysicalDeviceMemoryProperties properties;
   vkGetPhysicalDeviceMemoryProperties(physical, &properties);
   uint32_t type = UINT32_MAX;
   for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
      const VkMemoryPropertyFlags needed = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
      if ((requirements.memoryTypeBits & (1u << i)) && (properties.memoryTypes[i].propertyFlags & needed) == needed) {
         type = i;
         break;
      }
   }
   require(type != UINT32_MAX, "coherent host-visible storage memory");
   VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = requirements.size, .memoryTypeIndex = type};
   VkDeviceMemory memory;
   CHECK(vkAllocateMemory(device, &mai, NULL, &memory));
   CHECK(vkBindBufferMemory(device, buffer, memory, 0));
   void *mapped;
   CHECK(vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &mapped));
   memset(mapped, 0xa5, 256);
   VkDescriptorPoolSize pool_size = {.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1};
   VkDescriptorPoolCreateInfo dpci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &pool_size};
   VkDescriptorPool descriptors;
   CHECK(vkCreateDescriptorPool(device, &dpci, NULL, &descriptors));
   VkDescriptorSetAllocateInfo dsai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = descriptors, .descriptorSetCount = 1, .pSetLayouts = &set_layout};
   VkDescriptorSet descriptor;
   CHECK(vkAllocateDescriptorSets(device, &dsai, &descriptor));
   VkDescriptorBufferInfo dbi = {.buffer = buffer, .range = 256};
   VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = descriptor,
      .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &dbi};
   vkUpdateDescriptorSets(device, 1, &write, 0, NULL);
   VkCommandPoolCreateInfo cpci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = family};
   VkCommandPool commands;
   CHECK(vkCreateCommandPool(device, &cpci, NULL, &commands));
   const uint32_t command_count = hang_mode ? 1 : 65;
   VkCommandBuffer buffers[command_count];
   VkCommandBufferAllocateInfo cbai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = commands, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = command_count};
   CHECK(vkAllocateCommandBuffers(device, &cbai, buffers));
   for (unsigned i = 0; i < command_count; ++i) {
      VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
         .flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT};
      CHECK(vkBeginCommandBuffer(buffers[i], &begin));
      VkMemoryBarrier before = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
         .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
         .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT};
      vkCmdPipelineBarrier(buffers[i], VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &before, 0, NULL, 0, NULL);
      vkCmdBindPipeline(buffers[i], VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
      vkCmdBindDescriptorSets(buffers[i], VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &descriptor, 0, NULL);
      vkCmdDispatch(buffers[i], 1, 1, 1);
      VkMemoryBarrier after = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
         .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
      vkCmdPipelineBarrier(buffers[i], VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                           0, 1, &after, 0, NULL, 0, NULL);
      CHECK(vkEndCommandBuffer(buffers[i]));
   }
   VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
   VkFence fence;
   CHECK(vkCreateFence(device, &fci, NULL, &fence));
   require(vkGetFenceStatus(device, fence) == VK_NOT_READY, "new fence is unsignalled");
   require(vkWaitForFences(device, 1, &fence, VK_TRUE, 0) == VK_TIMEOUT, "unsignalled fence wait times out");
   VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = buffers};
   if (hang_mode) {
      VkSemaphoreTypeCreateInfo sti = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
         .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE};
      VkSemaphoreCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &sti};
      VkSemaphore semaphore;
      CHECK(vkCreateSemaphore(device, &sci, NULL, &semaphore));
      uint64_t point = 1;
      VkTimelineSemaphoreSubmitInfo timeline = {.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
         .signalSemaphoreValueCount = 1, .pSignalSemaphoreValues = &point};
      submit.pNext = &timeline;
      submit.signalSemaphoreCount = 1;
      submit.pSignalSemaphores = &semaphore;
      CHECK(vkQueueSubmit(queue, 1, &submit, fence));
      for (unsigned i = 0; i < 2; ++i) {
         uint64_t observed = UINT64_MAX;
         VkResult error = hang_mode == 1 ? vkGetSemaphoreCounterValue(device, semaphore, &observed)
                                        : vkWaitForFences(device, 1, &fence, VK_TRUE, 0);
         printf("SYNC_HANG mode=%s call=%u result=%d value=%llu\n", hang_mode == 1 ? "query" : "wait", i, error,
                (unsigned long long)observed);
         require(error == VK_ERROR_DEVICE_LOST, "synchronization propagates a GPU hang as DEVICE_LOST");
         require(observed == UINT64_MAX, "failed timeline query leaves output untouched");
      }
      require(vkGetFenceStatus(device, fence) == VK_ERROR_DEVICE_LOST, "lost-device fence status");
      require(vkQueueSubmit(queue, 1, &submit, fence) == VK_ERROR_DEVICE_LOST, "lost device rejects resubmission");
      vkDestroySemaphore(device, semaphore, NULL);
      vkDestroyFence(device, fence, NULL);
      vkDestroyCommandPool(device, commands, NULL);
      vkDestroyDescriptorPool(device, descriptors, NULL);
      vkUnmapMemory(device, memory);
      vkDestroyBuffer(device, buffer, NULL);
      vkFreeMemory(device, memory, NULL);
      puts("QUEUE_CASE_PASS sync_hang mock_fault=1 GPU_execution=0");
      return;
   }
   CHECK(vkQueueSubmit(queue, 1, &submit, fence));
   CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, 5000000000ull));
   CHECK(vkGetFenceStatus(device, fence));
   puts("QUEUE_CASE_PASS single_compute mock_acceptance=1 GPU_execution=0");
   CHECK(vkResetFences(device, 1, &fence));
   submit.commandBufferCount = command_count;
   CHECK(vkQueueSubmit(queue, 1, &submit, fence));
   CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, 5000000000ull));
   puts("QUEUE_CASE_PASS many_compute command_buffers=65 mock_acceptance=1 GPU_execution=0");

   VkSemaphoreTypeCreateInfo sti = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
      .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE};
   VkSemaphoreCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &sti};
   VkSemaphore semaphore;
   CHECK(vkCreateSemaphore(device, &sci, NULL, &semaphore));
   uint64_t point = 1;
   VkTimelineSemaphoreSubmitInfo timeline = {.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
      .signalSemaphoreValueCount = 1, .pSignalSemaphoreValues = &point};
   submit.pNext = &timeline;
   submit.commandBufferCount = 1;
   submit.signalSemaphoreCount = 1;
   submit.pSignalSemaphores = &semaphore;
   CHECK(vkResetFences(device, 1, &fence));
   CHECK(vkQueueSubmit(queue, 1, &submit, fence));
   CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, 5000000000ull));
   uint64_t observed = 0;
   CHECK(vkGetSemaphoreCounterValue(device, semaphore, &observed));
   require(observed == 1, "timeline point one retired");
   uint64_t wait = 1;
   point = 2;
   timeline.waitSemaphoreValueCount = 1;
   timeline.pWaitSemaphoreValues = &wait;
   VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
   submit.waitSemaphoreCount = 1;
   submit.pWaitSemaphores = &semaphore;
   submit.pWaitDstStageMask = &stage;
   CHECK(vkResetFences(device, 1, &fence));
   CHECK(vkQueueSubmit(queue, 1, &submit, fence));
   CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, 5000000000ull));
   CHECK(vkGetSemaphoreCounterValue(device, semaphore, &observed));
   require(observed == 2, "timeline wait and signal retired");
   puts("QUEUE_CASE_PASS timeline_wait_signal value=2 mock_acceptance=1 GPU_execution=0");

   CHECK(vkResetFences(device, 1, &fence));
   VkSubmitInfo empty = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO};
   CHECK(vkQueueSubmit(queue, 1, &empty, fence));
   CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, 5000000000ull));
   puts("QUEUE_CASE_PASS empty_submit mock_acceptance=1 GPU_execution=0");
   CHECK(vkQueueWaitIdle(queue));
   for (unsigned i = 0; i < 256; ++i)
      require(((const unsigned char *)mapped)[i] == 0xa5, "mock has not executed the shader");
   puts("QUEUE_MOCK_GUARD_PASS storage_sentinel_unchanged=1 GPU_execution=0");
   vkDestroySemaphore(device, semaphore, NULL);
   vkDestroyFence(device, fence, NULL);
   vkDestroyCommandPool(device, commands, NULL);
   vkDestroyDescriptorPool(device, descriptors, NULL);
   vkUnmapMemory(device, memory);
   vkDestroyBuffer(device, buffer, NULL);
   vkFreeMemory(device, memory, NULL);
}

int main(int argc, char **argv)
{
   require(argc == 4 || (argc == 5 && (!strcmp(argv[4], "--submit") || !strcmp(argv[4], "--query-hang") ||
                                      !strcmp(argv[4], "--wait-hang"))),
           "usage: test DYLIB SHADER.spv OUTPUT_DIR [--submit|--query-hang|--wait-hang]");
   const int submits = argc == 5;
   const int hang_mode = !submits || !strcmp(argv[4], "--submit") ? 0 : !strcmp(argv[4], "--query-hang") ? 1 : 2;
   setvbuf(stdout, NULL, _IONBF, 0);
   require(setenv("RADV_DARWIN_MOCK", "1", 1) == 0, "set mock");
   require(setenv("RADV_DARWIN_MOCK_GC", "gfx1200", 1) == 0, "set mock GC");
   if (hang_mode)
      require(setenv("RADV_DARWIN_MOCK_HANG", "1", 1) == 0, "set mock hang");
   else
      unsetenv("RADV_DARWIN_MOCK_HANG");
   require(setenv("RADV_DEBUG", "nocache", 1) == 0, "disable pipeline cache");
   require(setenv("MESA_SHADER_CACHE_DISABLE", "true", 1) == 0, "disable disk cache");
   unsetenv("RADV_DARWIN_HOST_BO");
   unsetenv("RADV_DARWIN_CHIP");
   unsetenv("RADV_DARWIN_ALLOW_ADDRCFG");
   unsetenv("RADV_FORCE_FAMILY");
   unsetenv("RADV_NULL_WINSYS");
   printf("dlopen %s; mock GC=gfx1200; caches disabled; queue_submission=%u GPU_execution=0\n", argv[1], submits);
   void *lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
   if (!lib) fprintf(stderr, "dlopen: %s\n", dlerror());
   require(lib != NULL, "dlopen ICD");
   PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vk_icdGetInstanceProcAddr");
   require(gipa != NULL, "vk_icdGetInstanceProcAddr");
   VkInstance instance = VK_NULL_HANDLE;
   INSTANCE_PROC(vkCreateInstance);
   VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "owned gfx1200 offline pipeline", .apiVersion = VK_API_VERSION_1_3};
   VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
   CHECK(vkCreateInstance(&ici, NULL, &instance));
   INSTANCE_PROC(vkEnumeratePhysicalDevices);
   INSTANCE_PROC(vkGetPhysicalDeviceProperties2);
   INSTANCE_PROC(vkGetPhysicalDeviceMemoryProperties);
   INSTANCE_PROC(vkGetPhysicalDeviceQueueFamilyProperties);
   INSTANCE_PROC(vkGetPhysicalDeviceFeatures2);
   INSTANCE_PROC(vkCreateDevice);
   INSTANCE_PROC(vkGetDeviceProcAddr);
   INSTANCE_PROC(vkDestroyInstance);
   uint32_t count = 0;
   CHECK(vkEnumeratePhysicalDevices(instance, &count, NULL));
   printf("physical device count=%u\n", count);
   require(count == 1, "exactly one mock physical device");
   VkPhysicalDevice physical;
   CHECK(vkEnumeratePhysicalDevices(instance, &count, &physical));
   VkPhysicalDeviceDriverProperties driver = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
   VkPhysicalDeviceProperties2 props = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &driver};
   vkGetPhysicalDeviceProperties2(physical, &props);
   printf("device=%s vendor=%#x deviceID=%#x driver=%s info=%s\n", props.properties.deviceName,
          props.properties.vendorID, props.properties.deviceID, driver.driverName, driver.driverInfo);
   require(strstr(props.properties.deviceName, "RADV GFX1200") != NULL, "actual gfx1200 selection");
   require(driver.driverID == VK_DRIVER_ID_MESA_RADV, "actual RADV driver");
   INSTANCE_PROC(vkGetPhysicalDeviceImageFormatProperties2);
   const VkPhysicalDeviceImageFormatInfo2 color_format = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
      .format = VK_FORMAT_B8G8R8A8_UNORM,
      .type = VK_IMAGE_TYPE_2D,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
   };
   VkImageCompressionPropertiesEXT compression = {.sType = VK_STRUCTURE_TYPE_IMAGE_COMPRESSION_PROPERTIES_EXT};
   VkImageFormatProperties2 color_properties = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, .pNext = &compression,
   };
   CHECK(vkGetPhysicalDeviceImageFormatProperties2(physical, &color_format, &color_properties));
   require(compression.imageCompressionFlags == VK_IMAGE_COMPRESSION_DISABLED_EXT,
           "Darwin must report uncompressed optimal color attachments");
   VkPhysicalDeviceMemoryProperties mem;
   vkGetPhysicalDeviceMemoryProperties(physical, &mem);
   for (uint32_t i = 0; i < mem.memoryHeapCount; ++i)
      printf("heap[%u] size=%llu flags=%#x (mock pools)\n", i,
             (unsigned long long)mem.memoryHeaps[i].size, mem.memoryHeaps[i].flags);
   count = 0;
   vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, NULL);
   require(count > 0 && count <= 16, "bounded queue family count");
   VkQueueFamilyProperties queues[16];
   vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, queues);
   uint32_t general = UINT32_MAX;
   for (uint32_t i = 0; i < count; ++i) {
      printf("queue family[%u] flags=%#x count=%u\n", i, queues[i].queueFlags, queues[i].queueCount);
      require(!(queues[i].queueFlags & VK_QUEUE_COMPUTE_BIT) ||
              (queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT), "no dedicated async compute family");
      if ((queues[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
          (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) general = i;
   }
   require(general != UINT32_MAX && queues[general].queueCount == 1, "one general GFX/compute queue");
   VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR exec = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR};
   VkPhysicalDeviceTimelineSemaphoreFeatures timeline_feature = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
   if (submits) exec.pNext = &timeline_feature;
   VkPhysicalDeviceFeatures2 features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &exec};
   vkGetPhysicalDeviceFeatures2(physical, &features);
   require(exec.pipelineExecutableInfo, "pipeline executable info support");
   if (submits) require(timeline_feature.timelineSemaphore, "timeline semaphore support");
   float priority = 1.0f;
   VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = general, .queueCount = 1, .pQueuePriorities = &priority};
   const char *extensions[] = {VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME};
   VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &exec,
      .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
      .enabledExtensionCount = 1, .ppEnabledExtensionNames = extensions};
   VkDevice device;
   CHECK(vkCreateDevice(physical, &dci, NULL, &device));
   DEVICE_PROC(vkCreateShaderModule);
   DEVICE_PROC(vkCreateDescriptorSetLayout);
   DEVICE_PROC(vkCreatePipelineLayout);
   DEVICE_PROC(vkCreateComputePipelines);
   DEVICE_PROC(vkGetPipelineExecutablePropertiesKHR);
   DEVICE_PROC(vkGetPipelineExecutableInternalRepresentationsKHR);
   DEVICE_PROC(vkDestroyPipeline);
   DEVICE_PROC(vkDestroyPipelineLayout);
   DEVICE_PROC(vkDestroyDescriptorSetLayout);
   DEVICE_PROC(vkDestroyShaderModule);
   DEVICE_PROC(vkDestroyDevice);
   FILE *file = fopen(argv[2], "rb");
   require(file != NULL, "open owned SPIR-V");
   require(fseek(file, 0, SEEK_END) == 0, "seek SPIR-V");
   long length = ftell(file);
   require(length >= 20 && length % 4 == 0 && length < (1 << 20), "SPIR-V size");
   rewind(file);
   uint32_t *code = malloc((size_t)length);
   require(code != NULL, "allocate SPIR-V");
   require(fread(code, 1, (size_t)length, file) == (size_t)length, "read SPIR-V");
   require(fclose(file) == 0 && code[0] == 0x07230203, "SPIR-V magic");
   VkShaderModuleCreateInfo smci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = (size_t)length, .pCode = code};
   VkShaderModule shader;
   CHECK(vkCreateShaderModule(device, &smci, NULL, &shader));
   free(code);
   VkDescriptorSetLayoutBinding binding = {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
   VkDescriptorSetLayoutCreateInfo slci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 1, .pBindings = &binding};
   VkDescriptorSetLayout set;
   CHECK(vkCreateDescriptorSetLayout(device, &slci, NULL, &set));
   VkPipelineLayoutCreateInfo plci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1, .pSetLayouts = &set};
   VkPipelineLayout layout;
   CHECK(vkCreatePipelineLayout(device, &plci, NULL, &layout));
   VkComputePipelineCreateInfo cpci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .flags = VK_PIPELINE_CREATE_CAPTURE_INTERNAL_REPRESENTATIONS_BIT_KHR,
      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = shader, .pName = "main"}, .layout = layout};
   VkPipeline pipeline;
   CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpci, NULL, &pipeline));
   VkPipelineInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR, .pipeline = pipeline};
   count = 0;
   CHECK(vkGetPipelineExecutablePropertiesKHR(device, &pi, &count, NULL));
   require(count == 1, "one compute executable");
   VkPipelineExecutablePropertiesKHR ep = {.sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR};
   CHECK(vkGetPipelineExecutablePropertiesKHR(device, &pi, &count, &ep));
   printf("executable=%s description=%s stages=%#x subgroup=%u\n", ep.name, ep.description, ep.stages, ep.subgroupSize);
   require(ep.stages == VK_SHADER_STAGE_COMPUTE_BIT, "compute executable stage");
   VkPipelineExecutableInfoKHR ei = {.sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR, .pipeline = pipeline};
   count = 0;
   CHECK(vkGetPipelineExecutableInternalRepresentationsKHR(device, &ei, &count, NULL));
   require(count > 0 && count <= 8, "bounded IR count");
   VkPipelineExecutableInternalRepresentationKHR ir[8] = {0};
   for (uint32_t i = 0; i < count; ++i)
      ir[i].sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INTERNAL_REPRESENTATION_KHR;
   CHECK(vkGetPipelineExecutableInternalRepresentationsKHR(device, &ei, &count, ir));
   for (uint32_t i = 0; i < count; ++i) {
      require(ir[i].dataSize < (16u << 20), "bounded IR size");
      ir[i].pData = calloc(ir[i].dataSize + 1, 1);
      require(ir[i].pData != NULL, "allocate IR");
   }
   CHECK(vkGetPipelineExecutableInternalRepresentationsKHR(device, &ei, &count, ir));
   int aco = 0;
   for (uint32_t i = 0; i < count; ++i) {
      printf("IR[%u]=%s bytes=%zu text=%u\n", i, ir[i].name, ir[i].dataSize, ir[i].isText);
      char path[4096];
      int n = snprintf(path, sizeof(path), "%s/pipeline-ir-%u.txt", argv[3], i);
      require(n > 0 && (size_t)n < sizeof(path), "IR path");
      file = fopen(path, "wb");
      require(file != NULL, "open IR artifact");
      require(fwrite(ir[i].pData, 1, ir[i].dataSize, file) == ir[i].dataSize, "write IR artifact");
      require(fclose(file) == 0, "close IR artifact");
      if (!strcmp(ir[i].name, "ACO IR")) aco = ir[i].isText && ir[i].dataSize > 1;
      free(ir[i].pData);
   }
   require(aco, "nonempty ACO IR captured from actual compute pipeline");
   if (submits) submit_cases(gipa, instance, physical, device, general, pipeline, layout, set, hang_mode);
   vkDestroyPipeline(device, pipeline, NULL);
   vkDestroyPipelineLayout(device, layout, NULL);
   vkDestroyDescriptorSetLayout(device, set, NULL);
   vkDestroyShaderModule(device, shader, NULL);
   vkDestroyDevice(device, NULL);
   vkDestroyInstance(instance, NULL);
   require(dlclose(lib) == 0, "dlclose");
   puts(submits ? "PASS: gfx1200 RADV pipeline and native submission marshalling; mock acceptance, no GPU execution"
                : "PASS: gfx1200 RADV compute pipeline compiled with ACO; no queue submissions");
   return 0;
}
