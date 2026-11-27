#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <CommonCrypto/CommonDigest.h>
#include <unistd.h>
#include "GarnetDevice.m"

@interface PipelineOOMFunction : NSObject
@property(nonatomic, copy) NSString *name;
@property(nonatomic, copy) NSData *bitcodeData;
@property(nonatomic) MTLFunctionType functionType;
@end
@implementation PipelineOOMFunction
@end

static void require(BOOL condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL %s\n", message);
        exit(1);
    }
}

static id<MTLFunction> cachedFunction(NSBundle *bundle, NSString *directory, NSString *name,
                                     NSString *sourceHash, MTLFunctionType stage) {
    PipelineOOMFunction *function = [PipelineOOMFunction new];
    function.name = name;
    function.functionType = stage;
    function.bitcodeData = [name dataUsingEncoding:NSUTF8StringEncoding];
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(function.bitcodeData.bytes, (CC_LONG)function.bitcodeData.length, digest);
    char hash[65];
    for (NSUInteger index = 0; index < sizeof digest; ++index)
        snprintf(hash + index * 2, 3, "%02x", digest[index]);
    for (NSString *suffix in @[@".spv", @".meta.json"]) {
        NSString *source = [[bundle.resourcePath stringByAppendingPathComponent:@"spvcache"]
            stringByAppendingPathComponent:[sourceHash stringByAppendingString:suffix]];
        NSData *data = [NSData dataWithContentsOfFile:source];
        NSString *destination = [directory stringByAppendingPathComponent:[@(hash) stringByAppendingString:suffix]];
        require(data && [data writeToFile:destination atomically:YES], "cached desktop shader fixture");
    }
    return (id<MTLFunction>)function;
}

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        require(argc == 3 && geteuid() == 0, "usage: sudo test-pipeline-oom BUNDLE EMPTY_CACHE_DIR");
        setvbuf(stdout, NULL, _IONBF, 0);
        alarm(90);
        unsetenv("RADV_DARWIN_MOCK");
        unsetenv("RADV_DARWIN_HOST_BO");
        setenv("RDNA4M_ALLOW", "1", 1);
        setenv("RDNA4M_TEST_INPROC", "1", 1);
        setenv("RDNA4M_PCACHE_DIR", argv[2], 1);
        NSString *side = [@(argv[2]) stringByAppendingPathComponent:@"shaders"];
        require([[NSFileManager defaultManager] createDirectoryAtPath:side
            withIntermediateDirectories:YES attributes:nil error:NULL], "isolated shader directory");
        setenv("RDNA4M_TEST_SIDE_DIR", side.fileSystemRepresentation, 1);
        NSBundle *bundle = [NSBundle bundleWithPath:@(argv[1])];
        GarnetDevice *device = [GarnetDevice new];
        require(device != nil, "production Metal device");
        ((void (*)(id, SEL))objc_msgSend)(device, sel_registerName("initLimits"));
        NSError *error = nil;
        require(rdna4_radv_open(&error), "native RADV open");
        int memoryType = rdna4_find_mem(UINT32_MAX, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        require(memoryType >= 0, "visible VRAM memory type");
        VkDeviceMemory pressure[1040] = {0};
        unsigned allocations = 0, refusals = 0;
        for (unsigned index = 0; index < 1040; ++index) {
            VkDeviceMemory memory = VK_NULL_HANDLE;
            VkMemoryAllocateInfo allocation = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                .allocationSize = index < 1024 ? 1024 * 1024 : 65536,
                .memoryTypeIndex = (uint32_t)memoryType };
            VkResult result = vkAllocateMemory(RDNA4R.dev, &allocation, NULL, &memory);
            if (result == VK_SUCCESS) pressure[allocations++] = memory;
            else {
                require(result == VK_ERROR_OUT_OF_DEVICE_MEMORY, "pressure refusal is device OOM");
                refusals++;
                if (index < 1024) index = 1023;
            }
        }
        require(allocations && refusals, "visible pool exhausted without submissions");
        printf("PRESSURE allocations=%u refusals=%u memory_type=%d\n", allocations, refusals, memoryType);
        MTLRenderPipelineDescriptor *descriptor = [MTLRenderPipelineDescriptor new];
        descriptor.vertexFunction = cachedFunction(bundle, side, @"VfxU10",
            @"21428566c2b79b41a3923eb1e40a3f2c42b1a05bcddc12c96bd48c4f590c6a29", MTLFunctionTypeVertex);
        descriptor.fragmentFunction = cachedFunction(bundle, side, @"TimgXcu_Iscd",
            @"259019c159f0cf9f664c65a34c23089dbddb7543798a82c2d268896fe08a4e2e", MTLFunctionTypeFragment);
        descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        descriptor.colorAttachments[0].blendingEnabled = YES;
        descriptor.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorOne;
        descriptor.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        descriptor.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
        descriptor.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        MTLVertexDescriptor *vertex = [MTLVertexDescriptor vertexDescriptor];
        const MTLVertexFormat formats[] = { MTLVertexFormatFloat4, MTLVertexFormatFloat2,
            MTLVertexFormatFloat2, MTLVertexFormatFloat4 };
        const NSUInteger offsets[] = { 0, 16, 24, 32 };
        for (NSUInteger index = 0; index < 4; ++index) {
            vertex.attributes[index].format = formats[index];
            vertex.attributes[index].offset = offsets[index];
            vertex.attributes[index].bufferIndex = 0;
        }
        vertex.layouts[0].stride = 48;
        descriptor.vertexDescriptor = vertex;
        id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
        require(!pipeline && error && [error.localizedDescription containsString:@"vkCreateGraphicsPipelines = -2"],
            "pipeline safely reports device OOM");
        printf("PASS pipeline reports device OOM\n");
        for (unsigned index = 0; index < allocations; ++index)
            vkFreeMemory(RDNA4R.dev, pressure[index], NULL);
        error = nil;
        pipeline = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
        require(pipeline && !error, "same pipeline recovers after pressure is freed");
        printf("PASS pipeline recovers without a partial cache entry; gpu_submission=0\n");
        return 0;
    }
}
