// Check missing-sampler descriptors without assigning undefined inputs a GPU oracle.
#include "GarnetDevice.m"

static unsigned checks, attempts, publications, retains;
static BOOL failComparison;
static VkSampler ordinary, comparison;
static void require(BOOL ok, const char *why) {
    ++checks;
    if (!ok) { fprintf(stderr, "NULL_SAMPLER_BINDINGS_FAIL %s\n", why); exit(1); }
}

@interface NullSamplerProbe : NSObject
@property NSString *failure;
@end
@implementation NullSamplerProbe
- (id)device { return nil; }
- (void)rdna4Fail:(NSString *)why { self.failure = why; }
- (void)rdna4RetainArgument:(id)object { require(object != nil, "retain actual fallback sampler"); ++retains; }
@end

static VkResult VKAPI_CALL create(VkDevice device, const VkSamplerCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkSampler *handle) {
    (void)device; (void)allocator;
    ++attempts;
    require(info && handle && info->sType==VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        "real fallback sampler create info");
    require(!info->unnormalizedCoordinates && info->mipLodBias==0 &&
        !info->anisotropyEnable && info->minFilter==VK_FILTER_NEAREST &&
        info->magFilter==VK_FILTER_NEAREST, "normalized ordinary/comparison fallback configuration");
    require(info->compareOp==VK_COMPARE_OP_NEVER, "default compare Never stays Never");
    if (info->compareEnable && failComparison) return VK_ERROR_OUT_OF_HOST_MEMORY;
    *handle = (VkSampler)(uintptr_t)(0x7000u+attempts);
    if (info->compareEnable) comparison=*handle; else ordinary=*handle;
    return VK_SUCCESS;
}
static void VKAPI_CALL destroy(VkDevice device, VkSampler handle,
    const VkAllocationCallbacks *allocator) { (void)device; (void)handle; (void)allocator; }
static void VKAPI_CALL update(VkDevice device, uint32_t count, const VkWriteDescriptorSet *writes,
    uint32_t copies, const VkCopyDescriptorSet *copy) {
    (void)device; (void)copy;
    require(count==1 && !copies && writes[0].descriptorType==VK_DESCRIPTOR_TYPE_SAMPLER &&
        writes[0].descriptorCount==1, "one production sampler descriptor publication");
    VkSampler expected = writes[0].dstBinding==736 ? comparison : ordinary;
    require(expected && writes[0].pImageInfo && writes[0].pImageInfo[0].sampler==expected,
        "descriptor uses the representation for its actual operation");
    ++publications;
}

int main(void) {
    @autoreleasepool {
        RDNA4R.ok=YES; RDNA4R.dev=(VkDevice)(uintptr_t)0x1234;
        RDNA4R.lim.maxSamplerAllocationCount=16;
        RDNA4R.lim.maxSamplerAnisotropy=1;
        vkCreateSampler=create; vkDestroySampler=destroy; vkUpdateDescriptorSets=update;
        SEL selector=@selector(rdna4FillSet:bindings:count:state:stage:);
        BOOL (*fill)(id,SEL,VkDescriptorSet,const RDNA4PB *,uint32_t,const RDNA4StageState *,const char *) =
            (void *)class_getMethodImplementation(RDNA4CommandBuffer.class,selector);
        RDNA4StageState state={0};
        RDNA4PB normal={.binding=160,.type=VK_DESCRIPTOR_TYPE_SAMPLER,.count=1,.normalizedCoordinates=YES};
        RDNA4PB compare={.binding=736,.type=VK_DESCRIPTOR_TYPE_SAMPLER,.count=1,
            .comparisonSampler=YES,.normalizedCoordinates=YES};
        NullSamplerProbe *first=[NullSamplerProbe new];
        require(fill(first,selector,(VkDescriptorSet)(uintptr_t)0x8888,&normal,1,&state,"test") &&
            !first.failure && ordinary && !comparison && publications==1 && retains==1,
            "ordinary missing sampler gets only a non-comparison representation");
        failComparison=YES;
        NullSamplerProbe *failed=[NullSamplerProbe new];
        require(!fill(failed,selector,(VkDescriptorSet)(uintptr_t)0x8888,&compare,1,&state,"test") &&
            failed.failure && !comparison && publications==1 && retains==1,
            "comparison allocation failure publishes nothing and keeps ordinary handle");
        failComparison=NO;
        NullSamplerProbe *retry=[NullSamplerProbe new];
        require(fill(retry,selector,(VkDescriptorSet)(uintptr_t)0x8888,&compare,1,&state,"test") &&
            !retry.failure && comparison && comparison!=ordinary && publications==2 && retains==2,
            "comparison retry creates compare-enabled Never representation");
        unsigned before=attempts;
        for (unsigned i=0;i<4;++i) {
            NullSamplerProbe *probe=[NullSamplerProbe new];
            require(fill(probe,selector,(VkDescriptorSet)(uintptr_t)0x8888,i&1?&compare:&normal,1,&state,"test") &&
                !probe.failure, "repeated mixed descriptor bindings succeed");
        }
        require(attempts==before && atomic_load(&rdna4_sampler_allocations)==2,
            "failed charge rolled back and exactly two cached handles remain");
        require(!RDNA4R.tried, "host check never opens a GPU backend");
        printf("NULL_SAMPLER_BINDINGS_PASS checks=%u attempts=%u publications=%u ordinary_comparison=1 "
            "allocation_failure_retry=1 undefined_gpu_oracle=0 gpu_execution=0\n",checks,attempts,publications);
    }
    return 0;
}
