// macOS: xcrun clang -arch x86_64 -fobjc-arc -Wall -Wextra -Werror -framework Foundation native-bundle-check.m -o native-bundle-check
#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int fail(int code, NSString *message) {
    fprintf(stderr, "FAIL[%d]: %s\n", code, message.UTF8String);
    return code;
}

static int checkBundle(const char *argument) {
    NSString *path = [[NSString alloc] initWithUTF8String:argument];
    if (!path.length) return fail(2, @"bundle path is empty or is not valid UTF-8");
    if (!path.isAbsolutePath)
        path = [NSFileManager.defaultManager.currentDirectoryPath stringByAppendingPathComponent:path];
    path = path.stringByStandardizingPath.stringByResolvingSymlinksInPath;
    NSFileManager *fm = NSFileManager.defaultManager;
    BOOL directory = NO;
    if (![fm fileExistsAtPath:path isDirectory:&directory])
        return fail(3, [NSString stringWithFormat:@"bundle path does not exist: %@", path]);
    if (!directory) return fail(3, [NSString stringWithFormat:@"bundle path is not a directory: %@", path]);

    NSString *plistPath = [path stringByAppendingPathComponent:@"Contents/Info.plist"];
    NSError *error = nil;
    NSData *data = [NSData dataWithContentsOfFile:plistPath options:0 error:&error];
    if (!data) return fail(3, [NSString stringWithFormat:@"cannot read %@: %@", plistPath, error]);
    id info = [NSPropertyListSerialization propertyListWithData:data options:NSPropertyListImmutable
                                                      format:NULL error:&error];
    if (![info isKindOfClass:NSDictionary.class])
        return fail(3, [NSString stringWithFormat:@"invalid bundle plist %@ (expected dictionary): %@",
                       plistPath, error ?: @"wrong root type"]);
    NSDictionary *expected = @{@"CFBundleExecutable": @"GarnetMetal", @"CFBundlePackageType": @"BNDL",
                              @"CFBundleIdentifier": @"org.garnet.metal", @"NSPrincipalClass": @"GarnetDevice"};
    for (NSString *key in expected) {
        if (![info[key] isEqual:expected[key]])
            return fail(3, [NSString stringWithFormat:@"%@: %@ expected %@, got %@",
                           plistPath, key, expected[key], info[key] ?: @"<missing>"]);
    }
    NSBundle *bundle = [NSBundle bundleWithPath:path];
    NSString *executable = [path stringByAppendingPathComponent:@"Contents/MacOS/GarnetMetal"];
    if (!bundle || ![bundle.executablePath.stringByResolvingSymlinksInPath
                     isEqualToString:executable.stringByResolvingSymlinksInPath])
        return fail(3, [NSString stringWithFormat:@"NSBundle cannot resolve expected executable: %@", executable]);
    NSArray<NSString *> *files = @[executable,
        [path stringByAppendingPathComponent:@"Contents/Resources/libvulkan_radeon.dylib"],
        [path stringByAppendingPathComponent:@"Contents/Resources/libgarnetxlate.dylib"]];
    for (NSString *file in files) {
        NSDictionary *attributes = [fm attributesOfItemAtPath:file.stringByResolvingSymlinksInPath error:&error];
        if (![attributes[NSFileType] isEqual:NSFileTypeRegular] || ![fm isReadableFileAtPath:file])
            return fail(3, [NSString stringWithFormat:@"required readable regular file missing: %@", file]);
    }

    // A renamed MoltenVK also exports the ICD entry point and recurses through this Metal device.
    const char *radvSymbols[] = {"vk_icdGetInstanceProcAddr", "radv_darwin_set_service_class",
        "radv_darwin_get_argument_limits_v1", "radv_darwin_buffer_copy",
        "radv_darwin_buffer_fill", "radv_darwin_buffer_wait", NULL};
    const char *xlateSymbols[] = {"garnetx_abi_version", "garnetx_translate", "garnetx_free", "garnetx_translate_linked",
        "garnetx_translate_with_capabilities", "garnetx_translate_linked_with_capabilities",
        "garnetx_lower_argument_resources_with_capabilities", NULL};
    const char **symbols[] = {radvSymbols, xlateSymbols};
    // Keep both handles alive through NSBundle loading; only the pure ABI query is called.
    for (NSUInteger i = 0; i < 2; ++i) {
        NSString *file = files[i + 1];
        void *handle = dlopen(file.fileSystemRepresentation, RTLD_NOW | RTLD_LOCAL);
        if (!handle) return fail(4, [NSString stringWithFormat:@"dlopen(RTLD_NOW) %@: %s", file, dlerror()]);
        printf("PASS dlopen(RTLD_NOW): %s\n", file.fileSystemRepresentation);
        for (const char **name = symbols[i]; *name; ++name) {
            dlerror();
            void *symbol = dlsym(handle, *name);
            const char *detail = dlerror();
            if (detail || !symbol)
                return fail(4, [NSString stringWithFormat:@"dlsym %@ %s: %s", file, *name,
                               detail ?: "NULL symbol address"]);
            Dl_info origin = {0};
            if (!dladdr(symbol, &origin) || !origin.dli_fname ||
                ![[NSString stringWithUTF8String:origin.dli_fname].stringByResolvingSymlinksInPath
                  isEqualToString:file.stringByResolvingSymlinksInPath])
                return fail(4, [NSString stringWithFormat:@"symbol %s is not defined by %@", *name, file]);
            printf("PASS export: %s\n", *name);
            if (strcmp(*name, "garnetx_abi_version") == 0) {
                uint32_t abi = ((uint32_t (*)(void))symbol)();
                if (abi != 3) return fail(4, [NSString stringWithFormat:@"translator ABI expected 3, got %u", abi]);
                printf("PASS translator ABI: %u\n", abi);
            }
        }
    }
    error = nil;
    if (![bundle loadAndReturnError:&error])
        return fail(4, [NSString stringWithFormat:@"NSBundle load %@: %@", path, error]);
    printf("PASS NSBundle load: %s\n", path.fileSystemRepresentation);
    Class device = objc_lookUpClass("GarnetDevice");
    const char *image = device ? class_getImageName(device) : NULL;
    if (!image || ![[NSString stringWithUTF8String:image].stringByResolvingSymlinksInPath
                   isEqualToString:executable.stringByResolvingSymlinksInPath])
        return fail(5, @"GarnetDevice is missing or belongs to a different executable");
    Class superclass = class_getSuperclass(device);
    if (!superclass || strcmp(class_getName(superclass), "MTLIOAccelDevice") != 0)
        return fail(5, [NSString stringWithFormat:@"GarnetDevice superclass expected MTLIOAccelDevice, got %s",
                       superclass ? class_getName(superclass) : "<none>"]);
    printf("PASS class: GarnetDevice : MTLIOAccelDevice (from bundle executable)\n");
    const char *protocols[] = {"MTLDevice", "MTLDeviceSPI"};
    for (NSUInteger i = 0; i < sizeof(protocols) / sizeof(protocols[0]); ++i) {
        Protocol *protocol = objc_getProtocol(protocols[i]);
        if (!protocol || !class_conformsToProtocol(device, protocol))
            return fail(5, [NSString stringWithFormat:@"GarnetDevice lacks protocol %s", protocols[i]]);
        printf("PASS protocol: %s\n", protocols[i]);
    }
    const char *selectors[] = {"initWithAcceleratorPort:", "newCommandQueue",
                              "newBufferWithLength:options:", "newTextureWithDescriptor:"};
    unsigned count = 0;
    Method *methods = class_copyMethodList(device, &count);
    for (NSUInteger i = 0; i < sizeof(selectors) / sizeof(selectors[0]); ++i) {
        BOOL found = NO;
        for (unsigned j = 0; j < count; ++j)
            if (strcmp(sel_getName(method_getName(methods[j])), selectors[i]) == 0) found = YES;
        if (!found) {
            free(methods);
            return fail(5, [NSString stringWithFormat:@"GarnetDevice lacks own implementation of -%s", selectors[i]]);
        }
        printf("PASS implemented selector (not invoked): -%s\n", selectors[i]);
    }
    free(methods);
    puts("PASS load-only verification; no device instantiated; GPU/Metal execution not tested.");
    return 0;
}

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        if (argc == 2 && strcmp(argv[1], "--help") == 0) {
            printf("Usage: %s /path/to/GarnetMetal.bundle\nLoad-only verifier; no accelerator or device is created.\n", argv[0]);
            return 0;
        }
        if (argc != 2 || !argv[1][0] || argv[1][0] == '-') {
            if (argc != 2) fprintf(stderr, "Expected exactly one bundle path; got %d arguments.\n", argc - 1);
            else if (!argv[1][0]) fputs("Bundle path is empty.\n", stderr);
            else if (strcmp(argv[1], "--help") != 0) fprintf(stderr, "Unknown option: %s\n", argv[1]);
            fprintf(stderr, "Usage: %s /path/to/GarnetMetal.bundle\n"
                    "Load-only verifier; no accelerator or device is created.\n"
                    "Exit: 0=verified, 2=usage, 3=invalid path/bundle, 4=load/export/ABI, 5=class contract.\n",
                    argv[0]);
            return 2;
        }
        @try { return checkBundle(argv[1]); }
        @catch (NSException *exception) {
            return fail(4, [NSString stringWithFormat:@"Objective-C exception %@: %@", exception.name, exception.reason]);
        }
    }
}
