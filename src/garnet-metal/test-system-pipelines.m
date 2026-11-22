// Build a test-only interposition dylib with -DRDNA4_FEATURE_AUDIT -dynamiclib
// -install_name @rpath/librdna4featureaudit.dylib, then build this executable linked
// with -L. -lrdna4featureaudit -Wl,-rpath,@executable_path. Both use -fobjc-arc
// -Wall -Wextra -Werror -I/path/to/Vulkan-Headers/include -framework Foundation.
// Run in a macOS VM: test-system-pipelines BUNDLE AIR_CORPUS
// Uses the shipped Metal pipeline implementation and RADV/ACO with mock BOs.
// No Metal device is instantiated and no command queue is submitted.
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <stdatomic.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#ifdef RDNA4_FEATURE_AUDIT
// Observe the actual bundle's device creation without replacing its ICD or BOs.
// RADV does not validate feature use itself; check shader capabilities against
// the captured enabled features, rather than accepting compilation as proof.
static PFN_vkGetInstanceProcAddr actualGIPA;
static PFN_vkCreateDevice actualCreateDevice;
static VkPhysicalDeviceFeatures enabled;
static VkBool32 enabledFloat16, enabledInt8, enabledDemote;
static VkBool32 enabledStorage8, enabledUniformStorage8, enabledStorage16, enabledUniformStorage16;
static BOOL capturedDevice;
static _Atomic unsigned submissions, waits, fenceDestroys, commandFrees, bufferDestroys;
static _Atomic unsigned computePipelines, graphicsPipelines, dispatches, draws;
static PFN_vkCreateFence actualCreateFence;
static PFN_vkDestroyFence actualDestroyFence;
static PFN_vkFreeCommandBuffers actualFreeCommandBuffers;
static PFN_vkDestroyBuffer actualDestroyBuffer;
static PFN_vkCreateComputePipelines actualComputePipelines;
static PFN_vkCreateGraphicsPipelines actualGraphicsPipelines;
static PFN_vkCmdDispatch actualDispatch;
static PFN_vkCmdDispatchIndirect actualDispatchIndirect;
static PFN_vkCmdDraw actualDraw;
static PFN_vkCmdDrawIndexed actualDrawIndexed;
static PFN_vkCmdDrawIndirect actualDrawIndirect;
static PFN_vkCmdDrawIndexedIndirect actualDrawIndexedIndirect;
static PFN_vkCreateBuffer actualCreateBuffer;
static PFN_vkCreateImageView actualCreateImageView;
static PFN_vkDestroyImageView actualDestroyImageView;
static PFN_vkCreateFramebuffer actualCreateFramebuffer;
static PFN_vkCmdCopyImage actualCopyImage;
static _Atomic unsigned bufferCopies, bufferFills;
static NSMutableDictionary<NSNumber *, NSNumber *> *identityViews;
static NSMutableDictionary<NSNumber *, NSNumber *> *bufferUsage;
static NSMutableArray<NSData *> *imageCopies;
static NSMutableArray<NSData *> *imageViews;
static _Atomic unsigned invalidAttachments;
static _Atomic unsigned invalidImageCopies;
static _Atomic unsigned indirectCalls, invalidIndirect;
static _Atomic unsigned indirectDispatches;
static _Atomic uint64_t indirectDispatchOffset;

BOOL rdna4_audit_has_device(void) { return capturedDevice; }
unsigned rdna4_audit_buffer_copies(void) { return atomic_load(&bufferCopies); }
unsigned rdna4_audit_buffer_fills(void) { return atomic_load(&bufferFills); }
unsigned rdna4_audit_submission_count(void) { return atomic_load(&submissions); }
unsigned rdna4_audit_wait_count(void) { return atomic_load(&waits); }
unsigned rdna4_audit_fence_destroys(void) { return atomic_load(&fenceDestroys); }
unsigned rdna4_audit_command_frees(void) { return atomic_load(&commandFrees); }
unsigned rdna4_audit_buffer_destroys(void) { return atomic_load(&bufferDestroys); }
unsigned rdna4_audit_compute_pipelines(void) { return atomic_load(&computePipelines); }
unsigned rdna4_audit_graphics_pipelines(void) { return atomic_load(&graphicsPipelines); }
unsigned rdna4_audit_dispatches(void) { return atomic_load(&dispatches); }
unsigned rdna4_audit_draws(void) { return atomic_load(&draws); }
unsigned rdna4_audit_invalid_attachments(void) { return atomic_load(&invalidAttachments); }
BOOL rdna4_audit_indirect_first_instance(void) { return enabled.drawIndirectFirstInstance; }
unsigned rdna4_audit_indirect_calls(void) { return atomic_load(&indirectCalls); }
unsigned rdna4_audit_invalid_indirect(void) { return atomic_load(&invalidIndirect); }
unsigned rdna4_audit_indirect_dispatches(void) { return atomic_load(&indirectDispatches); }
uint64_t rdna4_audit_indirect_dispatch_offset(void) { return atomic_load(&indirectDispatchOffset); }
unsigned rdna4_audit_invalid_image_copies(void) { return atomic_load(&invalidImageCopies); }
BOOL rdna4_audit_cube_array(void) { return enabled.imageCubeArray; }
unsigned rdna4_audit_image_copy_count(void) {
    @synchronized (NSProcessInfo.processInfo) { return (unsigned)imageCopies.count; }
}
BOOL rdna4_audit_image_copy(unsigned index, uint32_t fields[10]) {
    @synchronized (NSProcessInfo.processInfo) {
        if (index >= imageCopies.count) return NO;
        [imageCopies[index] getBytes:fields length:10 * sizeof(*fields)];
        return YES;
    }
}
unsigned rdna4_audit_image_view_count(void) {
    @synchronized (NSProcessInfo.processInfo) { return (unsigned)imageViews.count; }
}
BOOL rdna4_audit_image_view(unsigned index, uint32_t fields[5]) {
    @synchronized (NSProcessInfo.processInfo) {
        if (index >= imageViews.count) return NO;
        [imageViews[index] getBytes:fields length:5 * sizeof(*fields)];
        return YES;
    }
}

static void auditCopyImage(VkCommandBuffer command, VkImage source, VkImageLayout sourceLayout,
                           VkImage destination, VkImageLayout destinationLayout, uint32_t count, const VkImageCopy *regions) {
    @synchronized (NSProcessInfo.processInfo) {
        if (!imageCopies) imageCopies = [NSMutableArray array];
        if ((source == destination && (sourceLayout != VK_IMAGE_LAYOUT_GENERAL || destinationLayout != VK_IMAGE_LAYOUT_GENERAL)) ||
            (sourceLayout != VK_IMAGE_LAYOUT_GENERAL && sourceLayout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) ||
            (destinationLayout != VK_IMAGE_LAYOUT_GENERAL && destinationLayout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL))
            atomic_fetch_add(&invalidImageCopies, 1);
        for (uint32_t i = 0; i < count; ++i) {
            const VkImageCopy *r = &regions[i];
            if (r->srcSubresource.aspectMask != r->dstSubresource.aspectMask) atomic_fetch_add(&invalidImageCopies, 1);
            const uint32_t fields[] = { r->srcSubresource.mipLevel, r->srcSubresource.baseArrayLayer,
                r->dstSubresource.mipLevel, r->dstSubresource.baseArrayLayer, r->extent.width, r->extent.height,
                r->extent.depth, r->srcSubresource.layerCount, r->dstSubresource.layerCount, source == destination };
            [imageCopies addObject:[NSData dataWithBytes:fields length:sizeof fields]];
        }
    }
    actualCopyImage(command, source, sourceLayout, destination, destinationLayout, count, regions);
}

static VkResult auditCreateBuffer(VkDevice device, const VkBufferCreateInfo *info,
                                  const VkAllocationCallbacks *allocator, VkBuffer *buffer) {
    VkResult result = actualCreateBuffer(device, info, allocator, buffer);
    if (result == VK_SUCCESS) {
        @synchronized (NSProcessInfo.processInfo) {
            if (!bufferUsage) bufferUsage = [NSMutableDictionary dictionary];
            bufferUsage[@((uintptr_t)*buffer)] = @(info->usage);
        }
    }
    return result;
}

static void auditIndirectBuffer(VkBuffer buffer, VkDeviceSize offset) {
    @synchronized (NSProcessInfo.processInfo) {
        if ((offset & 3) ||
            !(bufferUsage[@((uintptr_t)buffer)].unsignedIntValue & VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT))
            atomic_fetch_add(&invalidIndirect, 1);
    }
    atomic_fetch_add(&indirectCalls, 1);
}

static void auditDrawIndirect(VkCommandBuffer command, VkBuffer buffer, VkDeviceSize offset,
                              uint32_t count, uint32_t stride) {
    if (!enabled.drawIndirectFirstInstance || count != 1) atomic_fetch_add(&invalidIndirect, 1);
    auditIndirectBuffer(buffer, offset);
    actualDrawIndirect(command, buffer, offset, count, stride);
}

static void auditDrawIndexedIndirect(VkCommandBuffer command, VkBuffer buffer, VkDeviceSize offset,
                                     uint32_t count, uint32_t stride) {
    if (!enabled.drawIndirectFirstInstance || count != 1) atomic_fetch_add(&invalidIndirect, 1);
    auditIndirectBuffer(buffer, offset);
    actualDrawIndexedIndirect(command, buffer, offset, count, stride);
}

static void auditDispatchIndirect(VkCommandBuffer command, VkBuffer buffer, VkDeviceSize offset) {
    auditIndirectBuffer(buffer, offset);
    atomic_fetch_add(&indirectDispatches, 1);
    atomic_store(&indirectDispatchOffset, offset);
    actualDispatchIndirect(command, buffer, offset);
}

static VkResult auditCreateImageView(VkDevice device, const VkImageViewCreateInfo *info,
                                     const VkAllocationCallbacks *allocator, VkImageView *view) {
    VkResult result = actualCreateImageView(device, info, allocator, view);
    if (result == VK_SUCCESS) {
        const uint32_t fields[] = { info->viewType, info->subresourceRange.baseMipLevel,
            info->subresourceRange.levelCount, info->subresourceRange.baseArrayLayer,
            info->subresourceRange.layerCount };
        VkComponentMapping c = info->components;
        BOOL identity = (!c.r || c.r == VK_COMPONENT_SWIZZLE_R) && (!c.g || c.g == VK_COMPONENT_SWIZZLE_G) &&
                        (!c.b || c.b == VK_COMPONENT_SWIZZLE_B) && (!c.a || c.a == VK_COMPONENT_SWIZZLE_A);
        @synchronized (NSProcessInfo.processInfo) {
            if (!imageViews) imageViews = [NSMutableArray array];
            [imageViews addObject:[NSData dataWithBytes:fields length:sizeof fields]];
            if (!identityViews) identityViews = [NSMutableDictionary dictionary];
            identityViews[@((uintptr_t)*view)] = @(identity);
        }
    }
    return result;
}

static void auditDestroyImageView(VkDevice device, VkImageView view, const VkAllocationCallbacks *allocator) {
    @synchronized (NSProcessInfo.processInfo) { [identityViews removeObjectForKey:@((uintptr_t)view)]; }
    actualDestroyImageView(device, view, allocator);
}

static VkResult auditCreateFramebuffer(VkDevice device, const VkFramebufferCreateInfo *info,
                                       const VkAllocationCallbacks *allocator, VkFramebuffer *framebuffer) {
    if (!(info->flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT)) {
        @synchronized (NSProcessInfo.processInfo) {
            for (uint32_t i = 0; i < info->attachmentCount; i++) {
                if (![identityViews[@((uintptr_t)info->pAttachments[i])] boolValue]) {
                    atomic_fetch_add(&invalidAttachments, 1);
                    printf("AUDIT_INVALID_ATTACHMENT VUID-VkFramebufferCreateInfo-pAttachments-00884 index=%u\n", i);
                }
            }
        }
    }
    return actualCreateFramebuffer(device, info, allocator, framebuffer);
}

static void auditDraw(VkCommandBuffer commands, uint32_t vertices, uint32_t instances,
                      uint32_t firstVertex, uint32_t firstInstance) {
    actualDraw(commands, vertices, instances, firstVertex, firstInstance);
    atomic_fetch_add(&draws, 1);
}

static void auditDrawIndexed(VkCommandBuffer commands, uint32_t indices, uint32_t instances,
                             uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) {
    actualDrawIndexed(commands, indices, instances, firstIndex, vertexOffset, firstInstance);
    atomic_fetch_add(&draws, 1);
}

static void auditDispatch(VkCommandBuffer commands, uint32_t x, uint32_t y, uint32_t z) {
    atomic_fetch_add(&dispatches, 1);
    actualDispatch(commands, x, y, z);
}

static VkResult auditComputePipelines(VkDevice device, VkPipelineCache cache, uint32_t count,
                                     const VkComputePipelineCreateInfo *infos,
                                     const VkAllocationCallbacks *allocator, VkPipeline *pipelines) {
    VkResult result = actualComputePipelines(device, cache, count, infos, allocator, pipelines);
    for (uint32_t i = 0; i < count; i++) if (pipelines[i]) atomic_fetch_add(&computePipelines, 1);
    return result;
}

static VkResult auditGraphicsPipelines(VkDevice device, VkPipelineCache cache, uint32_t count,
                                      const VkGraphicsPipelineCreateInfo *infos,
                                      const VkAllocationCallbacks *allocator, VkPipeline *pipelines) {
    VkResult result = actualGraphicsPipelines(device, cache, count, infos, allocator, pipelines);
    for (uint32_t i = 0; i < count; i++) if (pipelines[i]) atomic_fetch_add(&graphicsPipelines, 1);
    return result;
}

static VkResult rejectSubmission(VkQueue queue, uint32_t count, const VkSubmitInfo *submits, VkFence fence) {
    (void)queue; (void)count; (void)submits; (void)fence;
    atomic_fetch_add(&submissions, 1);
    const char *mode = getenv("RDNA4_AUDIT_SUBMIT");
    if (mode && !strcmp(mode, "submit-error")) return VK_ERROR_UNKNOWN;
    return mode && strcmp(mode, "reject") ? VK_SUCCESS : VK_ERROR_DEVICE_LOST;
}

static VkResult auditCreateFence(VkDevice device, const VkFenceCreateInfo *info,
                                 const VkAllocationCallbacks *allocator, VkFence *fence) {
    if (!strcmp(getenv("RDNA4_AUDIT_SUBMIT"), "fence-error")) return VK_ERROR_OUT_OF_HOST_MEMORY;
    return actualCreateFence(device, info, allocator, fence);
}

static VkResult auditWait(VkDevice device, uint32_t count, const VkFence *fences,
                          VkBool32 all, uint64_t timeout) {
    (void)device; (void)count; (void)fences; (void)all; (void)timeout;
    unsigned call = atomic_fetch_add(&waits, 1);
    const char *mode = getenv("RDNA4_AUDIT_SUBMIT");
    if (!strcmp(mode, "complete")) return VK_SUCCESS;
    if (!strcmp(mode, "lost")) return VK_ERROR_DEVICE_LOST;
    if (!strcmp(mode, "wait-error")) return VK_ERROR_OUT_OF_HOST_MEMORY;
    if (!strcmp(mode, "early-timeout") && call) return VK_SUCCESS;
    return VK_TIMEOUT;
}

static void auditDestroyFence(VkDevice device, VkFence fence, const VkAllocationCallbacks *allocator) {
    atomic_fetch_add(&fenceDestroys, 1);
    actualDestroyFence(device, fence, allocator);
}

static void auditFreeCommands(VkDevice device, VkCommandPool pool, uint32_t count,
                              const VkCommandBuffer *commands) {
    atomic_fetch_add(&commandFrees, count);
    actualFreeCommandBuffers(device, pool, count, commands);
}

static void auditDestroyBuffer(VkDevice device, VkBuffer buffer, const VkAllocationCallbacks *allocator) {
    atomic_fetch_add(&bufferDestroys, 1);
    @synchronized (NSProcessInfo.processInfo) { [bufferUsage removeObjectForKey:@((uintptr_t)buffer)]; }
    actualDestroyBuffer(device, buffer, allocator);
}

static VkResult auditCreateDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo *ci,
                                 const VkAllocationCallbacks *allocator, VkDevice *device) {
    VkResult result = actualCreateDevice(physical, ci, allocator, device);
    if (result != VK_SUCCESS) return result;
    capturedDevice = YES;
    enabled = ci->pEnabledFeatures ? *ci->pEnabledFeatures : (VkPhysicalDeviceFeatures){0};
    for (const VkBaseInStructure *p = ci->pNext; p; p = p->pNext) {
        switch (p->sType) {
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2:
            enabled = ((const VkPhysicalDeviceFeatures2 *)p)->features;
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES: {
            const VkPhysicalDeviceVulkan11Features *f = (const void *)p;
            enabledStorage16 = f->storageBuffer16BitAccess;
            enabledUniformStorage16 = f->uniformAndStorageBuffer16BitAccess;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES: {
            const VkPhysicalDeviceVulkan12Features *f = (const void *)p;
            enabledFloat16 = f->shaderFloat16; enabledInt8 = f->shaderInt8;
            enabledStorage8 = f->storageBuffer8BitAccess;
            enabledUniformStorage8 = f->uniformAndStorageBuffer8BitAccess;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES: {
            const VkPhysicalDeviceShaderFloat16Int8Features *f = (const void *)p;
            enabledFloat16 = f->shaderFloat16; enabledInt8 = f->shaderInt8;
            break;
        }
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES:
            enabledDemote = ((const VkPhysicalDeviceVulkan13Features *)p)->shaderDemoteToHelperInvocation;
            break;
        case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES:
            enabledDemote = ((const VkPhysicalDeviceShaderDemoteToHelperInvocationFeatures *)p)->shaderDemoteToHelperInvocation;
            break;
        default: break;
        }
    }
    printf("CAPTURE_DEVICE int64=%u int16=%u int8=%u float16=%u demote=%u\n",
           enabled.shaderInt64, enabled.shaderInt16, enabledInt8, enabledFloat16, enabledDemote);
    return result;
}

static void auditCopyBuffer(VkCommandBuffer command, VkBuffer source, VkBuffer destination,
                            uint32_t count, const VkBufferCopy *regions) {
    (void)command; (void)source; (void)destination; (void)regions;
    atomic_fetch_add(&bufferCopies, count);
}

static void auditFillBuffer(VkCommandBuffer command, VkBuffer buffer, VkDeviceSize offset,
                            VkDeviceSize size, uint32_t value) {
    (void)command; (void)buffer; (void)offset; (void)size; (void)value;
    atomic_fetch_add(&bufferFills, 1);
}

static PFN_vkVoidFunction auditGIPA(VkInstance instance, const char *name) {
    PFN_vkVoidFunction proc = actualGIPA(instance, name);
    if (proc && getenv("RDNA4_AUDIT_BUFFER_BLIT")) {
        if (!strcmp(name, "vkCmdCopyBuffer")) return (PFN_vkVoidFunction)auditCopyBuffer;
        if (!strcmp(name, "vkCmdFillBuffer")) return (PFN_vkVoidFunction)auditFillBuffer;
    }
    if (proc && !strcmp(name, "vkCreateDevice")) {
        actualCreateDevice = (PFN_vkCreateDevice)proc;
        return (PFN_vkVoidFunction)auditCreateDevice;
    }
    if (proc && !strcmp(name, "vkQueueSubmit")) return (PFN_vkVoidFunction)rejectSubmission;
    if (proc && !strcmp(name, "vkCreateComputePipelines")) {
        actualComputePipelines = (PFN_vkCreateComputePipelines)proc;
        return (PFN_vkVoidFunction)auditComputePipelines;
    }
    if (proc && !strcmp(name, "vkCreateGraphicsPipelines")) {
        actualGraphicsPipelines = (PFN_vkCreateGraphicsPipelines)proc;
        return (PFN_vkVoidFunction)auditGraphicsPipelines;
    }
    if (proc && !strcmp(name, "vkCmdDispatch")) {
        actualDispatch = (PFN_vkCmdDispatch)proc;
        return (PFN_vkVoidFunction)auditDispatch;
    }
    if (proc && !strcmp(name, "vkCmdDispatchIndirect")) {
        actualDispatchIndirect = (PFN_vkCmdDispatchIndirect)proc;
        return (PFN_vkVoidFunction)auditDispatchIndirect;
    }
    if (proc && !strcmp(name, "vkCmdCopyImage")) {
        actualCopyImage = (PFN_vkCmdCopyImage)proc;
        return (PFN_vkVoidFunction)auditCopyImage;
    }
    if (proc && !strcmp(name, "vkCmdDraw")) {
        actualDraw = (PFN_vkCmdDraw)proc;
        return (PFN_vkVoidFunction)auditDraw;
    }
    if (proc && !strcmp(name, "vkCmdDrawIndexed")) {
        actualDrawIndexed = (PFN_vkCmdDrawIndexed)proc;
        return (PFN_vkVoidFunction)auditDrawIndexed;
    }
    if (proc && !strcmp(name, "vkCmdDrawIndirect")) {
        actualDrawIndirect = (PFN_vkCmdDrawIndirect)proc;
        return (PFN_vkVoidFunction)auditDrawIndirect;
    }
    if (proc && !strcmp(name, "vkCmdDrawIndexedIndirect")) {
        actualDrawIndexedIndirect = (PFN_vkCmdDrawIndexedIndirect)proc;
        return (PFN_vkVoidFunction)auditDrawIndexedIndirect;
    }
    if (proc && !strcmp(name, "vkCreateBuffer")) {
        actualCreateBuffer = (PFN_vkCreateBuffer)proc;
        return (PFN_vkVoidFunction)auditCreateBuffer;
    }
    if (proc && !strcmp(name, "vkDestroyBuffer")) {
        actualDestroyBuffer = (PFN_vkDestroyBuffer)proc;
        return (PFN_vkVoidFunction)auditDestroyBuffer;
    }
    if (proc && !strcmp(name, "vkCreateImageView")) {
        actualCreateImageView = (PFN_vkCreateImageView)proc;
        return (PFN_vkVoidFunction)auditCreateImageView;
    }
    if (proc && !strcmp(name, "vkDestroyImageView")) {
        actualDestroyImageView = (PFN_vkDestroyImageView)proc;
        return (PFN_vkVoidFunction)auditDestroyImageView;
    }
    if (proc && !strcmp(name, "vkCreateFramebuffer")) {
        actualCreateFramebuffer = (PFN_vkCreateFramebuffer)proc;
        return (PFN_vkVoidFunction)auditCreateFramebuffer;
    }
    if (proc && getenv("RDNA4_AUDIT_SUBMIT")) {
        if (!strcmp(name, "vkCreateFence")) {
            actualCreateFence = (PFN_vkCreateFence)proc;
            return (PFN_vkVoidFunction)auditCreateFence;
        }
        if (!strcmp(name, "vkWaitForFences")) return (PFN_vkVoidFunction)auditWait;
        if (!strcmp(name, "vkDestroyFence")) {
            actualDestroyFence = (PFN_vkDestroyFence)proc;
            return (PFN_vkVoidFunction)auditDestroyFence;
        }
        if (!strcmp(name, "vkFreeCommandBuffers")) {
            actualFreeCommandBuffers = (PFN_vkFreeCommandBuffers)proc;
            return (PFN_vkVoidFunction)auditFreeCommands;
        }
    }
    return proc;
}

static void *auditDlsym(void *handle, const char *name) {
    void *symbol = dlsym(handle, name);
    if (symbol && !strcmp(name, "vk_icdGetInstanceProcAddr")) {
        actualGIPA = (PFN_vkGetInstanceProcAddr)symbol;
        return (void *)auditGIPA;
    }
    return symbol;
}

__attribute__((used, section("__DATA,__interpose")))
static const struct { const void *replacement, *original; } interposeDlsym = {
    (const void *)auditDlsym, (const void *)dlsym
};

BOOL rdna4_audit_capabilities(NSData *spv, NSString *name) {
    if (spv.length < 20 || spv.length % 4) return NO;
    const uint32_t *words = spv.bytes;
    BOOL valid = words[0] == 0x07230203;
    for (NSUInteger pos = 5; valid && pos < spv.length / 4;) {
        uint32_t count = words[pos] >> 16, opcode = words[pos] & 0xffff;
        if (!count || count > spv.length / 4 - pos) return NO;
        if (opcode == 17 && count == 2) { // OpCapability
            const char *missing = NULL;
            switch (words[pos + 1]) {
            case 9: if (!enabledFloat16) missing = "Float16 / shaderFloat16"; break;
            case 11: if (!enabled.shaderInt64) missing = "Int64 / shaderInt64"; break;
            case 22: if (!enabled.shaderInt16) missing = "Int16 / shaderInt16"; break;
            case 39: if (!enabledInt8) missing = "Int8 / shaderInt8"; break;
            case 4433: if (!enabledStorage16) missing = "StorageBuffer16BitAccess"; break;
            case 4434: if (!enabledUniformStorage16) missing = "UniformAndStorageBuffer16BitAccess"; break;
            case 4448: if (!enabledStorage8) missing = "StorageBuffer8BitAccess"; break;
            case 4449: if (!enabledUniformStorage8) missing = "UniformAndStorageBuffer8BitAccess"; break;
            case 34: case 45: if (!enabled.imageCubeArray) missing = "ImageCubeArray / SampledCubeArray / imageCubeArray"; break;
            case 5379: if (!enabledDemote) missing = "DemoteToHelperInvocation"; break;
            default: break;
            }
            if (missing) {
                printf("FAIL_FEATURE %s requires %s\n", name.UTF8String, missing);
                return NO;
            }
        }
        pos += count;
    }
    return valid;
}

#else
extern BOOL rdna4_audit_has_device(void);
extern BOOL rdna4_audit_capabilities(NSData *spv, NSString *name);
extern unsigned rdna4_audit_submission_count(void);

@interface RDNA4CorpusFunction : NSObject
@property(nonatomic, copy) NSString *name;
@property(nonatomic, copy) NSData *bitcodeData;
@end
@implementation RDNA4CorpusFunction
@end

@interface NSObject (RDNA4CorpusPipeline)
- (instancetype)initWithDevice:(id)device function:(id)function error:(NSError **)error;
- (const uint32_t *)nominalLocal;
- (void *)pipelineForLocal:(const uint32_t *)local;
- (void *)pipelineForLocal:(const uint32_t *)local indirect:(BOOL)indirect;
@end

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        if (argc != 3) {
            fprintf(stderr, "Usage: test-system-pipelines BUNDLE AIR_CORPUS\n");
            return 2;
        }
        setvbuf(stdout, NULL, _IONBF, 0);
        setenv("RADV_DARWIN_MOCK", "1", 1);
        setenv("RADV_DARWIN_MOCK_GC", "gfx1200", 1);
        setenv("RADV_DEBUG", "nocache", 1);
        setenv("MESA_SHADER_CACHE_DISABLE", "true", 1);
        NSString *path = @(argv[1]), *corpus = @(argv[2]);
        NSError *error = nil;
        NSBundle *bundle = [NSBundle bundleWithPath:path];
        if (![bundle loadAndReturnError:&error]) {
            fprintf(stderr, "FAIL bundle: %s\n", error.description.UTF8String);
            return 1;
        }
        Class pipelineClass = objc_getClass("RDNA4ComputePipelineState");
        if (!pipelineClass) return 1;
        NSString *manifest = [NSString stringWithContentsOfFile:
            [corpus stringByAppendingPathComponent:@"manifest.tsv"]
            encoding:NSUTF8StringEncoding error:&error];
        if (!manifest) {
            fprintf(stderr, "FAIL manifest: %s\n", error.description.UTF8String);
            return 1;
        }
        NSUInteger kernels = 0, failed = 0, indirectVariants = 0;
        for (NSString *line in [manifest componentsSeparatedByString:@"\n"]) {
            @autoreleasepool {
                if (!line.length) continue;
                NSArray<NSString *> *fields = [line componentsSeparatedByString:@"\t"];
                if (fields.count != 5) return 2;
                NSString *sha = fields[0];
                NSData *json = [NSData dataWithContentsOfFile:[bundle.resourcePath
                    stringByAppendingPathComponent:[NSString stringWithFormat:@"spvcache/%@.meta.json", sha]]];
                NSDictionary *meta = json ? [NSJSONSerialization JSONObjectWithData:json options:0 error:&error] : nil;
                if (![meta[@"stage"] isEqual:@"Kernel"]) continue;
                kernels++;
                RDNA4CorpusFunction *fn = [RDNA4CorpusFunction new];
                fn.name = fields[1];
                fn.bitcodeData = [NSData dataWithContentsOfFile:[corpus
                    stringByAppendingPathComponent:[sha stringByAppendingString:@".air"]]];
                if (!fn.bitcodeData.length) return 2;
                error = nil;
                id pipeline = [[pipelineClass alloc] initWithDevice:nil function:fn error:&error];
                if (!pipeline) {
                    failed++;
                    printf("FAIL %s: %s\n", fn.name.UTF8String, error.description.UTF8String);
                    continue;
                }
                const uint32_t *local = [pipeline nominalLocal];
                if (!local || ![pipeline pipelineForLocal:local]) {
                    failed++;
                    printf("FAIL %s: no compiled nominal pipeline\n", fn.name.UTF8String);
                    continue;
                }
                const uint32_t shapes[][3] = {{1,1,1}, {4,2,3}, {64,1,1}};
                BOOL dynamic = [meta[@"kernel_dispatch"] isKindOfClass:[NSDictionary class]] &&
                    meta[@"kernel_dispatch"][@"ThreadsDynamic"] != nil;
                BOOL variantsOK = YES;
                for (unsigned i = 0; i < (dynamic ? 3u : 1u); ++i) {
                    const uint32_t *shape = dynamic ? shapes[i] : local;
                    if (![pipeline pipelineForLocal:shape indirect:YES]) {
                        variantsOK = NO;
                        printf("FAIL %s: indirect pipeline local=%u,%u,%u\n", fn.name.UTF8String, shape[0], shape[1], shape[2]);
                    } else indirectVariants++;
                }
                if (!variantsOK) { failed++; continue; }
                printf("PASS %s local=%u,%u,%u\n", fn.name.UTF8String, local[0], local[1], local[2]);
            }
        }
        NSUInteger modules = 0, featureFailures = 0;
        for (NSString *line in [manifest componentsSeparatedByString:@"\n"]) {
            if (!line.length) continue;
            NSArray<NSString *> *fields = [line componentsSeparatedByString:@"\t"];
            NSData *spv = [NSData dataWithContentsOfFile:[bundle.resourcePath
                stringByAppendingPathComponent:[NSString stringWithFormat:@"spvcache/%@.spv", fields[0]]]];
            modules++;
            if (!rdna4_audit_has_device() || !rdna4_audit_capabilities(spv, fields[1])) featureFailures++;
        }
        printf("SYSTEM_COMPUTE_PIPELINES kernels=%lu failed=%lu; mock BOs; no submissions\n",
               (unsigned long)kernels, (unsigned long)failed);
        printf("SYSTEM_INDIRECT_COMPUTE_PIPELINES variants=%lu; mock BOs; no submissions\n",
               (unsigned long)indirectVariants);
        printf("SYSTEM_SHADER_FEATURES modules=%lu failed=%lu device_captured=%d\n",
               (unsigned long)modules, (unsigned long)featureFailures, rdna4_audit_has_device());
        return kernels && !failed && modules && !featureFailures && rdna4_audit_has_device() &&
            !rdna4_audit_submission_count() ? 0 : 1;
    }
}
#endif
