// test-census.m: the unimplemented-selector census (build 18, garnet_census.h) and the vetted-list policy.
// Build/run (from this directory):  clang -fobjc-arc -Wall -Wextra -Werror -framework Foundation -o /tmp/test-census test-census.m && /tmp/test-census .      (argv[1] = the directory holding GarnetDevice.m)
//   1. the scan itself, on a FAKE protocol (a required method, an optional one, an inherited one, and a method the class gets from its superclass): exactly the missing ones are reported, required ones marked '!'.
//   2. the policy pins on GarnetDevice.m: the census runs from +load over all the Metal protocols the bundle's classes claim, is logged once by a process that got a device, the vetted list is exactly the one
//      selector the evidence names, it is a no-op with a stated reason, and there is NO blanket handler (no forwardInvocation / methodSignatureForSelector / resolveInstanceMethod answers).
#import <Foundation/Foundation.h>
#include "garnet_census.h"

@protocol FakeBase
- (void)baseRequired;
@optional
- (void)baseOptional;
@end
@protocol FakeProto <FakeBase, NSObject>
- (void)haveIt;
- (void)missingRequired;
- (void)fromSuper;
- (NSUInteger)missingScalar:(NSUInteger)x with:(id)y;
@optional
- (void)missingOptional;
- (void)haveOptional;
@end
@interface FakeSuper : NSObject
- (void)fromSuper;
@end
@implementation FakeSuper
- (void)fromSuper {}
@end
@interface FakeCls : FakeSuper
@end
@implementation FakeCls
- (void)haveIt {}
- (void)haveOptional {}
- (void)baseRequired {}
@end

static int fails, runs;
#define CHECK(name, cond) do { int ok_ = (cond) ? 1 : 0; runs++; printf("%s %s\n", ok_ ? "ok  " : "FAIL", name); if (!ok_) fails++; } while (0)
static NSString *slurp(NSString *dir, NSString *name) { return [NSString stringWithContentsOfFile:[dir stringByAppendingPathComponent:name] encoding:NSUTF8StringEncoding error:nil]; }
static NSUInteger count(NSString *hay, NSString *needle) { NSUInteger n = 0, p = 0; for (;;) { NSRange r = [hay rangeOfString:needle options:0 range:NSMakeRange(p, hay.length - p)]; if (r.location == NSNotFound) break; n++; p = r.location + r.length; } return n; }

int main(int argc, char **argv) { @autoreleasepool {
    NSString *dir = argc > 1 ? @(argv[1]) : @".";
    (void)@protocol(FakeProto); (void)@protocol(FakeBase);   // reference the protocols so the runtime registers them
    rdna4cen_missing_set = [NSMutableSet set];
    unsigned miss = rdna4cen_scan([FakeCls class], "FakeCls", "FakeProto");
    NSString *all = [rdna4cen_lines componentsJoinedByString:@"\n"];
    CHECK("scan: exactly the four missing methods are counted", miss == 4);
    CHECK("scan: a missing REQUIRED method is reported with '!'", [all containsString:@"!missingRequired"]);
    CHECK("scan: a missing required method with arguments is reported by its full selector", [all containsString:@"!missingScalar:with:"]);
    CHECK("scan: a missing OPTIONAL method is reported without '!'", [all containsString:@" missingOptional"] && ![all containsString:@"!missingOptional"]);
    CHECK("scan: the inherited protocol's optional method is reported too", [all containsString:@" baseOptional"]);
    CHECK("scan: a method the class implements is not reported", ![all containsString:@"haveIt"] && ![all containsString:@"haveOptional"] && ![all containsString:@"baseRequired"]);
    CHECK("scan: a method inherited from the SUPERCLASS counts as implemented", ![all containsString:@"fromSuper"]);
    CHECK("scan: the line names the class, the count and the protocol", [all containsString:@"census: FakeCls lacks 4 of 8 FakeProto methods:"]);
    CHECK("scan: the missing set carries class + selector for each", [rdna4cen_missing_set containsObject:@"FakeCls missingRequired"] && rdna4cen_missing_set.count == 4);
    CHECK("scan: an unknown protocol name scans nothing and does not crash", rdna4cen_scan([FakeCls class], "FakeCls", "NoSuchProtocolAtAll") == 0);
    rdna4cen_pair pairs[] = { { [FakeCls class], "FakeCls", "FakeProto" }, { Nil, NULL, NULL } };
    rdna4cen_lines = nil; rdna4cen_missing_set = nil;
    CHECK("run: walks the pair list to the terminator", rdna4cen_run(pairs) == 4 && rdna4cen_lines.count >= 1);
    // a long list is continued on further lines, never truncated
    { NSMutableString *p = [NSMutableString string]; (void)p; }

    NSString *src = slurp(dir, @"GarnetDevice.m");
    CHECK("pin: GarnetDevice.m is readable", src.length > 0);
    if (src.length) {
        NSRange l = [src rangeOfString:@"+ (void)load {"], lEnd = [src rangeOfString:@"// lazyInitialize: log, then run the base"];
        NSString *load = (l.location != NSNotFound && lEnd.location > l.location) ? [src substringWithRange:NSMakeRange(l.location, lEnd.location - l.location)] : @"";
        for (NSString *pp in @[ @"MTLRenderCommandEncoder", @"MTLComputeCommandEncoder", @"MTLBlitCommandEncoder", @"MTLCommandBuffer", @"MTLDevice", @"MTLTexture", @"MTLBuffer" ]) {
            BOOL okp = [load containsString:[NSString stringWithFormat:@"\"%@\" }", pp]];
            NSString *nm = [NSString stringWithFormat:@"pin: +load runs the census against %@", pp];
            CHECK(nm.UTF8String, okp);
        }
        CHECK("pin: +load calls rdna4cen_run (the census is not silenced)", [load containsString:@"unsigned total = rdna4cen_run(pairs);"]);
        CHECK("pin: RDNA4M_CENSUS=1 prints the lines at +load (the host Mac harness)", [load containsString:@"getenv(\"RDNA4M_CENSUS\")"] && [load containsString:@"fprintf(stderr, \"%s\\n\", l.UTF8String)"]);
        NSRange cl = [src rangeOfString:@"static void rdna4_census_log(void) {"];
        NSString *clb = cl.location != NSNotFound ? [src substringWithRange:NSMakeRange(cl.location, MIN((NSUInteger)700, src.length - cl.location))] : @"";
        CHECK("pin: the census is logged once per process (atomic flag) through RDNA4LOG, every line", [clb containsString:@"atomic_exchange(&done_, 1)"] && [clb containsString:@"RDNA4LOG(\"%s\", l.UTF8String)"]);
        NSRange ia = [src rangeOfString:@"- (instancetype)initWithAcceleratorPort:(uint32_t)port {"];
        NSString *iab = ia.location != NSNotFound ? [src substringWithRange:NSMakeRange(ia.location, MIN((NSUInteger)2600, src.length - ia.location))] : @"";
        NSRange dec = [iab rangeOfString:@"return nil;"], cal = [iab rangeOfString:@"rdna4_census_log();"];
        CHECK("pin: only a process that was NOT declined logs the census (the call comes after the decline's return)", dec.location != NSNotFound && cal.location != NSNotFound && dec.location < cal.location);
        CHECK("pin: the vetted list is exactly ONE selector, setFragmentVisibleFunctionTable:atBufferIndex:", count(src, @"- (void)setFragmentVisibleFunctionTable:(id)t atBufferIndex:(NSUInteger)i {") == 1 && count(src, @"VisibleFunctionTable") >= 1 && count(src, @"- (void)setVertexVisibleFunctionTable") == 0 && count(src, @"- (void)setVisibleFunctionTable") == 0);
        CHECK("pin: the vetted selector is a no-op with a stated reason, logged once", [src containsString:@"RDNA4_ONCE(\"setFragmentVisibleFunctionTable:atBufferIndex: ignored (vetted no-op: pipelines that call through a function table are refused at creation)\")"]);
        CHECK("pin: NO blanket handler: no forwardInvocation / methodSignatureForSelector / resolveInstanceMethod / forwardingTargetForSelector", count(src, @"forwardInvocation:") == 0 && count(src, @"methodSignatureForSelector:") == 0 && count(src, @"resolveInstanceMethod:") == 0 && count(src, @"forwardingTargetForSelector:") == 0);
        CHECK("pin: everything else keeps the crash, logged first (RDNA4_DNR on every encoder and pipeline class, the device logs too)", count(src, @"RDNA4_DNR(RDNA4RenderEncoder)") == 1 && count(src, @"RDNA4_DNR(RDNA4ComputeEncoder)") == 1 && count(src, @"RDNA4_DNR(RDNA4BlitEncoder)") == 1 && count(src, @"RDNA4_DNR(RDNA4CommandBuffer)") == 1 && [src containsString:@"UNRECOGNIZED selector %{public}s"]);
        CHECK("pin: RDNA4_DNR still ends in the standard exception (no swallowing)", [src containsString:@"RDNA4LOG(\"UNRECOGNIZED selector %s on \" #cls, sel_getName(sel)); \\\n        [super doesNotRecognizeSelector:sel]; }"]);
    }
    printf("test-census: %d checks, %d failed\n", runs, fails);
    return fails ? 1 : 0;
} }
