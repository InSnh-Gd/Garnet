// Verify the real Metal function metadata and stitched library compiler.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include <spawn.h>
#include <sys/wait.h>
#include <errno.h>
#include <stdlib.h>

extern char **environ;

@interface NSObject (RDNA4StitchingReflectionTest)
- (void)reflectionWithFunction:(id)function options:(NSUInteger)options sync:(BOOL)sync
                 binaryArchives:(id)archives
              completionHandler:(void (^)(NSUInteger, dispatch_data_t, NSString *))completion;
- (void)reflectionWithFunction:(id)function options:(NSUInteger)options sync:(BOOL)sync
              completionHandler:(void (^)(NSUInteger, dispatch_data_t, NSString *))completion;
- (void)reflectionWithFunction:(id)function options:(NSUInteger)options sync:(BOOL)sync
                 pipelineLibrary:(id)library
              completionHandler:(void (^)(NSUInteger, dispatch_data_t, NSString *))completion;
@end

@interface RDNA4MalformedReflectionFunction : NSObject
- (NSData *)reflectionData;
@end
@implementation RDNA4MalformedReflectionFunction
- (NSData *)reflectionData {
    const uint8_t bytes[16] = {0, 0, 0, 0, 'A', 'I', 'R', 'R'};
    return [NSData dataWithBytes:bytes length:sizeof bytes];
}
@end

@interface RDNA4MalformedVisibleReflectionFunction : RDNA4MalformedReflectionFunction
@end
@implementation RDNA4MalformedVisibleReflectionFunction
- (MTLFunctionType)functionType { return MTLFunctionTypeVisible; }
@end

static void fail(NSString *message) __attribute__((noreturn));
static void fail(NSString *message) {
    fprintf(stderr, "STITCHING_FAIL %s\n", message.UTF8String);
    _exit(1);
}

static void checkContainer(NSData *data);

static NSData *reflectionReply(id compiler, id function, NSUInteger options,
                               NSUInteger *status, NSString **message) {
    __block NSUInteger code = NSUIntegerMax;
    __block dispatch_data_t reply = nil;
    __block NSString *failure = nil;
    dispatch_semaphore_t completed = dispatch_semaphore_create(0);
    [compiler reflectionWithFunction:function options:options sync:YES binaryArchives:nil
        completionHandler:^(NSUInteger result, dispatch_data_t data, NSString *error) {
            code = result; reply = data; failure = error;
            dispatch_semaphore_signal(completed);
        }];
    if (dispatch_semaphore_wait(completed, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)))
        fail(@"reflection compiler callback deadline");
    if (status) *status = code;
    if (message) *message = failure;
    if (!reply) fail(@"reflection compiler returned a null dispatch object");
    const void *bytes = NULL;
    size_t length = 0;
    dispatch_data_t mapped = dispatch_data_create_map(reply, &bytes, &length);
    if (!mapped || (length && !bytes)) fail(@"reflection reply mapping failed");
    NSData *result = [NSData dataWithBytes:bytes length:length];
    if (code) checkContainer(result);
    return result;
}

static void checkContainer(NSData *data) {
    if (data.length < 80 + 25 || (data.length & 7)) fail(@"short or unaligned reflection container");
    const uint8_t *bytes = data.bytes;
    uint32_t magic, headerWord;
    uint64_t fields[9];
    memcpy(&magic, bytes, 4); memcpy(&headerWord, bytes + 4, 4);
    memcpy(fields, bytes + 8, sizeof fields);
    if (magic != 0xef13c710 || headerWord != 8 || fields[0] != 20230418 ||
        fields[1] != 80 || fields[2] < 36 || fields[2] > data.length - 80 ||
        ((80 + fields[2] + 7) & ~UINT64_C(7)) != data.length ||
        memcmp(bytes + 80, "MTLPSBIN", 8))
        fail(@"invalid stitching reflection container header");
    uint32_t trailerOffset;
    memcpy(&trailerOffset, bytes + 80 + 17, 4);
    if ((uint64_t)trailerOffset + 36 != fields[2]) fail(@"incorrect reflection section length");
    uint64_t trailerLength;
    memcpy(&trailerLength, bytes + 80 + trailerOffset + 28, 8);
    if (trailerLength != fields[2]) fail(@"incorrect reflection trailer length");
    for (unsigned i = 3; i < 9; i += 2)
        if (fields[i] != data.length || fields[i + 1] != 0)
            fail(@"unexpected nonempty reflection container section");
    for (size_t i = 80 + (size_t)fields[2]; i < data.length; ++i)
        if (bytes[i]) fail(@"nonzero reflection container padding");
}

static void checkLinkedAccessor(const char *executable, const char *bundle, const char *selector) {
    int ready[2];
    if (pipe(ready)) fail(@"linked metadata readiness pipe failed");
    char descriptor[32];
    snprintf(descriptor, sizeof descriptor, "%d", ready[1]);
    char *const arguments[] = { (char *)executable, (char *)bundle,
        (char *)selector, descriptor, NULL };
    pid_t child;
    int error = posix_spawnp(&child, executable, NULL, NULL, arguments, environ);
    close(ready[1]);
    if (error) { close(ready[0]); fail(@"linked metadata subprocess failed to start"); }
    char marker = 0;
    ssize_t count;
    do { count = read(ready[0], &marker, 1); } while (count < 0 && errno == EINTR);
    close(ready[0]);
    int status = 0;
    pid_t waited;
    do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    if (waited != child || count != 1 || marker != 'R') fail(@"linked accessor was not reached");
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
        printf("STITCHING_LINKED_ACCESSOR_PASS %s result=returned\n", selector);
    else fail([NSString stringWithFormat:@"linked accessor %s terminated with wait status %d", selector, status]);
}

static void captureLinkedReflection(id compiler, id linked, const char *path) {
    const NSUInteger masks[] = { 0, 0x20000000u };
    for (unsigned i = 0; i < sizeof masks / sizeof masks[0]; ++i) {
        __block NSUInteger status = NSUIntegerMax;
        __block dispatch_data_t reply = nil;
        __block NSString *message = nil;
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        [compiler reflectionWithFunction:linked options:masks[i] sync:YES binaryArchives:nil
            completionHandler:^(NSUInteger code, dispatch_data_t data, NSString *error) {
                status = code; reply = data; message = error;
                dispatch_semaphore_signal(done);
            }];
        if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)))
            fail(@"linked reflection capture deadline");
        const void *bytes = NULL; size_t length = 0;
        dispatch_data_t mapped = reply ? dispatch_data_create_map(reply, &bytes, &length) : nil;
        printf("STITCHING_LINKED_REPLY options=0x%lx status=%lu present=%d length=%zu error=%s\n",
            (unsigned long)masks[i], (unsigned long)status, reply != nil, length, message.UTF8String ?: "nil");
        if (path && mapped && (!length || bytes)) {
            NSString *output = [@(path) stringByAppendingFormat:@".%08lx.reply", (unsigned long)masks[i]];
            if (![[NSData dataWithBytes:bytes length:length] writeToFile:output atomically:YES])
                fail(@"linked reply capture failed");
        }
    }
}

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        setvbuf(stdout, NULL, _IONBF, 0);
        if (argc != 2 && argc != 4) return 2;
        const char *linkedSelector = argc == 4 ? argv[2] : NULL;
        if (linkedSelector && strcmp(linkedSelector, "returnType") && strcmp(linkedSelector, "arguments") &&
            strcmp(linkedSelector, "capture")) return 2;
        if (!strcmp(argv[1], "--help")) { puts("Usage: test-stitching BUNDLE|--reference [returnType|arguments|capture READY_FD]"); return 0; }
        alarm(45);
        BOOL reference = !strcmp(argv[1], "--reference");
        id<MTLDevice> device = nil;
        if (reference) device = MTLCreateSystemDefaultDevice();
        else {
            NSError *error = nil;
            if (![[NSBundle bundleWithPath:@(argv[1])] loadAndReturnError:&error]) fail(error.description);
            device = [[objc_getClass("GarnetDevice") alloc] init];
            ((void (*)(id, SEL))objc_msgSend)(device, sel_registerName("initLimits"));
        }
        if (!device) fail(@"Metal device unavailable");
        NSError *error = nil;
        NSString *source = @"#include <metal_stdlib>\nusing namespace metal;\n"
            "kernel void unconvertible(device float *o [[buffer(0)]]) { o[0] = 1; }\n"
            "[[visible]] float4 probe_visible(float2 p, float4 c) { return c + float4(p, 0, 0); }\n"
            "[[stitchable]] half4 rdna4Tint(float2 p, half4 c) { (void)p; return half4(c.r, 0.0h, 0.0h, c.a); }\n";
        id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
        id<MTLFunction> function = [library newFunctionWithName:@"rdna4Tint"];
        if (!function || error) fail([NSString stringWithFormat:@"source: %@", error]);

        id compiler = reference ? ((id (*)(id, SEL))objc_msgSend)(device, sel_registerName("compiler")) :
            [NSClassFromString(@"RDNA4ReflectionCompiler") new];
        if (!compiler) fail(@"reflection compiler unavailable");
        NSUInteger replyStatus = NSUIntegerMax;
        NSString *replyError = nil;
        id<MTLFunction> visible = [library newFunctionWithName:@"probe_visible"];
        if (!visible) fail(@"visible function unavailable");
        // Exercise Metal's metadata callback before making direct compiler calls.
        id visibleReturn = ((id (*)(id, SEL))objc_msgSend)(visible, sel_registerName("returnType"));
        NSArray *visibleArguments = ((id (*)(id, SEL))objc_msgSend)(visible, sel_registerName("arguments"));
        if (visibleReturn || !visibleArguments || visibleArguments.count)
            fail(@"ordinary visible function should expose nil returnType and empty arguments");
        NSData *visibleDefault = reflectionReply(compiler, visible, 0, &replyStatus, &replyError);
        if (replyStatus || visibleDefault.length != 144) fail(@"visible default reflection failed");
        checkContainer(visibleDefault);
        NSData *visibleStitching = reflectionReply(compiler, visible, 0x20000000u, &replyStatus, &replyError);
        if (replyStatus || ![visibleDefault isEqualToData:visibleStitching])
            fail(@"visible reflection replies differ by options");
        uint32_t visibleRecords;
        memcpy(&visibleRecords, (const uint8_t *)visibleDefault.bytes + 80 + 21, sizeof visibleRecords);
        if (visibleRecords) fail(@"ordinary visible reflection should have zero records");
        puts("STITCHING_VISIBLE_EMPTY_SIGNATURE_PASS");
        NSData *ordinary = reflectionReply(compiler, function, 0, &replyStatus, &replyError);
        if (replyStatus || !ordinary)
            fail([NSString stringWithFormat:@"visible reflection failed: %@", replyError]);
        checkContainer(ordinary);
        puts("STITCHING_REFLECTION_DEFAULT_CONTAINER_PASS");
        NSData *stitching = reflectionReply(compiler, function, 0x20000000u, &replyStatus, &replyError);
        if (replyStatus || !stitching)
            fail([NSString stringWithFormat:@"stitching reflection failed: %@", replyError]);
        checkContainer(stitching);
        if (![ordinary isEqualToData:stitching]) fail(@"visible reflection replies differ by options");
        puts("STITCHING_REFLECTION_CONTAINER_PASS");
        for (NSUInteger mask = 1; mask <= 3; ++mask) {
            NSData *visibleReply = reflectionReply(compiler, visible, mask, &replyStatus, &replyError);
            if (replyStatus || ![visibleReply isEqualToData:visibleDefault])
                fail(@"visible binding/type-info reflection differs from default container");
            NSData *stitchableReply = reflectionReply(compiler, function, mask, &replyStatus, &replyError);
            if (replyStatus || ![stitchableReply isEqualToData:ordinary])
                fail(@"stitchable binding/type-info reflection differs from default container");
        }
        puts("STITCHING_REFLECTION_OPTION_MASKS_PASS");

        if (!reference) {
            RDNA4MalformedReflectionFunction *malformed = [RDNA4MalformedReflectionFunction new];
            NSData *invalid = reflectionReply(compiler, malformed, 0x20000000u, &replyStatus, &replyError);
            if (!replyStatus || !replyError.length || ![invalid isEqualToData:visibleDefault])
                fail(@"failed stitching conversion did not return a zero-record error container");
            // Real compiler AIRR without a visible signature exercises unsupported
            // conversion independently of the malformed test double.
            id<MTLFunction> unconvertible = [library newFunctionWithName:@"unconvertible"];
            if (!unconvertible) fail(@"unconvertible kernel unavailable");
            NSData *rejected = reflectionReply(compiler, unconvertible, 0x20000000u, &replyStatus, &replyError);
            if (!replyStatus || !replyError.length || ![rejected isEqualToData:visibleDefault])
                fail(@"unconvertible kernel did not return a zero-record error container");
            puts("STITCHING_REFLECTION_CONVERSION_ERROR_PASS");
            for (NSUInteger mask = 0; mask <= 3; ++mask) {
                NSData *passthrough = reflectionReply(compiler, malformed, mask, &replyStatus, &replyError);
                if (replyStatus || replyError || ![passthrough isEqualToData:malformed.reflectionData])
                    fail(@"non-visible ordinary reflection was not passed through unchanged");
            }
            puts("STITCHING_REFLECTION_PASSTHROUGH_PASS");
            RDNA4MalformedVisibleReflectionFunction *unsupported = [RDNA4MalformedVisibleReflectionFunction new];
            const NSUInteger errorMasks[] = { 0, 1, 2, 3, 4, 0x20000000u };
            for (unsigned i = 0; i < sizeof errorMasks / sizeof errorMasks[0]; ++i) {
                NSData *fallback = reflectionReply(compiler, unsupported, errorMasks[i], &replyStatus, &replyError);
                if (!replyStatus || !replyError.length || ![fallback isEqualToData:visibleDefault])
                    fail(@"failed visible conversion did not return a zero-record error container");
            }
            puts("STITCHING_REFLECTION_OPTION_ERRORS_PASS");
            for (NSUInteger n = 0; n < 2; ++n) {
                NSData *missing = reflectionReply(compiler, n ? [NSObject new] : nil,
                    n ? 0x20000000u : 0, &replyStatus, &replyError);
                if (!replyStatus || ![missing isEqualToData:visibleDefault] || !replyError.length)
                    fail(@"missing reflection data did not return a valid zero-record error container");
            }
            [compiler reflectionWithFunction:visible options:0 sync:YES binaryArchives:nil completionHandler:nil];
            [compiler reflectionWithFunction:malformed options:0x20000000u sync:YES pipelineLibrary:nil completionHandler:nil];
            [compiler reflectionWithFunction:nil options:0 sync:YES completionHandler:nil];
            puts("STITCHING_REFLECTION_NULL_SAFETY_PASS");
        }

        id returnType = ((id (*)(id, SEL))objc_msgSend)(function, sel_registerName("returnType"));
        NSUInteger type = ((NSUInteger (*)(id, SEL))objc_msgSend)(returnType, sel_registerName("dataType"));
        NSArray<id<MTLBinding>> *arguments = ((id (*)(id, SEL))objc_msgSend)(function, sel_registerName("arguments"));
        if (type != MTLDataTypeHalf4 || arguments.count != 2 ||
            ![arguments[0].name isEqual:@"p"] || ![arguments[1].name isEqual:@"c"])
            fail([NSString stringWithFormat:@"signature return=%@ arguments=%@", returnType, arguments]);
        puts("STITCHING_SIGNATURE_PASS");
        MTLFunctionStitchingInputNode *p = [[MTLFunctionStitchingInputNode alloc] initWithArgumentIndex:0];
        MTLFunctionStitchingInputNode *c = [[MTLFunctionStitchingInputNode alloc] initWithArgumentIndex:1];
        MTLFunctionStitchingFunctionNode *call = [[MTLFunctionStitchingFunctionNode alloc]
            initWithName:@"rdna4Tint" arguments:@[p, c] controlDependencies:@[]];
        MTLFunctionStitchingGraph *graph = [[MTLFunctionStitchingGraph alloc]
            initWithFunctionName:@"rdna4LinkedTint" nodes:@[call] outputNode:call attributes:@[]];
        MTLStitchedLibraryDescriptor *descriptor = [MTLStitchedLibraryDescriptor new];
        descriptor.functions = @[function];
        descriptor.functionGraphs = @[graph];
        id<MTLLibrary> stitched = [device newLibraryWithStitchedDescriptor:descriptor error:&error];
        id<MTLFunction> linked = [stitched newFunctionWithName:@"rdna4LinkedTint"];
        if (!linked || error) fail([NSString stringWithFormat:@"stitch: %@", error]);
        id bitcode = [linked respondsToSelector:sel_registerName("bitcodeData")] ?
            ((id (*)(id, SEL))objc_msgSend)(linked, sel_registerName("bitcodeData")) : nil;
        if (![bitcode length]) fail(@"stitched function has no translatable AIR");
        printf("STITCHING_LIBRARY_PASS AIR=%lu\n", (unsigned long)[bitcode length]);
        if (linkedSelector) {
            const char *dump = getenv("RDNA4_STITCH_LINKED_AIRR");
            if (dump) {
                NSData *reflection = [linked respondsToSelector:sel_registerName("reflectionData")] ?
                    ((id (*)(id, SEL))objc_msgSend)(linked, sel_registerName("reflectionData")) : nil;
                if (![reflection isKindOfClass:NSData.class] || !reflection.length)
                    puts("STITCHING_LINKED_AIRR unavailable");
                else {
                    if (![reflection writeToFile:@(dump) atomically:YES]) fail(@"linked AIRR capture failed");
                    printf("STITCHING_LINKED_AIRR bytes=%lu path=%s\n", (unsigned long)reflection.length, dump);
                }
            }
            if (!strcmp(linkedSelector, "capture")) {
                captureLinkedReflection(compiler, linked, dump);
                _exit(0);
            }
            if (write(atoi(argv[3]), "R", 1) != 1) fail(@"linked metadata readiness signal failed");
            id metadata = ((id (*)(id, SEL))objc_msgSend)(linked, sel_registerName(linkedSelector));
            if (!strcmp(linkedSelector, "returnType")) {
                NSUInteger linkedType = ((NSUInteger (*)(id, SEL))objc_msgSend)(metadata, sel_registerName("dataType"));
                if (linkedType != MTLDataTypeHalf4) fail(@"linked returnType is not half4");
            } else {
                NSArray<id<MTLBinding>> *linkedArguments = metadata;
                if (!linkedArguments || linkedArguments.count != 2 ||
                    linkedArguments[0].name.length || linkedArguments[1].name.length)
                    fail(@"linked arguments should contain two unnamed inputs");
            }
            printf("STITCHING_LINKED_METADATA %s present=%d\n", linkedSelector, metadata != nil);
        } else if (!reference) {
            checkLinkedAccessor(argv[0], argv[1], "returnType");
            checkLinkedAccessor(argv[0], argv[1], "arguments");
        }
        _exit(0);
    }
}
