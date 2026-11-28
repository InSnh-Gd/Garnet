#import <Foundation/Foundation.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
static NSString *captureFlag;
#define RDNA4_SHADER_CAPTURE_FLAG captureFlag.fileSystemRepresentation
#include "dump-write.inc"
#define GARNET_SHADER_REQUEST_CAPTURE_ONLY 1
#include "garnet_shaders.m"

static unsigned failures;
#define CHECK(label, expression) do { BOOL okay = (expression); printf("%s %s\n", okay ? "PASS" : "FAIL", label); failures += !okay; } while (0)

int main(int argc, const char **argv) { @autoreleasepool {
    if (argc != 2) return 2;
    NSString *directory = [@(argv[1]) stringByAppendingFormat:@"/request-%d", getpid()];
    [[NSFileManager defaultManager] createDirectoryAtPath:directory withIntermediateDirectories:YES attributes:nil error:NULL];
    captureFlag = [directory stringByAppendingPathComponent:@"enabled"];
    NSString *sha = @"949955cbac3972951101fcc4e41c54cb617ed92274513829067595d2d4e9e2cd";
    NSString *output = [directory stringByAppendingPathComponent:[sha stringByAppendingString:@".capture.plist"]];
    NSData *entry = [@"entry bitcode" dataUsingEncoding:NSUTF8StringEncoding];
    NSData *dependency = [@"linked bitcode" dataUsingEncoding:NSUTF8StringEncoding];
    NSArray *deps = @[@{@"name": @"custom_fn", @"bc": dependency}];
    CHECK("disabled capture writes no shader source", rdna4x_capture_request(directory, sha, @"fragment_name", "fragment", entry, deps) == 0);
    [@"ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff\n" writeToFile:captureFlag atomically:YES encoding:NSUTF8StringEncoding error:NULL];
    CHECK("capture opt-in ignores unrelated shader keys", rdna4x_capture_request(directory, sha, @"fragment_name", "fragment", entry, deps) == 0);
    unlink(captureFlag.fileSystemRepresentation);
    [[sha stringByAppendingString:@"\n"] writeToFile:captureFlag atomically:YES
        encoding:NSUTF8StringEncoding error:NULL];
    int result = rdna4x_capture_request(directory, sha, @"fragment_name", "fragment", entry, deps);
    NSDictionary *request = [NSDictionary dictionaryWithContentsOfURL:[NSURL fileURLWithPath:output] error:NULL];
    CHECK("capture preserves the failed entry and its exact linked dependency", result == 1 && request &&
        [request[@"sha256"] isEqual:sha] && [request[@"function"] isEqual:@"fragment_name"] &&
        [request[@"role"] isEqual:@"fragment"] && [request[@"air"] isEqual:entry] &&
        [request[@"dependencies"] isEqual:deps]);
    printf("SHADER_REQUEST_CAPTURE failures=%u\n", failures);
    return failures ? 1 : 0;
} }
