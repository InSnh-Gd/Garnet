static _Atomic uint64_t rdna4_argument_pool_descriptors;

@interface RDNA4ArgumentPool : NSObject {
@public
    VkDescriptorPool handle;
@private
    uint64_t _descriptorCount;
}
- (instancetype)initWithInfo:(const VkDescriptorPoolCreateInfo *)info error:(NSError **)error;
@end

@implementation RDNA4ArgumentPool
- (instancetype)initWithInfo:(const VkDescriptorPoolCreateInfo *)info error:(NSError **)error {
    self = [super init];
    if (!self) return nil;
    uint64_t count = 0;
    for (uint32_t i = 0; i < info->poolSizeCount; ++i) {
        if (info->pPoolSizes[i].descriptorCount > UINT64_MAX - count) {
            if (error) *error = rdna4_err(49, @"GPU argument pool descriptor count overflows");
            return nil;
        }
        count += info->pPoolSizes[i].descriptorCount;
    }
    uint64_t limit = RDNA4R.descriptorLimits.maxUpdateAfterBindDescriptorsInAllPools;
    uint64_t used = atomic_load(&rdna4_argument_pool_descriptors);
    do {
        if (used > limit || count > limit - used) {
            if (error) *error = rdna4_err(49, @"GPU argument pools exceed the backend global descriptor limit");
            return nil;
        }
    } while (!atomic_compare_exchange_weak(&rdna4_argument_pool_descriptors, &used, used + count));
    VkResult result = vkCreateDescriptorPool(RDNA4R.dev, info, NULL, &handle);
    if (result != VK_SUCCESS) {
        handle = VK_NULL_HANDLE;
        atomic_fetch_sub(&rdna4_argument_pool_descriptors, count);
        if (error) *error = rdna4_err(49, [NSString stringWithFormat:@"GPU argument descriptor pool = %d", result]);
        return nil;
    }
    _descriptorCount = count;
    return self;
}
- (void)dealloc {
    if (handle && RDNA4R.ok) vkDestroyDescriptorPool(RDNA4R.dev, handle, NULL);
    atomic_fetch_sub(&rdna4_argument_pool_descriptors, _descriptorCount);
}
@end
