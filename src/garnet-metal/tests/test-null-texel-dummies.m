// Exercise the real typed texel dummy factory and every fallible construction step.
#include "GarnetDevice.m"

static unsigned checks, creates, allocations, liveBuffers, liveMemory, liveViews;
static unsigned failureStep, expectedComponent;
static BOOL expectedStorage, missingFormat, missingMemory;
static VkDeviceMemory currentMemory;
typedef struct { uint8_t bytes[16]; BOOL live; } Memory;
static Memory memories[128];
static void require(BOOL ok, const char *why) {
    ++checks;
    if (!ok) { fprintf(stderr, "NULL_TEXEL_DUMMIES_FAIL %s\n", why); exit(1); }
}
static VkFormat expectedFormat(void) {
    const VkFormat formats[]={VK_FORMAT_R32_SFLOAT,VK_FORMAT_R32_UINT,VK_FORMAT_R32_SINT};
    return formats[expectedComponent];
}
static void VKAPI_CALL properties(VkPhysicalDevice device, VkFormat format, VkFormatProperties *out) {
    (void)device;
    require(format==expectedFormat(), "typed component selects the matching buffer format");
    *out=(VkFormatProperties){.bufferFeatures=missingFormat ? 0 :
        VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT|VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT};
}
static VkResult VKAPI_CALL createBuffer(VkDevice device, const VkBufferCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkBuffer *out) {
    (void)device; (void)allocator;
    require(info && info->size==4 && info->usage==(VK_BUFFER_USAGE_TRANSFER_DST_BIT|
        (expectedStorage ? VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT : VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT)),
        "one texel buffer with exact storage/uniform usage");
    ++creates;
    if (failureStep==1) return VK_ERROR_OUT_OF_HOST_MEMORY;
    *out=(VkBuffer)(uintptr_t)(0x1000u+creates); ++liveBuffers; return VK_SUCCESS;
}
static void VKAPI_CALL requirements(VkDevice device, VkBuffer buffer, VkMemoryRequirements *out) {
    (void)device; (void)buffer;
    *out=(VkMemoryRequirements){.size=16,.alignment=4,.memoryTypeBits=1};
    RDNA4R.mp.memoryTypes[0].propertyFlags=missingMemory ? 0 :
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
}
static VkResult VKAPI_CALL allocate(VkDevice device, const VkMemoryAllocateInfo *info,
    const VkAllocationCallbacks *allocator, VkDeviceMemory *out) {
    (void)device; (void)allocator;
    require(info && info->allocationSize==16 && !info->memoryTypeIndex, "exact coherent memory allocation");
    if (failureStep==2) return VK_ERROR_OUT_OF_HOST_MEMORY;
    require(allocations<128, "bounded mock memory slots");
    Memory *memory=&memories[allocations++]; memset(memory->bytes,0xa5,sizeof memory->bytes); memory->live=YES;
    *out=(VkDeviceMemory)(uintptr_t)memory; currentMemory=*out; ++liveMemory; return VK_SUCCESS;
}
static VkResult VKAPI_CALL bind(VkDevice device, VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize offset) {
    (void)device; (void)buffer;
    require(memory==currentMemory && !offset, "bind correct backing at zero offset");
    return failureStep==3 ? VK_ERROR_OUT_OF_DEVICE_MEMORY : VK_SUCCESS;
}
static VkResult VKAPI_CALL map(VkDevice device, VkDeviceMemory memory, VkDeviceSize offset,
    VkDeviceSize size, VkMemoryMapFlags flags, void **out) {
    (void)device;
    require(memory==currentMemory && !offset && size==VK_WHOLE_SIZE && !flags,
        "zero through the actual coherent mapped memory");
    if (failureStep==4) return VK_ERROR_MEMORY_MAP_FAILED;
    *out=((Memory *)(uintptr_t)memory)->bytes; return VK_SUCCESS;
}
static void VKAPI_CALL unmap(VkDevice device, VkDeviceMemory memory) {
    (void)device;
    const uint8_t *bytes=((Memory *)(uintptr_t)memory)->bytes;
    require(!bytes[0] && !bytes[1] && !bytes[2] && !bytes[3] && bytes[4]==0xa5,
        "zero exactly the dummy texel without touching allocation padding");
}
static VkResult VKAPI_CALL createView(VkDevice device, const VkBufferViewCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkBufferView *out) {
    (void)device; (void)allocator;
    require(info && info->buffer && info->format==expectedFormat() && !info->offset && info->range==4,
        "a real typed buffer view, not an image view");
    if (failureStep==5) return VK_ERROR_OUT_OF_HOST_MEMORY;
    *out=(VkBufferView)(uintptr_t)(0x9000u+allocations); ++liveViews; return VK_SUCCESS;
}
static void VKAPI_CALL destroyBuffer(VkDevice device, VkBuffer buffer, const VkAllocationCallbacks *allocator) {
    (void)device; (void)allocator; require(buffer && liveBuffers, "destroy only an acquired buffer"); --liveBuffers;
}
static void VKAPI_CALL freeMemory(VkDevice device, VkDeviceMemory handle, const VkAllocationCallbacks *allocator) {
    (void)device; (void)allocator;
    Memory *memory=(Memory *)(uintptr_t)handle;
    require(memory && memory->live && liveMemory, "release only live memory once"); memory->live=NO; --liveMemory;
}

int main(void) {
    @autoreleasepool {
        RDNA4R.dev=(VkDevice)(uintptr_t)0x1234;
        RDNA4R.uniformTexelBuffers=RDNA4R.storageTexelBuffers=YES;
        RDNA4R.lim.minTexelBufferOffsetAlignment=256; RDNA4R.lim.maxTexelBufferElements=1024;
        RDNA4R.mp.memoryTypeCount=1;
        vkGetPhysicalDeviceFormatProperties=properties; vkCreateBuffer=createBuffer;
        vkGetBufferMemoryRequirements=requirements; vkAllocateMemory=allocate; vkBindBufferMemory=bind;
        vkMapMemory=map; vkUnmapMemory=unmap; vkCreateBufferView=createView;
        vkDestroyBuffer=destroyBuffer; vkFreeMemory=freeMemory;
        NSObject *device=[NSObject new];
        for (unsigned storage=0;storage<2;++storage) for (unsigned component=0;component<3;++component) {
            expectedStorage=storage; expectedComponent=component;
            rdna4ns_shape shape={.dim=RDNA4NS_DIM_BUFFER,.component=component,
                .access=storage ? RDNA4NS_ACCESS_STORAGE_TEXEL : RDNA4NS_ACCESS_UNIFORM_TEXEL};
            unsigned baseline=liveViews;
            for (unsigned failure=1;failure<=7;++failure) {
                failureStep=failure; missingFormat=failure==6; missingMemory=failure==7;
                const char *why=NULL;
                require(!rdna4_dummy_null_texel(device,&shape,&why) && why,
                    "construction failures do not publish a partial cached view");
                require(liveBuffers==baseline && liveMemory==baseline && liveViews==baseline,
                    "every acquired buffer and memory is rolled back on failure");
            }
            failureStep=0; missingFormat=missingMemory=NO;
            const char *why=NULL;
            VkBufferView view=rdna4_dummy_null_texel(device,&shape,&why);
            require(view && !why && liveViews==baseline+1 && liveBuffers==liveViews && liveMemory==liveViews,
                "retry publishes one complete dummy and backing pair");
            unsigned before=creates;
            require(rdna4_dummy_null_texel(device,&shape,&why)==view && creates==before,
                "same typed shape reuses the bounded cache entry");
        }
        require(liveViews==6 && liveBuffers==6 && liveMemory==6 && !RDNA4R.tried,
            "six typed uniform/storage caches, no GPU open");
        printf("NULL_TEXEL_DUMMIES_PASS checks=%u shapes=6 failures=42 zero_and_padding=1 "
            "rollback_retry=1 typed_buffer_views=1 gpu_execution=0\n",checks);
    }
    return 0;
}
