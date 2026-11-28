#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include <stdlib.h>
#include <unistd.h>

static void require(BOOL ok, NSString *message) {
    if (!ok) {
        fprintf(stderr, "GPU_ADDRESS_FAIL %s\n", message.UTF8String);
        exit(1);
    }
}

static void check_buffer(id<MTLBuffer> buffer, NSMutableArray *live, NSString *name) {
    require(buffer != nil, [name stringByAppendingString:@" allocation failed"]);
    uint64_t address = buffer.gpuAddress;
    require(address != 0, [name stringByAppendingString:@" GPU address is zero"]);
    require(address == buffer.gpuAddress, [name stringByAppendingString:@" GPU address changed"]);
    require(address <= UINT64_MAX - buffer.length, @"GPU address range overflows");
    for (id<MTLBuffer> other in live) {
        uint64_t start = other.gpuAddress;
        require(address >= start + other.length || start >= address + buffer.length,
                [name stringByAppendingString:@" overlaps a live allocation"]);
    }
    [live addObject:buffer];
    printf("GPU_ADDRESS_BUFFER_PASS %s length=%lu address=0x%llx\n", name.UTF8String,
           (unsigned long)buffer.length, (unsigned long long)address);
}

int main(int argc, const char **argv) {
    @autoreleasepool {
        if (argc == 2 && strcmp(argv[1], "--help") == 0) {
            puts("Usage: test-gpu-address BUNDLE | --reference\nChecks stable, nonoverlapping GPU addresses for real Metal buffers.");
            return 0;
        }
        if (argc != 2) return 2;
        id<MTLDevice> device = nil;
        if (strcmp(argv[1], "--reference") == 0) device = MTLCreateSystemDefaultDevice();
        else {
            NSBundle *bundle = [NSBundle bundleWithPath:@(argv[1])];
            NSError *error = nil;
            require([bundle loadAndReturnError:&error], error.description ?: @"bundle load failed");
            Class cls = [bundle classNamed:@"GarnetDevice"];
            require(cls != Nil, @"Metal class missing");
            device = [[cls alloc] init];
        }
        require(device != nil, @"Metal device unavailable");
        NSMutableArray *live = [NSMutableArray array];
        for (NSUInteger i = 0; i < 10; ++i)
            check_buffer([device newBufferWithLength:4096 + 256 * i options:MTLResourceStorageModeShared], live,
                         [NSString stringWithFormat:@"shared-%lu", (unsigned long)i]);
        check_buffer([device newBufferWithLength:8192 options:MTLResourceStorageModeManaged], live, @"managed");
        check_buffer([device newBufferWithLength:8192 options:MTLResourceStorageModePrivate], live, @"private");
        MTLHeapDescriptor *descriptor = [MTLHeapDescriptor new];
        descriptor.size = 1u << 20;
        descriptor.storageMode = MTLStorageModePrivate;
        id<MTLHeap> heap = [device newHeapWithDescriptor:descriptor];
        require(heap != nil, @"heap allocation failed");
        check_buffer([heap newBufferWithLength:4096 options:MTLResourceStorageModePrivate], live, @"heap");
        size_t page = (size_t)getpagesize();
        size_t length = page > 65536 ? page : 65536;
        void *allocation = NULL;
        require(posix_memalign(&allocation, length, length) == 0, @"client allocation failed");
        __block BOOL freed = NO;
        id<MTLBuffer> imported = [device newBufferWithBytesNoCopy:allocation length:length
            options:MTLResourceStorageModeShared deallocator:^(void *pointer, NSUInteger size) {
                (void)size;
                freed = YES;
                free(pointer);
            }];
        if (!imported) free(allocation);
        check_buffer(imported, live, @"imported");
        require(imported.contents == allocation && !freed, @"client memory ownership changed");
        printf("GPU_ADDRESS_PASS %lu live buffers on %s\n", (unsigned long)live.count, device.name.UTF8String);
    }
    return 0;
}
