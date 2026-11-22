#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#import <objc/runtime.h>

int main(int argc, const char *argv[]) { @autoreleasepool {
    if (argc != 2) return 2;
    NSBundle *bundle = [NSBundle bundleWithPath:@(argv[1])];
    NSError *error = nil;
    if (![bundle loadAndReturnError:&error]) {
        fprintf(stderr, "FAIL bundle load: %s\n", error.localizedDescription.UTF8String);
        return 2;
    }
    Class deviceClass = objc_getClass("GarnetDevice");
    id<MTLDevice> device = [[deviceClass alloc] init];
    SEL selector = sel_registerName("iosurfaceTextureAlignmentBytes");
    if (!device || ![device respondsToSelector:selector]) {
        fprintf(stderr, "FAIL GarnetDevice has no IOSurface alignment query\n");
        return 1;
    }
    NSUInteger surface = ((NSUInteger (*)(id, SEL))objc_msgSend)(device, selector);
    NSUInteger buffer = [device minimumTextureBufferAlignmentForPixelFormat:MTLPixelFormatBGRA8Unorm];
    printf("IOSurface alignment=%lu buffer texture alignment=%lu\n", (unsigned long)surface, (unsigned long)buffer);
    return surface == 32 && buffer == 256 ? 0 : 1;
} }
