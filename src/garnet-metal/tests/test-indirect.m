#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <unistd.h>

#ifdef RDNA4_INDIRECT_AUDIT
extern BOOL rdna4_audit_indirect_first_instance(void);
extern unsigned rdna4_audit_indirect_calls(void);
extern unsigned rdna4_audit_invalid_indirect(void);
extern unsigned rdna4_audit_submission_count(void);
#endif

@interface NSObject (RDNA4IndirectPreparation)
- (BOOL)rdna4Finish:(NSError **)error;
@end

static void fail(NSString *message) __attribute__((noreturn));
static void fail(NSString *message) {
    fprintf(stderr, "INDIRECT_FAIL %s\n", message.UTF8String);
    _exit(1);
}

static void finish(id<MTLCommandBuffer> commands) {
    [commands commit];
    [commands waitUntilCompleted];
    if (commands.status != MTLCommandBufferStatusCompleted || commands.error)
        fail([NSString stringWithFormat:@"command status=%lu error=%@", (unsigned long)commands.status, commands.error]);
}

int main(int argc, const char *argv[]) {
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        puts("Usage: sudo test-indirect BUNDLE|--reference draw|indexed|multi|zero|bad-offset|bad-size|bad-index [--prepare]\n"
             "Executes indirect rendering with private GPU argument buffers. --prepare uses real RADV with mock BOs and submits nothing.");
        return 0;
    }
    if (argc < 3 || argc > 4 || (argc == 4 && strcmp(argv[3], "--prepare"))) return 2;
    BOOL reference = !strcmp(argv[1], "--reference"), prepare = argc == 4;
    const char *mode = argv[2];
    if (strcmp(mode, "draw") && strcmp(mode, "indexed") && strcmp(mode, "multi") && strcmp(mode, "zero") &&
        strcmp(mode, "bad-offset") && strcmp(mode, "bad-size") && strcmp(mode, "bad-index")) return 2;
    BOOL invalid = !strncmp(mode, "bad-", 4), indexed = !strcmp(mode, "indexed") || !strcmp(mode, "bad-index");
    if ((reference && (prepare || invalid)) || (!reference && geteuid() != 0)) return 2;
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(60);
    @autoreleasepool {
        @try {
            id<MTLDevice> device;
            if (reference) device = MTLCreateSystemDefaultDevice();
            else {
                setenv("RDNA4M_ALLOW", "1", 1);
                setenv("RDNA4M_TEST_INPROC", "1", 1);
                setenv("MVK_CONFIG_LOG_LEVEL", "2", 1);
                setenv("MVK_CONFIG_USE_MTLHEAP", "0", 1);
                if (prepare) {
                    setenv("RADV_DARWIN_MOCK", "1", 1);
                    setenv("RADV_DARWIN_MOCK_GC", "gfx1200", 1);
                }
                NSError *error = nil;
                if (![[NSBundle bundleWithPath:@(argv[1])] loadAndReturnError:&error]) fail(error.description);
                device = [[objc_getClass("GarnetDevice") alloc] init];
                ((void (*)(id, SEL))objc_msgSend)(device, sel_registerName("initLimits"));
            }
            if (!device) fail(@"device unavailable");
            id<MTLCommandQueue> queue = [device newCommandQueue];
            if (!queue) fail(@"queue unavailable");
            NSString *source = @"#include <metal_stdlib>\nusing namespace metal;\n"
                "struct V { float4 position [[position]]; float4 color; };\n"
                "vertex V indirect_vs(const device packed_float2 *p [[buffer(0)]], uint v [[vertex_id]], uint i [[instance_id]]) {\n"
                " V o; o.position=float4(p[v],0,1); o.color=i==2 ? float4(0.25,0.5,0.75,1) : float4(1,0,0,1); return o; }\n"
                "fragment float4 indirect_fs(V in [[stage_in]]) { return in.color; }\n";
            NSError *error = nil;
            id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
            if (!library) fail([NSString stringWithFormat:@"source library: %@", error]);
            MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
            pd.vertexFunction = [library newFunctionWithName:@"indirect_vs"];
            pd.fragmentFunction = [library newFunctionWithName:@"indirect_fs"];
            pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
            id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithDescriptor:pd error:&error];
            if (!pipeline) fail([NSString stringWithFormat:@"render pipeline: %@", error]);
            const float positions[] = { 99,99, -1,-1, 3,-1, -1,3 };
            const uint16_t indices[] = { 99,99,99, 0,1,2 };
            id<MTLBuffer> vertices = [device newBufferWithBytes:positions length:sizeof positions options:MTLResourceStorageModeShared];
            id<MTLBuffer> index = [device newBufferWithBytes:indices length:sizeof indices options:MTLResourceStorageModeShared];
            uint32_t arguments[16];
            for (unsigned i = 0; i < 16; ++i) arguments[i] = 99;
            arguments[4] = !strcmp(mode, "zero") ? 0 : 3;
            arguments[5] = 1;
            arguments[6] = 1;
            arguments[7] = indexed ? 1 : 2;
            arguments[8] = 2;
            if (!strcmp(mode, "multi")) {
                arguments[7] = 0;
                arguments[8] = 3; arguments[9] = 1; arguments[10] = 1; arguments[11] = 2;
            }
            id<MTLBuffer> upload = [device newBufferWithBytes:arguments length:sizeof arguments options:MTLResourceStorageModeShared];
            NSUInteger argumentLength = !strcmp(mode, "bad-size") ? 24 : sizeof arguments;
            id<MTLBuffer> indirect = [device newBufferWithLength:argumentLength options:MTLResourceStorageModePrivate];
            if (!vertices || !index || !upload || !indirect || indirect.contents) fail(@"buffer creation or private-storage contract");
            const NSUInteger width = 16, height = 16;
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                width:width height:height mipmapped:NO];
            td.storageMode = MTLStorageModePrivate;
            td.usage = MTLTextureUsageRenderTarget;
            id<MTLTexture> target = [device newTextureWithDescriptor:td];
            id<MTLCommandBuffer> commands = [queue commandBuffer];
            id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
            [blit copyFromBuffer:upload sourceOffset:0 toBuffer:indirect destinationOffset:0 size:argumentLength];
            [blit endEncoding];
            MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
            pass.colorAttachments[0].texture = target;
            pass.colorAttachments[0].loadAction = MTLLoadActionClear;
            pass.colorAttachments[0].storeAction = MTLStoreActionStore;
            pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
            id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
            if (!target || !commands || !blit || !encoder) fail(@"encoding resources unavailable");
            [encoder setRenderPipelineState:pipeline];
            [encoder setVertexBuffer:vertices offset:0 atIndex:0];
            NSUInteger offset = !strcmp(mode, "bad-offset") ? 18 : 16;
            if (indexed)
                [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexType:MTLIndexTypeUInt16 indexBuffer:index
                    indexBufferOffset:!strcmp(mode, "bad-index") ? 3 : 4 indirectBuffer:indirect indirectBufferOffset:offset];
            else [encoder drawPrimitives:MTLPrimitiveTypeTriangle indirectBuffer:indirect indirectBufferOffset:offset];
            if (!strcmp(mode, "multi")) {
                [encoder setViewport:(MTLViewport){ width / 2, 0, width / 2, height, 0, 1 }];
                [encoder drawPrimitives:MTLPrimitiveTypeTriangle indirectBuffer:indirect indirectBufferOffset:32];
            }
            [encoder endEncoding];
            if (prepare) {
                BOOL ready = [(id)commands rdna4Finish:&error];
                if (ready == invalid || (!ready && !error)) fail([NSString stringWithFormat:@"preparation=%u invalid=%u error=%@", ready, invalid, error]);
#ifdef RDNA4_INDIRECT_AUDIT
                unsigned expected = invalid ? 0u : !strcmp(mode, "multi") ? 2u : 1u;
                if (!rdna4_audit_indirect_first_instance() || rdna4_audit_invalid_indirect() ||
                    rdna4_audit_indirect_calls() != expected || rdna4_audit_submission_count())
                    fail(@"indirect Vulkan feature, buffer usage, command count or submission contract");
                printf("INDIRECT_AUDIT_PASS mode=%s first_instance_enabled=1 buffer_usage_valid=1 commands=%u submissions=0\n",
                    mode, rdna4_audit_indirect_calls());
#endif
                printf("INDIRECT_PREPARE_PASS mode=%s private_arguments=1 GPU_submissions=0\n", mode);
                _exit(0);
            }
            if (invalid) {
                [commands commit];
                [commands waitUntilCompleted];
                if (commands.status != MTLCommandBufferStatusError || !commands.error) fail(@"invalid argument accepted");
                printf("INDIRECT_REJECTION_PASS mode=%s\n", mode);
                _exit(0);
            }
            finish(commands);
            id<MTLBuffer> pixels = [device newBufferWithLength:width * height * 4 options:MTLResourceStorageModeShared];
            commands = [queue commandBuffer];
            blit = [commands blitCommandEncoder];
            if (!pixels || !commands || !blit) fail(@"readback resources unavailable");
            [blit copyFromTexture:target sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
                sourceSize:MTLSizeMake(width,height,1) toBuffer:pixels destinationOffset:0
                destinationBytesPerRow:width*4 destinationBytesPerImage:width*height*4];
            [blit endEncoding];
            finish(commands);
            const uint8_t expected[] = { 64,128,191,255 }, clear[] = { 0,0,0,255 }, red[] = { 255,0,0,255 };
            const uint8_t *actual = pixels.contents;
            for (NSUInteger i = 0; i < width * height * 4; ++i) {
                const uint8_t *want = !strcmp(mode, "zero") ? clear :
                    !strcmp(mode, "multi") && (i / 4) % width < width / 2 ? red : expected;
                int difference = (int)actual[i] - (int)want[i % 4];
                if (difference < -1 || difference > 1)
                    fail([NSString stringWithFormat:@"pixel byte %lu actual=%u expected=%u", (unsigned long)i, actual[i], want[i % 4]]);
            }
            printf("INDIRECT_EXECUTION_PASS mode=%s pixels=%lu private_arguments=1 base_instance=2 reference=%u\n",
                mode, (unsigned long)(width * height), reference);
            _exit(0);
        } @catch (NSException *exception) { fail(exception.description); }
    }
}
