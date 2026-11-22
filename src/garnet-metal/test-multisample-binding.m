// Exercise reflected MS type and production descriptor admission without opening a GPU.
#include "GarnetDevice.m"

static unsigned checks, writes;
static void require(BOOL ok, const char *message) {
    ++checks;
    if (!ok) { fprintf(stderr, "MULTISAMPLE_BINDING_FAIL %s\n", message); exit(1); }
}

@interface SampleTexture : RDNA4Texture
@property NSUInteger testSamples;
@end
@implementation SampleTexture
- (NSUInteger)rdna4Samples { return self.testSamples; }
- (VkImageView)vkView { return (VkImageView)(uintptr_t)0x1234; }
@end

@interface SampleBindingProbe : NSObject
@property NSString *failure;
@property unsigned retained;
@end
@implementation SampleBindingProbe
- (id)device { return nil; }
- (void)rdna4Fail:(NSString *)message { self.failure = message; }
- (void)rdna4RetainEncoded:(id)object { require(object != nil, "retain actual texture"); self.retained++; }
@end

static void VKAPI_CALL capture(VkDevice device, uint32_t count,
    const VkWriteDescriptorSet *updates, uint32_t copies, const VkCopyDescriptorSet *copy) {
    (void)device; (void)copy;
    require(count == 1 && !copies && updates[0].descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE &&
        updates[0].dstBinding == 32 && updates[0].descriptorCount == 1 &&
        updates[0].pImageInfo[0].imageView == (VkImageView)(uintptr_t)0x1234,
        "one matching texture descriptor published");
    writes++;
}

static void reflectionKinds(void) {
    RDNA4sId ids[8] = {0};
    ids[1] = (RDNA4sId){.op=22, .width=32};
    ids[2] = (RDNA4sId){.op=25, .elem=1, .dim=1, .sampled=1};
    ids[3] = (RDNA4sId){.op=32, .elem=2};
    ids[4] = (RDNA4sId){.op=59, .elem=3, .sc=0, .set=0, .bind=32};
    RDNA4sMod module = {.bound=7, .id=ids};
    RDNA4sRefl *reflection = calloc(1, sizeof *reflection);
    require(reflection != NULL, "reflection allocation");
    for (unsigned ms=0; ms<=1; ++ms) {
        ids[2].ms=ms;
        require(!rdna4s_reflect(&module, 2, reflection), "reflect sampled image");
        require(reflection->bind[0][32].imageSampleKinds == (ms ? 2 : 1),
            "reflection retains the actual image MS bit");
    }
    ids[5] = ids[2]; ids[5].ms=0;
    ids[6] = (RDNA4sId){.op=32, .elem=5};
    ids[7] = (RDNA4sId){.op=59, .elem=6, .sc=0, .set=0, .bind=32};
    require(!rdna4s_reflect(&module, 2, reflection) &&
        reflection->bind[0][32].imageSampleKinds == 3, "mixed image declarations are not mistaken for one shape");
    free(reflection);
}

int main(void) {
    @autoreleasepool {
        reflectionKinds();
        vkUpdateDescriptorSets = capture;
        SEL selector = @selector(rdna4FillSet:bindings:count:state:stage:);
        BOOL (*fill)(id, SEL, VkDescriptorSet, const RDNA4PB *, uint32_t,
            const RDNA4StageState *, const char *) =
            (void *)class_getMethodImplementation(RDNA4CommandBuffer.class, selector);
        SampleTexture *texture = [SampleTexture new];
        RDNA4StageState state = {0}; state.tex[0] = texture;
        for (unsigned kinds=0; kinds<=3; ++kinds) for (unsigned samples=1; samples<=4; samples*=2) {
            SampleBindingProbe *probe = [SampleBindingProbe new];
            texture.testSamples=samples;
            RDNA4PB binding = {.binding=32, .type=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                .count=1, .imageSampleKinds=(uint8_t)kinds};
            BOOL expected=(kinds ? kinds : 1)==(samples>1 ? 2 : 1);
            unsigned before=writes;
            BOOL ok=fill(probe, selector, (VkDescriptorSet)(uintptr_t)0x5678,
                &binding, 1, &state, "test");
            require(ok==expected && writes-before==(unsigned)expected,
                "descriptor is published only for matching single/MS types");
            require(expected ? (!probe.failure && probe.retained==1) :
                ([probe.failure containsString:@"multisample type"] && !probe.retained),
                "mismatch refuses before descriptor publication or retention");
        }
        vkUpdateDescriptorSets=NULL;
        require(!RDNA4R.tried && RDNA4R.dev==VK_NULL_HANDLE, "no GPU backend opened");
        printf("MULTISAMPLE_BINDING_PASS checks=%u matrix=12 gpu_execution=0\n", checks);
    }
    return 0;
}
