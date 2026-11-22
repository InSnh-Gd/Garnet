#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <stdbool.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "radv_darwin_scanout.h"

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
#define PROC(name) PFN_##name name = (PFN_##name)gipa(instance, #name); REQUIRE(name)

int main(int argc, char **argv)
{
   REQUIRE(argc == 2);
   setvbuf(stdout, NULL, _IONBF, 0);
   REQUIRE(setenv("RADV_DARWIN_MOCK", "1", 1) == 0);
   REQUIRE(setenv("RADV_DARWIN_MOCK_GC", "gfx1200", 1) == 0);
   REQUIRE(setenv("MESA_SHADER_CACHE_DISABLE", "true", 1) == 0);
   unsetenv("RADV_DARWIN_HOST_BO");
   unsetenv("RADV_DARWIN_CHIP");
   unsetenv("RADV_DARWIN_MOCK_HANG");
   unsetenv("RADV_DARWIN_ALLOW_ADDRCFG");
   unsetenv("RADV_FORCE_FAMILY");
   unsetenv("RADV_NULL_WINSYS");
   void *lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
   if (!lib) fprintf(stderr, "%s\n", dlerror());
   REQUIRE(lib);
   PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vk_icdGetInstanceProcAddr");
   REQUIRE(gipa);
   int (*copy)(VkDevice, VkDeviceMemory, uint64_t, VkDeviceMemory, uint64_t, uint64_t, uint64_t *) =
      (typeof(copy))dlsym(lib, "radv_darwin_buffer_copy");
   int (*fill)(VkDevice, VkDeviceMemory, uint64_t, uint64_t, uint32_t, uint64_t *) =
      (typeof(fill))dlsym(lib, "radv_darwin_buffer_fill");
   int (*wait)(VkDevice, uint64_t, uint64_t, bool *) =
      (typeof(wait))dlsym(lib, "radv_darwin_buffer_wait");
   REQUIRE(copy && fill && wait);
   puts("EXPORTS_PASS typed copy/fill/wait resolved from actual dylib");
   VkInstance instance = VK_NULL_HANDLE;
   PROC(vkCreateInstance);
   VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3};
   VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
   REQUIRE(vkCreateInstance(&ici, NULL, &instance) == VK_SUCCESS);
   PROC(vkEnumeratePhysicalDevices);
   PROC(vkGetPhysicalDeviceProperties);
   PROC(vkCreateDevice);
   PROC(vkAllocateMemory);
   PROC(vkFreeMemory);
   PROC(vkDestroyDevice);
   PROC(vkDestroyInstance);
   uint32_t count = 1;
   VkPhysicalDevice physical;
   REQUIRE(vkEnumeratePhysicalDevices(instance, &count, &physical) == VK_SUCCESS && count == 1);
   VkPhysicalDeviceProperties props;
   vkGetPhysicalDeviceProperties(physical, &props);
   REQUIRE(strstr(props.deviceName, "RADV GFX1200"));
   printf("GFX1200_PASS device=%s vendor=%#x id=%#x (mock discovery, production ICD)\n",
          props.deviceName, props.vendorID, props.deviceID);
   float priority = 1;
   VkDeviceQueueCreateInfo queue = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &priority};
   VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue};
   VkDevice device, other;
   REQUIRE(vkCreateDevice(physical, &dci, NULL, &device) == VK_SUCCESS);
   REQUIRE(vkCreateDevice(physical, &dci, NULL, &other) == VK_SUCCESS);
   VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = 4096, .memoryTypeIndex = 0};
   VkDeviceMemory src, dst;
   REQUIRE(vkAllocateMemory(device, &mai, NULL, &src) == VK_SUCCESS);
   REQUIRE(vkAllocateMemory(device, &mai, NULL, &dst) == VK_SUCCESS);
   uint64_t seq = 123;
   REQUIRE(copy(device, src, 0, dst, 0, 4, &seq) == -ENOSYS && seq == 0);
   REQUIRE(fill(device, dst, 0, 4, 0x12345678, &seq) == -ENOSYS && seq == 0);
   REQUIRE(fill(device, dst, 0, 4, 0, &seq) == -ENOSYS && seq == 0);
   puts("OLD_CAPABILITY_PASS copy/fill return -ENOSYS, seq=0; no optional operation accepted");
   REQUIRE(fill(other, dst, 0, 4, 0, &seq) == -EINVAL && seq == 0);
   REQUIRE(fill(device, dst, 4096, 4, 0, &seq) == -EINVAL && seq == 0);
   REQUIRE(copy(device, src, 0, src, 0, 4, &seq) == -EINVAL && seq == 0);
   REQUIRE(fill(device, dst, 0, 3, 0, &seq) == -EINVAL && seq == 0);
   bool busy = false;
   REQUIRE(wait(device, 0, 0, &busy) == -EINVAL && busy);
   puts("VALIDATION_PASS foreign device, bounds, overlap, alignment, invalid wait");
   vkFreeMemory(device, src, NULL);
   vkFreeMemory(device, dst, NULL);
   vkDestroyDevice(other, NULL);
   vkDestroyDevice(device, NULL);
   vkDestroyInstance(instance, NULL);
   REQUIRE(dlclose(lib) == 0);
   puts("DYLIB_BRIDGE_PASS no queue submit, no physical GPU access; production native backend preserved");
   return 0;
}
