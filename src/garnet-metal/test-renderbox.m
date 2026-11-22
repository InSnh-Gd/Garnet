// Exercise RenderBox's optional binary archive, stitched shader reflection,
// render completion and pixels through the real custom-effect caller.
// VM: root + RDNA4M_ALLOW=1 RDNA4M_TEST_INPROC=1, using the VM backend bundle.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#import <objc/message.h>
#import <dlfcn.h>
#include <stdatomic.h>
#include <unistd.h>

static int failures;
static _Atomic unsigned archiveCalls;
typedef id<MTLBinaryArchive> (*ArchiveFn)(id, SEL, MTLBinaryArchiveDescriptor *, NSError **) __attribute__((ns_returns_retained));
static ArchiveFn originalArchive;
static id<MTLBinaryArchive> observeArchive(id self, SEL selector, MTLBinaryArchiveDescriptor *descriptor, NSError **error) NS_RETURNS_RETAINED;
static id<MTLBinaryArchive> observeArchive(id self, SEL selector, MTLBinaryArchiveDescriptor *descriptor, NSError **error) {
    atomic_fetch_add(&archiveCalls, 1);
    return originalArchive(self, selector, descriptor, error);
}

#define CHECK(cond, fmt, ...) do { \
    if (cond) printf("PASS " fmt "\n", ##__VA_ARGS__); \
    else { printf("FAIL " fmt "\n", ##__VA_ARGS__); failures++; } } while (0)

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        if (argc == 2 && !strcmp(argv[1], "--help")) { puts("Usage: test-renderbox BUNDLE|--reference"); return 0; }
        if (argc != 2) { puts("Usage: test-renderbox BUNDLE|--reference"); return 2; }
        alarm(60);
        setvbuf(stdout, NULL, _IONBF, 0);

        NSError *error = nil;
        BOOL reference = !strcmp(argv[1], "--reference");
        id<MTLDevice> device = nil;
        if (reference) device = MTLCreateSystemDefaultDevice();
        else {
            NSBundle *bundle = [NSBundle bundleWithPath:@(argv[1])];
            if (![bundle loadAndReturnError:&error]) {
                printf("FAIL bundle load %s\n", error.description.UTF8String); return 1;
            }
            device = [[objc_getClass("GarnetDevice") alloc] init];
            ((void(*)(id,SEL))objc_msgSend)(device, sel_getUid("initLimits"));
        }
        if (!device) { puts("FAIL Metal device unavailable"); return 1; }
        printf("INFO device %s\n", device.name.UTF8String);

        dlopen("/System/Library/PrivateFrameworks/RenderBox.framework/RenderBox", RTLD_NOW);
        Class RBDevice = objc_getClass("RBDevice");
        Class RBDrawable = objc_getClass("RBDrawable");
        Class RBDisplayList = objc_getClass("RBDisplayList");
        Class RBShape = objc_getClass("RBShape");
        Class RBFill = objc_getClass("RBFill");
        Class RBShader = objc_getClass("RBShader");
        Class RBShaderLibrary = objc_getClass("RBShaderLibrary");
        if (!RBDevice || !RBDrawable || !RBDisplayList || !RBShape || !RBFill || !RBShader || !RBShaderLibrary) {
            puts("FAIL RenderBox classes missing"); return 1;
        }
        Class native = object_getClass(device);
        SEL archiveSelector = @selector(newBinaryArchiveWithDescriptor:error:);
        Method archiveMethod = class_getInstanceMethod(native, archiveSelector);
        if (!reference) {
            originalArchive = (ArchiveFn)method_getImplementation(archiveMethod);
            if (!class_addMethod(native, archiveSelector, (IMP)observeArchive, method_getTypeEncoding(archiveMethod)))
                method_setImplementation(archiveMethod, (IMP)observeArchive);
        }

        MTLBinaryArchiveDescriptor *desc = [MTLBinaryArchiveDescriptor new];
        desc.url = [NSURL fileURLWithPath:@"/System/Library/PrivateFrameworks/RenderBox.framework/Versions/A/Resources/archive.metallib"];
        if (![[NSFileManager defaultManager] fileExistsAtPath:desc.url.path]) { puts("FAIL no archive.metallib"); return 1; }
        error = nil;
        if (!reference) {
            id<MTLBinaryArchive> archive = [device newBinaryArchiveWithDescriptor:desc error:&error];
            CHECK(!archive && [error.domain isEqualToString:MTLBinaryArchiveDomain] && error.code != MTLBinaryArchiveErrorNone,
                  "native archive for RenderBox archive.metallib declined with MTLBinaryArchiveDomain (%s)",
                  error.description.UTF8String);
        }
        if (failures) _exit(1);
        unsigned directCalls = atomic_load(&archiveCalls);

        // The caller: an RBDevice bound to the injected RDNA4 device.
        id rbDevice = ((id(*)(id,SEL,id))objc_msgSend)([RBDevice alloc], sel_getUid("initWithDevice:"), device);
        CHECK(rbDevice != nil, "RBDevice initWithDevice: accepts the selected device");
        ((void(*)(id,SEL,id))objc_msgSend)(RBDevice, sel_getUid("setCurrentDevice:"), rbDevice);

        // RBDrawable render target, 64x64.
        id drawable = ((id(*)(id,SEL,id))objc_msgSend)([RBDrawable alloc], sel_getUid("initWithDevice:"), rbDevice);
        CGSize sz = {64,64};
        ((void(*)(id,SEL,CGSize))objc_msgSend)(drawable, sel_getUid("setSize:"), sz);
        ((void(*)(id,SEL,unsigned long))objc_msgSend)(drawable, sel_getUid("setPixelFormat:"), MTLPixelFormatBGRA8Unorm);
        CHECK(drawable != nil, "RBDrawable created on RBDevice");
        MTLTextureDescriptor *targetDescriptor = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:64 height:64 mipmapped:NO];
        targetDescriptor.storageMode = MTLStorageModeShared;
        targetDescriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        id<MTLTexture> target = [device newTextureWithDescriptor:targetDescriptor];
        if (!target) { puts("FAIL render target allocation"); _exit(1); }
        uint32_t zeros[64*64] = {0};
        [target replaceRegion:MTLRegionMake2D(0,0,64,64) mipmapLevel:0 withBytes:zeros bytesPerRow:64*4];
        ((void(*)(id,SEL,id))objc_msgSend)(drawable, sel_getUid("setTexture:"), target);
        dispatch_semaphore_t completed = dispatch_semaphore_create(0);
        void (^handler)(void) = ^{ dispatch_semaphore_signal(completed); };
        ((void(*)(id,SEL,id))objc_msgSend)(drawable, sel_getUid("setCompletedHandler:"), handler);

        // A SwiftUI color effect forces RenderBox to link a custom function.
        // A unique function prevents Apple's shared stitched-library cache from hiding a broken cold path.
        NSString *functionName = [@"rdna4Tint_" stringByAppendingString:
            [NSUUID.UUID.UUIDString stringByReplacingOccurrencesOfString:@"-" withString:@""]];
        NSString *src = [NSString stringWithFormat:@"#include <metal_stdlib>\nusing namespace metal;\n[[stitchable]] half4 %@(float2 p, half4 c) { (void)p; return half4(c.r, 0.0h, 0.0h, c.a); }", functionName];
        id lib = ((id(*)(id,SEL,id))objc_msgSend)(RBShaderLibrary, sel_getUid("libraryWithSource:"), src);
        CHECK(lib != nil, "RBShaderLibrary libraryWithSource:");
        ((void(*)(id,SEL,id))objc_msgSend)(lib, sel_getUid("setBinaryArchiveURL:"), desc.url);

        // Find a function selector on the library/shader path.
        // RBShader -initWithLibrary:function: where function is the function name string.
        id shader = nil;
        SEL initShader = sel_getUid("initWithLibrary:function:");
        shader = ((id(*)(id,SEL,id,id))objc_msgSend)([RBShader alloc], initShader, lib, functionName);
        if (shader) ((void(*)(id,SEL,int))objc_msgSend)(shader, sel_getUid("setType:"), 1);
        CHECK(shader != nil, "RBShader initWithLibrary:function: rdna4Tint");

        error = nil;
        BOOL compiled = ((BOOL(*)(id,SEL,id,NSError **))objc_msgSend)(rbDevice, sel_getUid("compileShader:error:"), shader, &error);
        CHECK(compiled, "RBDevice compileShader: completed without Metal assertion (error=%s)",
              error ? error.description.UTF8String : "nil");
        if (!compiled) _exit(1);

        // Filters apply to subsequent draws in a display list.
        id dlist = [[RBDisplayList alloc] init];
        ((void(*)(id,SEL,double))objc_msgSend)(dlist, sel_getUid("setDeviceScale:"), 1.0);
        CGRect content = CGRectMake(0,0,64,64);
        ((void(*)(id,SEL,CGRect))objc_msgSend)(dlist, sel_getUid("setContentRect:"), content);
        ((void(*)(id,SEL,BOOL))objc_msgSend)(dlist, sel_getUid("setLinearColors:"), YES);

        id shape = [[RBShape alloc] init];
        ((void(*)(id,SEL,CGRect))objc_msgSend)(shape, sel_getUid("setRect:"), CGRectMake(8,8,48,48));
        id fill = [[RBFill alloc] init];
        typedef struct { float r,g,b,a; } RDNA4Color;
        RDNA4Color col = {0.2f,0.8f,0.4f,1.0f};
        ((void(*)(id,SEL,RDNA4Color))objc_msgSend)(fill, sel_getUid("setColor:"), col);

        SEL drawShape = sel_getUid("drawShape:fill:alpha:blendMode:");
        if (shader) {
            SEL filt = sel_getUid("addFilterWithShader:border:bounds:flags:");
            CGSize border = CGSizeMake(0,0);
            // Bit 0 applies the color effect to subsequent draws (0 filters an empty layer).
            ((void(*)(id,SEL,id,CGSize,const CGRect *,unsigned))objc_msgSend)(
                dlist, filt, shader, border, &content, 1u);
        }
        ((void(*)(id,SEL,id,id,float,int))objc_msgSend)(dlist, drawShape, shape, fill, 1.0f, 0);

        // Render - this is where the original crash fired (FunctionLibrary ctor).
        ((void(*)(id,SEL,id,unsigned))objc_msgSend)(drawable, sel_getUid("renderDisplayList:flags:"), dlist, 0u);
        BOOL finished = dispatch_semaphore_wait(completed, dispatch_time(DISPATCH_TIME_NOW, 20 * NSEC_PER_SEC)) == 0;
        CHECK(finished, "RenderBox completion callback arrived");
        if (!finished) _exit(1);
        if (!reference) CHECK(atomic_load(&archiveCalls) > directCalls,
            "RenderBox requested its optional native archive (%u calls)", atomic_load(&archiveCalls) - directCalls);

        id<MTLTexture> tex = ((id(*)(id,SEL))objc_msgSend)(drawable, sel_getUid("texture"));
        CHECK(tex != nil, "RBDrawable produced a texture after renderDisplayList:");
        if (tex) {
            NSUInteger w = tex.width, h = tex.height;
            NSUInteger bytesPerRow = w * 4;
            NSMutableData *px = [NSMutableData dataWithLength:bytesPerRow * h];
            [tex getBytes:px.mutableBytes bytesPerRow:bytesPerRow
               fromRegion:MTLRegionMake2D(0,0,w,h) mipmapLevel:0];
            const uint8_t *b = px.bytes;
            NSUInteger center = 32 * bytesPerRow + 32 * 4;
            CHECK(w == 64 && h == 64 && b[center] <= 1 && b[center+1] <= 1 &&
                b[center+2] >= 122 && b[center+2] <= 126 && b[center+3] == 255 && b[3] == 0,
                "custom-effect pixels: center BGRA=%u,%u,%u,%u corner alpha=%u", b[center], b[center+1], b[center+2], b[center+3], b[3]);
        }
        printf(failures ? "RENDERBOX_FAIL %d\n" : "RENDERBOX_PASS\n", failures);
        _exit(failures ? 1 : 0);
    }
}
