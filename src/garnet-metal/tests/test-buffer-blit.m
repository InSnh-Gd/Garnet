#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <unistd.h>

@interface NSObject (RDNA4BufferBlitTest)
- (BOOL)rdna4Finish:(NSError **)error;
@end

#ifdef RDNA4_BUFFER_BLIT_AUDIT
extern unsigned rdna4_audit_buffer_copies(void);
extern unsigned rdna4_audit_buffer_fills(void);
extern unsigned rdna4_audit_submission_count(void);
#endif

static void fail(NSString *message) __attribute__((noreturn));
static void fail(NSString *message) {
    fprintf(stderr, "BUFFER_BLIT_FAIL %s\n", message.UTF8String);
    _exit(1);
}

int main(int argc, const char **argv) {
    @autoreleasepool {
        if (argc == 2 && !strcmp(argv[1], "--help")) {
            puts("Usage: test-buffer-blit BUNDLE MODE | --reference MODE\nModes: copy, same, fill, byte-fill, zero, bad-source, bad-destination, bad-overflow, bad-overlap, bad-align, bad-fill, ended, sticky");
            return 0;
        }
        if (argc != 3) return 2;
#ifdef RDNA4_BUFFER_BLIT_AUDIT
        setenv("RDNA4_AUDIT_BUFFER_BLIT", "1", 1);
#endif
        NSString *mode = @(argv[2]);
        BOOL rejected = [mode hasPrefix:@"bad-"] || [mode isEqual:@"ended"] || [mode isEqual:@"sticky"];
        id<MTLDevice> device = nil;
        if (!strcmp(argv[1], "--reference")) {
            if (rejected) return 2;
            device = MTLCreateSystemDefaultDevice();
        } else {
            NSError *error = nil;
            NSBundle *bundle = [NSBundle bundleWithPath:@(argv[1])];
            if (![bundle loadAndReturnError:&error]) fail(error.description);
            device = [[[bundle classNamed:@"GarnetDevice"] alloc] init];
        }
        id<MTLCommandQueue> queue = [device newCommandQueue];
        id<MTLBuffer> source = [device newBufferWithLength:256 options:MTLResourceStorageModeShared];
        id<MTLBuffer> destination = [device newBufferWithLength:256 options:MTLResourceStorageModeShared];
        if (!device || !queue || !source || !destination) fail(@"device/queue/buffers unavailable");
        uint8_t *bytes = source.contents;
        for (NSUInteger i = 0; i < 256; ++i) bytes[i] = (uint8_t)(i * 17 + 3);
        memset(destination.contents, 0xA5, 256);
        uint8_t expected[256];
        memset(expected, 0xA5, sizeof expected);
        id<MTLCommandBuffer> commands = [queue commandBuffer];
        id<MTLBlitCommandEncoder> encoder = [commands blitCommandEncoder];
        if (!commands || !encoder) fail(@"command buffer/encoder unavailable");
        if ([mode isEqual:@"copy"]) {
            [encoder copyFromBuffer:source sourceOffset:4 toBuffer:destination destinationOffset:12 size:36];
            memcpy(expected + 12, bytes + 4, 36);
        } else if ([mode isEqual:@"same"]) {
            memcpy(destination.contents, bytes, 256);
            memcpy(expected, bytes, 256);
            [encoder copyFromBuffer:destination sourceOffset:0 toBuffer:destination destinationOffset:64 size:64];
            memcpy(expected + 64, bytes, 64);
        } else if ([mode isEqual:@"fill"] || [mode isEqual:@"byte-fill"]) {
            NSRange range = [mode isEqual:@"fill"] ? NSMakeRange(4, 248) : NSMakeRange(1, 7);
            [encoder fillBuffer:destination range:range value:0x3C];
            memset(expected + range.location, 0x3C, range.length);
        } else if ([mode isEqual:@"zero"]) {
            [encoder copyFromBuffer:source sourceOffset:256 toBuffer:destination destinationOffset:256 size:0];
            [encoder fillBuffer:destination range:NSMakeRange(256, 0) value:0x3C];
        } else if ([mode isEqual:@"bad-source"]) {
            [encoder copyFromBuffer:source sourceOffset:252 toBuffer:destination destinationOffset:0 size:8];
        } else if ([mode isEqual:@"bad-destination"]) {
            [encoder copyFromBuffer:source sourceOffset:0 toBuffer:destination destinationOffset:252 size:8];
        } else if ([mode isEqual:@"bad-overflow"]) {
            [encoder copyFromBuffer:source sourceOffset:NSUIntegerMax - 3 toBuffer:destination destinationOffset:0 size:8];
        } else if ([mode isEqual:@"bad-overlap"]) {
            [encoder copyFromBuffer:source sourceOffset:0 toBuffer:source destinationOffset:4 size:8];
        } else if ([mode isEqual:@"bad-align"]) {
            [encoder copyFromBuffer:source sourceOffset:1 toBuffer:destination destinationOffset:0 size:8];
        } else if ([mode isEqual:@"bad-fill"]) {
            [encoder fillBuffer:destination range:NSMakeRange(NSUIntegerMax - 3, 8) value:0x3C];
        } else if ([mode isEqual:@"ended"]) {
            [encoder endEncoding];
            [encoder fillBuffer:destination range:NSMakeRange(0, 4) value:0x3C];
        } else if ([mode isEqual:@"sticky"]) {
            [encoder fillBuffer:destination range:NSMakeRange(252, 8) value:0x3C];
            [encoder copyFromBuffer:source sourceOffset:0 toBuffer:destination destinationOffset:0 size:4];
        } else return 2;
        if (![mode isEqual:@"ended"]) [encoder endEncoding];
        if (rejected) {
            NSError *error = nil;
            if ([(id)commands rdna4Finish:&error] || !error) fail(@"invalid buffer operation accepted");
#ifdef RDNA4_BUFFER_BLIT_AUDIT
            if (rdna4_audit_buffer_copies() || rdna4_audit_buffer_fills() || rdna4_audit_submission_count())
                fail(@"invalid buffer operation reached Vulkan encoding/submission");
#endif
            printf("BUFFER_BLIT_REJECTION_PASS %s error=%s; no GPU submission\n", argv[2], error.localizedDescription.UTF8String);
            return 0;
        }
        [commands commit];
        [commands waitUntilCompleted];
        if (commands.status != MTLCommandBufferStatusCompleted || commands.error) fail(commands.error.description);
        if (memcmp(destination.contents, expected, sizeof expected)) {
            const uint8_t *actual = destination.contents;
            for (NSUInteger i = 0; i < sizeof expected; ++i)
                if (actual[i] != expected[i]) fail([NSString stringWithFormat:@"%@ byte %lu actual=%u expected=%u",
                    mode, (unsigned long)i, actual[i], expected[i]]);
        }
        if ([mode isEqual:@"byte-fill"]) {
            for (NSUInteger start = 0; start < 8; ++start) for (NSUInteger count = 1; count <= 12; ++count) {
                memset(destination.contents, 0xA5, 256);
                memset(expected, 0xA5, sizeof expected);
                uint8_t value = (uint8_t)(start * 13 + count);
                memset(expected + start, value, count);
                id<MTLCommandBuffer> edgeCommands = [queue commandBuffer];
                id<MTLBlitCommandEncoder> edgeEncoder = [edgeCommands blitCommandEncoder];
                [edgeEncoder fillBuffer:destination range:NSMakeRange(start, count) value:value];
                [edgeEncoder endEncoding]; [edgeCommands commit]; [edgeCommands waitUntilCompleted];
                if (edgeCommands.status != MTLCommandBufferStatusCompleted || edgeCommands.error ||
                    memcmp(destination.contents, expected, sizeof expected))
                    fail([NSString stringWithFormat:@"byte fill differs at start=%lu count=%lu",
                        (unsigned long)start, (unsigned long)count]);
            }
            puts("BUFFER_BLIT_EDGES_PASS 96 byte ranges including all word boundaries");
        }
        printf("BUFFER_BLIT_EXECUTION_PASS %s bytes=256\n", argv[2]);
    }
    return 0;
}
