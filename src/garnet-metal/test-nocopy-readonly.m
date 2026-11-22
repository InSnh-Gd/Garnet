// Build/run on macOS:
//   clang -arch x86_64 -fobjc-arc -Wall -Wextra -Werror -framework Foundation -framework Metal \
//     -o /tmp/test-nocopy-readonly test-nocopy-readonly.m
//   /tmp/test-nocopy-readonly /path/to/GarnetMetal.bundle [DEVICE_CLASS]
//   /tmp/test-nocopy-readonly --reference
// Pure formatter check (host C): cc -O1 -Wall -Wextra -Werror -o /tmp/test-nocopy-protection test-nocopy-protection.c && /tmp/test-nocopy-protection
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <spawn.h>
#include <sys/wait.h>
extern char **environ;

static int failures;
static BOOL referenceMode;
static BOOL writableImportAvailable;
static BOOL referenceChild;
static const char *executablePath;
static int referenceOperation = -1;

static void fill_pattern(uint8_t *p, size_t len) {
    for (size_t i = 0; i < len; ++i) p[i] = (uint8_t)(i * 37u + 11u);
}

static BOOL pattern_matches(const uint8_t *p, size_t len, size_t offset) {
    for (size_t i = 0; i < len; ++i) if (p[i] != (uint8_t)((i + offset) * 37u + 11u)) return NO;
    return YES;
}

// A normal-buffer blit probes command execution before touching a host import.
static BOOL command_completed(id<MTLCommandBuffer> command, const char *name, const char *op) {
    [command commit];
    [command waitUntilCompleted];
    BOOL complete = command.status == MTLCommandBufferStatusCompleted;
    printf("%s_%s_%s status=%lu error=%s\n", referenceMode ? "REF" : "NOCOPY", name, op,
           (unsigned long)command.status, command.error ? command.error.description.UTF8String : "none");
    return complete;
}

static void run_gpu_policy(id<MTLDevice> device, id<MTLBuffer> buffer, const void *memory,
                           NSUInteger len, const char *name) {
    const char *writeOptIn = getenv("RDNA4_TEST_RO_GPU_WRITE");
    BOOL allowWrites = !referenceMode || (writeOptIn && !strcmp(writeOptIn, "1"));
    if (referenceMode && !referenceChild) {
        for (unsigned op = 0; op < 3; op++) {
            if (op > 0 && !allowWrites) {
                printf("REF_%s_%u skipped=RO GPU write gated (set RDNA4_TEST_RO_GPU_WRITE=1)\n", name, op);
                continue;
            }
            char number[2] = { (char)('0' + op), '\0' };
            char *args[] = { (char *)executablePath, "--reference-child", (char *)name, number, NULL };
            pid_t child;
            int error = posix_spawnp(&child, executablePath, NULL, NULL, args, environ);
            if (error) { printf("REF_%s_%u spawn-error=%s\n", name, op, strerror(error)); continue; }
            int status = 0;
            pid_t waited;
            do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
            if (waited < 0) printf("REF_%s_%u wait-error=%s\n", name, op, strerror(errno));
            else if (WIFSIGNALED(status)) printf("REF_%s_%u signal=%d\n", name, op, WTERMSIG(status));
            else printf("REF_%s_%u exit=%d\n", name, op, WEXITSTATUS(status));
        }
        return;
    }
    id<MTLCommandQueue> queue = [device newCommandQueue];
    id<MTLBuffer> normal = [device newBufferWithLength:len options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> probe = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [probe blitCommandEncoder];
    if (!normal || !normal.contents || !blit) {
        printf("%s_SKIP case=%s reason=GPU commands unavailable\n", referenceMode ? "REF" : "NOCOPY", name);
        return;
    }
    [blit fillBuffer:normal range:NSMakeRange(0, len) value:0];
    [blit endEncoding];
    if (!command_completed(probe, name, "PROBE")) {
        printf("%s_SKIP case=%s reason=device cannot run commands\n", referenceMode ? "REF" : "NOCOPY", name);
        return;
    }
    for (unsigned op = 0; op < 3; op++) {
        if (referenceChild && (int)op != referenceOperation) continue;
        if (op > 0 && !allowWrites) {
            printf("REF_%s_%u skipped=RO GPU write gated (set RDNA4_TEST_RO_GPU_WRITE=1)\n", name, op);
            continue;
        }
        const char *operation = op == 0 ? "READ" : op == 1 ? "COPY_INTO" : "FILL";
        @try {
            id<MTLCommandBuffer> command = [queue commandBuffer];
            id<MTLBlitCommandEncoder> encoder = [command blitCommandEncoder];
            if (!encoder) {
                printf("%s_%s_%s unavailable\n", referenceMode ? "REF" : "NOCOPY", name, operation);
                if (!referenceMode) failures++;
                continue;
            }
            if (op == 0) [encoder copyFromBuffer:buffer sourceOffset:0 toBuffer:normal destinationOffset:0 size:len];
            else if (op == 1) {
                memset(normal.contents, 0xa5, len);
                [encoder copyFromBuffer:normal sourceOffset:0 toBuffer:buffer destinationOffset:0 size:len];
            } else [encoder fillBuffer:buffer range:NSMakeRange(0, len) value:0x5a];
            [encoder endEncoding];
            BOOL complete = command_completed(command, name, operation);
            const uint8_t *bytes = op == 0 ? normal.contents : memory;
            BOOL matches = bytes != NULL;
            for (NSUInteger i = 0; matches && i < len; i++)
                if (bytes[i] != (uint8_t)(i * 37u + 11u)) matches = NO;
            printf("%s_%s_%s pattern=%s\n", referenceMode ? "REF" : "NOCOPY", name, operation, matches ? "unchanged" : "changed");
            if (!referenceMode && (op == 0 ? !complete : command.status != MTLCommandBufferStatusError)) failures++;
            if (!referenceMode && !matches) failures++;
        } @catch (NSException *exception) {
            printf("%s_%s_%s exception=%s\n", referenceMode ? "REF" : "NOCOPY", name, operation, exception.reason.UTF8String);
            if (!referenceMode) failures++;
        }
    }
}

static void run_render_target_policy(id<MTLDevice> device, id<MTLBuffer> buffer, const uint8_t *memory,
                                     NSUInteger len, const char *name) {
    const NSUInteger width = 128, height = 128, bytesPerRow = width * 4;
    if (len < bytesPerRow * height) {
        fprintf(stderr, "NOCOPY_FAIL %s render-target backing is too small\n", name); failures++; return;
    }
    MTLTextureDescriptor *textureDescriptor = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:width height:height mipmapped:NO];
    textureDescriptor.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> texture = [buffer newTextureWithDescriptor:textureDescriptor offset:0 bytesPerRow:bytesPerRow];
    if (!texture) {
        fprintf(stderr, "NOCOPY_FAIL %s read-only render-target texture creation failed\n", name); failures++; return;
    }
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(1.0, 0.0, 1.0, 1.0);
    id<MTLCommandQueue> queue = [device newCommandQueue];
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
    if (!encoder) {
        fprintf(stderr, "NOCOPY_FAIL %s read-only render encoder creation failed\n", name); failures++; return;
    }
    [encoder endEncoding];
    (void)command_completed(command, name, "RENDER_TARGET_CLEAR");
    BOOL unchanged = pattern_matches(memory, bytesPerRow * height, 0);
    printf("NOCOPY_%s_RENDER_TARGET status=%lu contents=%s\n", name,
           (unsigned long)command.status, unchanged ? "unchanged" : "changed");
    if (command.status != MTLCommandBufferStatusError || !unchanged) failures++;
}

static void run_case(id<MTLDevice> device, void *p, NSUInteger len, const char *name, BOOL skipNil) {
    __block BOOL deallocatorCalled = NO;
    void (^deallocator)(void *, NSUInteger) = ^(void *pointer, NSUInteger size) {
        (void)pointer; (void)size; deallocatorCalled = YES;
    };
    BOOL readonly = !strcmp(name, "RO") || !strcmp(name, "FILE_RO");
    // Check the required flag diagnostic without invoking private buffer selectors.
    FILE *diagnostic = !referenceMode && readonly ? tmpfile() : NULL;
    int savedStderr = -1;
    if (diagnostic) {
        fflush(stderr);
        savedStderr = dup(STDERR_FILENO);
        if (savedStderr < 0 || dup2(fileno(diagnostic), STDERR_FILENO) < 0) {
            if (savedStderr >= 0) close(savedStderr);
            savedStderr = -1; fclose(diagnostic); diagnostic = NULL;
        }
    }
    id<MTLBuffer> buffer = [device newBufferWithBytesNoCopy:p length:len options:MTLResourceStorageModeShared
        deallocator:deallocator];
    printf("%s_%s result=%s error=none-public-selector (see driver refusal log)\n", referenceMode ? "REF" : "NOCOPY", name, buffer ? "buffer" : "nil");
    if (diagnostic) {
        fflush(stderr);
        if (dup2(savedStderr, STDERR_FILENO) < 0) { perror("restore stderr"); failures++; }
        close(savedStderr);
        rewind(diagnostic);
        char line[4096], prefix[80]; BOOL flag = NO;
        (void)snprintf(prefix, sizeof prefix, "RDNA4Buffer %p:", (__bridge void *)buffer);
        while (fgets(line, sizeof line, diagnostic)) {
            fputs(line, stderr);
            if (strstr(line, prefix) && strstr(line, "imported READ-ONLY (GPU writes will be dropped)")) flag = YES;
        }
        fclose(diagnostic);
        if (buffer) {
            printf("NOCOPY_%s flag=%s\n", name, flag ? "READ-ONLY" : "missing");
            if (!flag) failures++;
        }
    } else if (readonly && !referenceMode) printf("NOCOPY_SKIP case=%s reason=cannot capture import flag diagnostic\n", name);
    if (!buffer) {
        if (readonly && !referenceMode && writableImportAvailable) {
            fprintf(stderr, "NOCOPY_FAIL %s read-only creation failed after a successful RW import\n", name); failures++;
        }
        if (deallocatorCalled) {
            fprintf(stderr, "NOCOPY_FAIL %s deallocator called for nil result\n", name); failures++;
        }
        if (skipNil) printf("NOCOPY_SKIP case=%s reason=no-copy import unavailable (see driver refusal log)\n", name);
        return;
    }
    if (buffer.contents != p) {
        fprintf(stderr, "NOCOPY_FAIL %s contents %p != input %p\n", name, buffer.contents, p); failures++;
    } else if (!pattern_matches((const uint8_t *)buffer.contents, len, 0)) {
        fprintf(stderr, "NOCOPY_FAIL %s pattern mismatch\n", name); failures++;
    }
    if (!strcmp(name, "RW")) writableImportAvailable = YES;
    if (readonly) {
        buffer.label = [NSString stringWithFormat:@"%s policy test", name];
        run_gpu_policy(device, buffer, p, len, name);
        if (!referenceMode) run_render_target_policy(device, buffer, p, len, name);
    }
    buffer = nil;
}

static void run_range_gpu(id<MTLDevice> device, id<MTLBuffer> buffer, NSUInteger len,
                          size_t patternOffset, const char *name) {
    @autoreleasepool {
        id<MTLCommandQueue> queue = [device newCommandQueue];
        id<MTLBuffer> normal = [device newBufferWithLength:len options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> probe = [queue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [probe blitCommandEncoder];
        if (!normal || !blit) {
            printf("NOCOPY_SKIP case=%s reason=GPU commands unavailable\n", name); return;
        }
        [blit fillBuffer:normal range:NSMakeRange(0, len) value:0];
        [blit endEncoding];
        if (!command_completed(probe, name, "RANGE_PROBE")) {
            printf("NOCOPY_SKIP case=%s reason=device cannot run commands\n", name); return;
        }
        @try {
            id<MTLCommandBuffer> command = [queue commandBuffer];
            id<MTLBlitCommandEncoder> encoder = [command blitCommandEncoder];
            if (!encoder) {
                fprintf(stderr, "NOCOPY_FAIL %s blit unavailable after successful probe\n", name); failures++; return;
            }
            NSUInteger copyLength = len & ~(NSUInteger)3;
            [encoder copyFromBuffer:buffer sourceOffset:0 toBuffer:normal destinationOffset:0 size:copyLength];
            [encoder endEncoding];
            BOOL complete = command_completed(command, name, "RANGE_READ");
            BOOL matches = normal.contents && pattern_matches(normal.contents, copyLength, patternOffset);
            printf("%s_%s_GPU bytes=%lu pattern=%s\n", referenceMode ? "REF" : "NOCOPY", name,
                   (unsigned long)copyLength, matches ? "match" : "mismatch");
            if (!complete || !matches) { fprintf(stderr, "NOCOPY_FAIL %s aligned-length GPU blit\n", name); failures++; }
        } @catch (NSException *exception) {
            fprintf(stderr, "NOCOPY_FAIL %s blit exception: %s\n", name, exception.reason.UTF8String); failures++;
        }
    }
}

static void run_range_case(id<MTLDevice> device, void *p, NSUInteger len, size_t patternOffset, const char *name) {
    __block unsigned deallocatorCalls = 0;
    __block void *releasedPointer = NULL;
    __block NSUInteger releasedLength = 0;
    @autoreleasepool {
        __attribute__((objc_precise_lifetime)) id<MTLBuffer> buffer = [device newBufferWithBytesNoCopy:p length:len options:MTLResourceStorageModeShared
            deallocator:^(void *pointer, NSUInteger size) {
                deallocatorCalls++; releasedPointer = pointer; releasedLength = size;
            }];
        printf("%s_%s result=%s contents=%s length=%lu\n", referenceMode ? "REF" : "NOCOPY", name,
               buffer ? "buffer" : "nil", buffer && buffer.contents == p ? "same-pointer" : "different-pointer",
               (unsigned long)buffer.length);
        if (!buffer) {
            if (!referenceMode && !writableImportAvailable) {
                printf("NOCOPY_SKIP case=%s reason=no-copy import unavailable (see driver refusal log)\n", name);
                if (deallocatorCalls) {
                    fprintf(stderr, "NOCOPY_FAIL %s deallocator called for nil result\n", name); failures++;
                }
                return;
            }
            fprintf(stderr, "NOCOPY_FAIL %s input must be accepted (deallocator calls=%u)\n", name, deallocatorCalls);
            failures++; return;
        }
        if (buffer.contents != p || buffer.length != len || !pattern_matches(buffer.contents, len, patternOffset)) {
            fprintf(stderr, "NOCOPY_FAIL %s contents/length/pattern invariant\n", name); failures++;
        }
        run_range_gpu(device, buffer, len, patternOffset, name);
        if (deallocatorCalls) { fprintf(stderr, "NOCOPY_FAIL %s deallocator called before release\n", name); failures++; }
        buffer = nil;
    }
    if (deallocatorCalls != 1 || releasedPointer != p || releasedLength != len) {
        fprintf(stderr, "NOCOPY_FAIL %s deallocator calls=%u pointer=%p length=%lu (expected %p, %lu)\n",
                name, deallocatorCalls, releasedPointer, (unsigned long)releasedLength, p, (unsigned long)len); failures++;
    } else printf("%s_%s_DEALLOC calls=1 original-pointer=yes original-length=yes\n", referenceMode ? "REF" : "NOCOPY", name);
}

static void run_anonymous_cases(id<MTLDevice> device, size_t page) {
    const size_t len = 65536;
    void *memory = NULL;
    // Include the extra page covered by the enclosing import of (memory + 1, len).
    if (posix_memalign(&memory, page, len + page) != 0 || !memory) {
        printf("NOCOPY_SKIP case=anonymous reason=aligned allocation failed\n"); return;
    }
    fill_pattern(memory, len + page);
    @autoreleasepool { run_case(device, memory, len, "RW", YES); }
    if (mprotect(memory, len, PROT_READ) != 0) {
        printf("NOCOPY_SKIP case=anonymous-RO reason=mprotect failed: %s\n", strerror(errno));
    } else {
        printf("%s_RO expected=readable-import-write-commands-dropped\n", referenceMode ? "REF" : "NOCOPY");
        @autoreleasepool { run_case(device, memory, len, "RO", YES); }
    }
    (void)mprotect(memory, len, PROT_READ | PROT_WRITE);

    @autoreleasepool { run_range_case(device, (uint8_t *)memory + 1, len, 1, "UNALIGNED"); }
    @autoreleasepool { run_range_case(device, memory, len - 1, 0, "NONMULTIPLE"); }
    free(memory);
}

static void run_file_shared_readonly(id<MTLDevice> device) {
    NSString *temporaryPath = [NSTemporaryDirectory() stringByAppendingPathComponent:@"garnet-nocopy-XXXXXX"];
    char *path = strdup(temporaryPath.fileSystemRepresentation);
    if (!path) { printf("NOCOPY_SKIP case=FILE_RO reason=temporary path allocation failed\n"); return; }
    int fd = mkstemp(path);
    if (fd >= 0) (void)unlink(path);
    free(path);
    if (fd < 0) { printf("NOCOPY_SKIP case=FILE_RO reason=mkstemp failed: %s\n", strerror(errno)); return; }
    const size_t len = 65536;
    if (ftruncate(fd, (off_t)len) != 0) {
        printf("NOCOPY_SKIP case=FILE_RO reason=ftruncate failed: %s\n", strerror(errno)); close(fd); return;
    }
    void *writer = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (writer == MAP_FAILED) {
        printf("NOCOPY_SKIP case=FILE_RO reason=writable mmap failed: %s\n", strerror(errno)); close(fd); return;
    }
    fill_pattern(writer, len);
    if (msync(writer, len, MS_SYNC) != 0) fprintf(stderr, "NOCOPY_WARN msync: %s\n", strerror(errno));
    (void)munmap(writer, len);
    void *reader = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, 0);
    if (reader == MAP_FAILED) {
        printf("NOCOPY_SKIP case=FILE_RO reason=read-only mmap failed: %s\n", strerror(errno)); close(fd); return;
    }
    @autoreleasepool { run_case(device, reader, len, "FILE_RO", YES); }
    (void)munmap(reader, len);
    close(fd);
}

static int run_all(id<MTLDevice> device) {
    if (!device) { printf("NOCOPY_REF_SKIP no system default device\n"); return 0; }
    const long pageSize = sysconf(_SC_PAGESIZE);
    if (pageSize <= 0 || ((size_t)pageSize & ((size_t)pageSize - 1)) != 0) {
        printf("NOCOPY_SKIP invalid page size %ld\n", pageSize); return 0;
    }
    const char *deviceName = device.name.UTF8String;
    printf("NOCOPY_BEGIN device=%s page=%ld\n", deviceName ? deviceName : "?", pageSize);
    run_anonymous_cases(device, (size_t)pageSize);
    run_file_shared_readonly(device);
    if (!failures && writableImportAvailable) printf("NOCOPY_PASS failures=0\n");
    else if (!failures) printf("NOCOPY_SKIP reason=no successful writable import; runtime policy unverified\n");
    return failures ? 1 : 0;
}

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        setvbuf(stdout, NULL, _IONBF, 0);
        alarm(90);
        executablePath = argv[0];
        if (argc == 4 && !strcmp(argv[1], "--reference-child")) {
            alarm(10);
            referenceMode = YES; referenceChild = YES; referenceOperation = atoi(argv[3]);
            id<MTLDevice> device = MTLCreateSystemDefaultDevice();
            if (!device) { puts("REF_SKIP no system default device"); return 0; }
            if (!strcmp(argv[2], "FILE_RO")) run_file_shared_readonly(device);
            else {
                const size_t len = 65536;
                void *memory = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
                if (memory == MAP_FAILED) { puts("REF_SKIP mmap failed"); return 0; }
                fill_pattern(memory, len);
                if (mprotect(memory, len, PROT_READ) == 0) run_case(device, memory, len, "RO", YES);
                else puts("REF_SKIP mprotect failed");
                (void)munmap(memory, len);
            }
            return 0;
        }
        if (argc == 2 && strcmp(argv[1], "--reference") == 0) {
            referenceMode = YES;
            return run_all(MTLCreateSystemDefaultDevice());
        }
        if (argc < 2 || argc > 3) {
            fprintf(stderr, "Usage: test-nocopy-readonly <GarnetMetal.bundle> [DEVICE_CLASS] | --reference\n"); return 2;
        }
        setenv("RDNA4M_ALLOW", "1", 1);
        NSError *error = nil;
        NSBundle *bundle = [NSBundle bundleWithPath:@(argv[1])];
        if (![bundle loadAndReturnError:&error]) {
            fprintf(stderr, "NOCOPY_SKIP bundle load: %s\n", error.localizedDescription.UTF8String); return 0;
        }
        const char *className = argc == 3 ? argv[2] : "GarnetDevice";
        Class deviceClass = objc_getClass(className);
        if (!deviceClass) { fprintf(stderr, "NOCOPY_SKIP class missing: %s\n", className); return 0; }
        id<MTLDevice> device = [[deviceClass alloc] init];
        if (!device) { fprintf(stderr, "NOCOPY_SKIP device init: %s\n", className); return 0; }
        return run_all(device);
    }
}
