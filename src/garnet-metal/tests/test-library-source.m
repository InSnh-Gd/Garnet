// Build against Foundation/Metal and the test-only librdna4featureaudit.dylib.
// Run as root in the macOS test VM: test-library-source BUNDLE
// The generic initializer isolates CPU APIs from accelerator-port admission;
// this tests source compilation and encoding, never hardware initialization.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <unistd.h>

extern BOOL rdna4_audit_has_device(void);
extern unsigned rdna4_audit_submission_count(void);

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        if (argc != 2 || geteuid() != 0) {
            fprintf(stderr, "Usage in test VM: sudo test-library-source BUNDLE\n");
            return 2;
        }
        setvbuf(stdout, NULL, _IONBF, 0);
        setenv("RADV_DARWIN_MOCK", "1", 1);
        setenv("RADV_DARWIN_MOCK_GC", "gfx1200", 1);
        setenv("RADV_DEBUG", "nocache", 1);
        setenv("MESA_SHADER_CACHE_DISABLE", "true", 1);
        setenv("RDNA4M_ALLOW", "1", 1);
        setenv("RDNA4M_TEST_INPROC", "1", 1);
        NSError *error = nil;
        NSBundle *bundle = [NSBundle bundleWithPath:@(argv[1])];
        if (![bundle loadAndReturnError:&error]) {
            fprintf(stderr, "FAIL bundle: %s\n", error.description.UTF8String);
            return 1;
        }
        Class cls = objc_getClass("GarnetDevice");
        id<MTLDevice> device = [[cls alloc] init];
        if (!device) return 1;
        NSString *source = @"#include <metal_stdlib>\nusing namespace metal;\n"
            "kernel void native_dynamic_add(device uint *p [[buffer(0)]], uint i [[thread_position_in_grid]]) { p[i] += 1; }\n"
            "constant uint native_increment [[function_constant(7)]];\n"
            "kernel void native_dynamic_constant(device uint *p [[buffer(0)]], uint i [[thread_position_in_grid]]) { p[i] += native_increment; }\n"
            "vertex float4 native_dynamic_vertex(uint i [[vertex_id]]) { return float4(float((i << 1) & 2) * 2.0 - 1.0, float(i & 2) * 2.0 - 1.0, 0.0, 1.0); }\n"
            "fragment half4 native_dynamic_fragment() { return half4(0.25h, 0.5h, 0.75h, 1.0h); }\n";
        id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
        id<MTLFunction> function = [library newFunctionWithName:@"native_dynamic_add"];
        SEL bitcode = sel_registerName("bitcodeData");
        NSData *air = [function respondsToSelector:bitcode] ? ((id (*)(id, SEL))objc_msgSend)(function, bitcode) : nil;
        if (!library || !function || !air.length) {
            fprintf(stderr, "FAIL source library: %s\n", error.description.UTF8String);
            return 1;
        }
        printf("SOURCE_LIBRARY_PASS AIR_BYTES=%lu\n", (unsigned long)air.length);
        error = nil;
        id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:function error:&error];
        if (!pipeline) {
            fprintf(stderr, "FAIL source pipeline: %s\n", error.description.UTF8String);
            return 1;
        }
        uint32_t initial[64] = {0};
        id<MTLBuffer> buffer = [device newBufferWithBytes:initial length:sizeof initial options:MTLResourceStorageModeShared];
        id<MTLCommandQueue> queue = [device newCommandQueue];
        id<MTLCommandBuffer> commands = [queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder = [commands computeCommandEncoder];
        if (!buffer || !queue || !commands || !encoder) return 1;
        [encoder setComputePipelineState:pipeline];
        [encoder setBuffer:buffer offset:0 atIndex:0];
        [encoder dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
        [encoder endEncoding];
        if (commands.error || rdna4_audit_submission_count() || !rdna4_audit_has_device()) {
            fprintf(stderr, "FAIL encoding: %s submissions=%u\n", commands.error.description.UTF8String,
                    rdna4_audit_submission_count());
            return 1;
        }
        MTLRenderPipelineDescriptor *descriptor = [MTLRenderPipelineDescriptor new];
        descriptor.vertexFunction = [library newFunctionWithName:@"native_dynamic_vertex"];
        descriptor.fragmentFunction = [library newFunctionWithName:@"native_dynamic_fragment"];
        descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
        id<MTLRenderPipelineState> renderPipeline = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
        if (!renderPipeline) {
            fprintf(stderr, "FAIL render pipeline: %s\n", error.description.UTF8String);
            return 1;
        }
        MTLTextureDescriptor *textureDescriptor = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float width:8 height:8 mipmapped:NO];
        textureDescriptor.storageMode = MTLStorageModePrivate;
        textureDescriptor.usage = MTLTextureUsageRenderTarget;
        id<MTLTexture> texture = [device newTextureWithDescriptor:textureDescriptor];
        if (!texture) return 1;
        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = texture;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLCommandBuffer> renderCommands = [queue commandBuffer];
        id<MTLRenderCommandEncoder> renderEncoder = [renderCommands renderCommandEncoderWithDescriptor:pass];
        if (!renderCommands || !renderEncoder) return 1;
        [renderEncoder setRenderPipelineState:renderPipeline];
        [renderEncoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [renderEncoder endEncoding];
        if (renderCommands.error || rdna4_audit_submission_count()) {
            fprintf(stderr, "FAIL render encoding: %s\n", renderCommands.error.description.UTF8String);
            return 1;
        }
        printf("SOURCE_RENDER_ENCODING_PASS RGBA16Float target; no submissions\n");
        NSMutableArray<NSData *> *constantAIR = [NSMutableArray array];
        for (uint32_t increment = 3; increment <= 7; increment += 4) {
            MTLFunctionConstantValues *values = [MTLFunctionConstantValues new];
            [values setConstantValue:&increment type:MTLDataTypeUInt atIndex:7];
            error = nil;
            id<MTLFunction> specialized = [library newFunctionWithName:@"native_dynamic_constant" constantValues:values error:&error];
            NSData *specializedAIR = [specialized respondsToSelector:bitcode] ? ((id (*)(id, SEL))objc_msgSend)(specialized, bitcode) : nil;
            if (!specialized || !specializedAIR.length) {
                fprintf(stderr, "FAIL specialized function %u: %s\n", increment, error.description.UTF8String);
                return 1;
            }
            [constantAIR addObject:specializedAIR];
            id<MTLComputePipelineState> specializedPipeline = [device newComputePipelineStateWithFunction:specialized error:&error];
            if (!specializedPipeline) {
                fprintf(stderr, "FAIL specialized pipeline %u: %s\n", increment, error.description.UTF8String);
                return 1;
            }
            printf("SPECIALIZED_PIPELINE_PASS increment=%u AIR_BYTES=%lu\n", increment, (unsigned long)specializedAIR.length);
        }
        if ([constantAIR[0] isEqualToData:constantAIR[1]]) {
            fprintf(stderr, "FAIL function constants share AIR; pipeline specialization must be audited\n");
            return 1;
        }
        error = nil;
        id<MTLLibrary> invalid = [device newLibraryWithSource:@"kernel void broken( {" options:nil error:&error];
        if (invalid || !error) {
            fprintf(stderr, "FAIL invalid source must return nil and NSError\n");
            return 1;
        }
        printf("INVALID_SOURCE_REJECTED %s\n", error.domain.UTF8String);
        printf("SOURCE_PIPELINE_ENCODING_PASS no submissions; GPU results untested\n");
        // The generic object has no accelerator teardown state. Exit explicitly.
        _exit(0);
    }
}
