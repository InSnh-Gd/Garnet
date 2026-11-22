#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <unistd.h>

#ifdef RDNA4_INDIRECT_COMPUTE_AUDIT
extern unsigned rdna4_audit_indirect_calls(void);
extern unsigned rdna4_audit_invalid_indirect(void);
extern unsigned rdna4_audit_indirect_dispatches(void);
extern uint64_t rdna4_audit_indirect_dispatch_offset(void);
extern unsigned rdna4_audit_dispatches(void);
extern unsigned rdna4_audit_submission_count(void);
#endif

@interface NSObject (RDNA4IndirectComputePreparation)
- (BOOL)rdna4Finish:(NSError **)error;
@end

static void fail(NSString *message) __attribute__((noreturn));
static void fail(NSString *message) {
    fprintf(stderr, "INDIRECT_COMPUTE_FAIL %s\n", message.UTF8String);
    _exit(1);
}

int main(int argc, const char *argv[]) {
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        puts("Usage: sudo test-indirect-compute BUNDLE|--reference grid|generated|zero|bad-offset|bad-size|bad-local [--prepare]\n"
             "Checks private indirect arguments, 3D grid builtins and GPU-generated dispatch counts. --prepare submits nothing.");
        return 0;
    }
    if (argc < 3 || argc > 4 || (argc == 4 && strcmp(argv[3], "--prepare"))) return 2;
    BOOL reference = !strcmp(argv[1], "--reference"), prepare = argc == 4;
    const char *mode = argv[2];
    if (strcmp(mode, "grid") && strcmp(mode, "generated") && strcmp(mode, "zero") &&
        strcmp(mode, "bad-offset") && strcmp(mode, "bad-size") && strcmp(mode, "bad-local")) return 2;
    BOOL invalid = !strncmp(mode, "bad-", 4), generated = !strcmp(mode, "generated");
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
            NSString *source = @"#include <metal_stdlib>\nusing namespace metal;\n"
                "kernel void make_counts(device uint *a [[buffer(0)]], uint i [[thread_position_in_grid]]) {\n"
                " if (i==0) { a[4]=3; a[5]=2; a[6]=2; } }\n"
                "kernel void inspect_grid(device uint *out [[buffer(0)]], uint3 tid [[thread_position_in_grid]],\n"
                " uint3 gid [[threadgroup_position_in_grid]], uint3 lid [[thread_position_in_threadgroup]],\n"
                " uint3 grid [[threads_per_grid]], uint3 groups [[threadgroups_per_grid]],\n"
                " uint3 local [[threads_per_threadgroup]]) {\n"
                " uint i=(tid.z*8+tid.y)*16+tid.x; uint b=1+i*19;\n"
                " out[b]=0x12340000u+i; for(uint d=0;d<3;d++) { out[b+1+d]=tid[d]; out[b+4+d]=gid[d];\n"
                " out[b+7+d]=lid[d]; out[b+10+d]=grid[d]; out[b+13+d]=groups[d]; out[b+16+d]=local[d]; } }\n";
            NSError *error = nil;
            id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
            if (!library) fail([NSString stringWithFormat:@"source library: %@", error]);
            id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:[library newFunctionWithName:@"inspect_grid"] error:&error];
            id<MTLComputePipelineState> generator = [device newComputePipelineStateWithFunction:[library newFunctionWithName:@"make_counts"] error:&error];
            if (!pipeline || !generator) fail([NSString stringWithFormat:@"compute pipeline: %@", error]);
            enum { words = 1 + 16 * 8 * 6 * 19 + 1 };
            uint32_t *expected = malloc(words * sizeof(uint32_t));
            if (!expected) fail(@"allocation failed");
            for (unsigned i = 0; i < words; ++i) expected[i] = 0xa5a5a5a5u;
            id<MTLBuffer> output = [device newBufferWithBytes:expected length:words * sizeof(uint32_t) options:MTLResourceStorageModeShared];
            uint32_t arguments[16];
            for (unsigned i = 0; i < 16; ++i) arguments[i] = 99;
            arguments[4] = generated || !strcmp(mode, "zero") ? 0 : 3;
            arguments[5] = 2; arguments[6] = 2;
            id<MTLBuffer> upload = [device newBufferWithBytes:arguments length:sizeof arguments options:MTLResourceStorageModeShared];
            NSUInteger length = !strcmp(mode, "bad-size") ? 24 : sizeof arguments;
            id<MTLBuffer> indirect = [device newBufferWithLength:length options:MTLResourceStorageModePrivate];
            id<MTLCommandQueue> queue = [device newCommandQueue];
            id<MTLCommandBuffer> commands = [queue commandBuffer];
            if (!output || !upload || !indirect || indirect.contents || !commands) fail(@"resources or private-storage contract");
            id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
            [blit copyFromBuffer:upload sourceOffset:0 toBuffer:indirect destinationOffset:0 size:length];
            [blit endEncoding];
            id<MTLComputeCommandEncoder> encoder = [commands computeCommandEncoder];
            if (generated) {
                [encoder setComputePipelineState:generator];
                [encoder setBuffer:indirect offset:0 atIndex:0];
                [encoder dispatchThreadgroups:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(1,1,1)];
            }
            [encoder setComputePipelineState:pipeline];
            [encoder setBuffer:output offset:0 atIndex:0];
            [encoder dispatchThreadgroupsWithIndirectBuffer:indirect indirectBufferOffset:!strcmp(mode, "bad-offset") ? 18 : 16
                threadsPerThreadgroup:MTLSizeMake(!strcmp(mode, "bad-local") ? 0 : 4,2,3)];
            [encoder endEncoding];
            if (prepare) {
                BOOL ready = [(id)commands rdna4Finish:&error];
                if (ready == invalid || (!ready && !error)) fail([NSString stringWithFormat:@"preparation=%u invalid=%u error=%@", ready, invalid, error]);
#ifdef RDNA4_INDIRECT_COMPUTE_AUDIT
                unsigned expected = invalid ? 0 : 1;
                if (rdna4_audit_invalid_indirect() || rdna4_audit_indirect_calls() != expected ||
                    rdna4_audit_indirect_dispatches() != expected || rdna4_audit_submission_count() ||
                    rdna4_audit_dispatches() != (generated ? 1u : 0u) ||
                    (expected && rdna4_audit_indirect_dispatch_offset() != 16))
                    fail(@"RADV indirect dispatch, buffer usage, offset or submission audit");
                printf("INDIRECT_COMPUTE_AUDIT_PASS mode=%s buffer_usage_valid=1 commands=%u submissions=0\n",
                    mode, rdna4_audit_indirect_dispatches());
#endif
                printf("INDIRECT_COMPUTE_PREPARE_PASS mode=%s private_arguments=1 submissions=0\n", mode);
                _exit(0);
            }
            [commands commit];
            [commands waitUntilCompleted];
            if (invalid) {
                if (commands.status != MTLCommandBufferStatusError || !commands.error) fail(@"invalid dispatch accepted");
                printf("INDIRECT_COMPUTE_REJECTION_PASS mode=%s\n", mode);
                _exit(0);
            }
            if (commands.status != MTLCommandBufferStatusCompleted || commands.error)
                fail([NSString stringWithFormat:@"command status=%lu error=%@", (unsigned long)commands.status, commands.error]);
            if (strcmp(mode, "zero")) {
                for (unsigned z = 0; z < 6; ++z) for (unsigned y = 0; y < 4; ++y) for (unsigned x = 0; x < 12; ++x) {
                    unsigned index = (z * 8 + y) * 16 + x, base = 1 + index * 19;
                    unsigned position[] = {x,y,z}, local[] = {4,2,3}, grid[] = {12,4,6}, groups[] = {3,2,2};
                    expected[base] = 0x12340000u + index;
                    for (unsigned d = 0; d < 3; ++d) {
                        expected[base+1+d] = position[d]; expected[base+4+d] = position[d]/local[d];
                        expected[base+7+d] = position[d]%local[d]; expected[base+10+d] = grid[d];
                        expected[base+13+d] = groups[d]; expected[base+16+d] = local[d];
                    }
                }
            }
            const uint32_t *actual = output.contents;
            for (unsigned i = 0; i < words; ++i) if (actual[i] != expected[i])
                fail([NSString stringWithFormat:@"word %u actual=%#x expected=%#x", i, actual[i], expected[i]]);
            printf("INDIRECT_COMPUTE_EXECUTION_PASS mode=%s words=%u private_arguments=1 reference=%u\n", mode, words, reference);
            _exit(0);
        } @catch (NSException *exception) { fail(exception.description); }
    }
}
