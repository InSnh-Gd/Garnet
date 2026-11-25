#!/bin/bash
set -euo pipefail
[ "$(uname -s)" = Darwin ] || exit 2
source_dir=${1:-$(cd "$(dirname "$0")" && pwd)}
out=$(mktemp -d "${TMPDIR:-/tmp}/render-daemon-miss.XXXXXXXX")
trap 'rm -rf "$out"' EXIT
{
    cat <<'OBJC'
#import <Foundation/Foundation.h>
#include <stdatomic.h>
#define MTLRenderPipelineDescriptor RenderMissDescriptor

@interface MTLRenderPipelineDescriptor : NSObject
@property(copy) NSString *vertexFunction;
@property(copy) NSString *fragmentFunction;
@end
@implementation MTLRenderPipelineDescriptor
@end

static NSMutableArray<NSString *> *queued;
static NSArray<NSString *> *waited;
static NSSet<NSString *> *ready;
static _Atomic int rdna4_app_admitted;
#define RDNA4LOG(...) ((void)0)

static NSError *rdna4_err(NSInteger code, NSString *text) {
    return [NSError errorWithDomain:@"test" code:code userInfo:@{NSLocalizedDescriptionKey: text}];
}

static void rdna4_dump_function(NSString *shader, const char *role, NSMutableArray *paths, NSMutableArray *notes) {
    if (!shader) { [notes addObject:@"nil shader"]; return; }
    [queued addObject:shader];
    [paths addObject:@(role)];
}

static BOOL rdna4_is_ws(void) { return YES; }
static BOOL rdna4_force_fallback(void) { return NO; }
static BOOL rdna4_test_fb_as_ws(void) { return NO; }
static BOOL rdna4g_syncwait_applies(BOOL ws, BOOL admitted, BOOL force, BOOL test) {
    return (ws || admitted) && !force && !test;
}
static NSString *rdna4_fn_sha(NSString *shader) { return shader; }

static BOOL rdna4_sync_wait(const char *label, NSArray *keys, BOOL needMeta) {
    if (strcmp(label, "render") || needMeta) abort();
    waited = [keys copy];
    return [[NSSet setWithArray:keys] isSubsetOfSet:ready];
}

static NSData *rdna4_spv_lookup(NSString *shader, const char *role, NSError **error,
                               NSString **name, NSDictionary **meta) {
    *name = shader;
    *meta = nil;
    if ([ready containsObject:shader]) return [NSData data];
    *error = rdna4_err(41, @(role));
    return nil;
}
OBJC
    echo '#define GARNET_INTERNAL static'
    awk '/^GARNET_INTERNAL NSError \*rdna4_dump_pipeline\(/ {emit=1} emit {print} emit && /^}/ {exit}' "$source_dir/garnet_shaders.m"
    cat <<'OBJC'
static BOOL check(const char *label, BOOL vertexHit, BOOL fragmentHit, BOOL hasFrag, BOOL inproc,
                  NSArray<NSString *> *expected) {
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = @"vertex";
    if (hasFrag) d.fragmentFunction = @"fragment";
    NSData *vsd = vertexHit ? [NSData data] : nil;
    NSData *fsd = fragmentHit ? [NSData data] : nil;
    NSError *e1 = nil, *e2 = nil;
    NSString *vname = nil, *fname = nil;
    NSDictionary *mv_ = nil, *mf_ = nil;
    char _vnm[16], _fnm[16];
    queued = [NSMutableArray array];
    waited = @[];
    ready = [NSSet setWithArray:expected];
OBJC
    awk '/rdna4_dump_pipeline\(d/ {emit=1} emit {print} /NSError \*dumpFailure =/ {print "    (void)dumpFailure;"} emit && /^            }$/ {exit}' "$source_dir/garnet_pipelines.m"
    cat <<'OBJC'
    BOOL okay = [queued isEqualToArray:expected] && [waited isEqualToArray:expected] &&
        (inproc || (vsd && (!hasFrag || fsd)));
    printf("%s %s: queued=[%s] waited=[%s] stages=%d/%d\n", okay ? "PASS" : "FAIL", label,
           [queued componentsJoinedByString:@","].UTF8String, [waited componentsJoinedByString:@","].UTF8String,
           vsd != nil, fsd != nil);
    return okay;
}

int main(void) {
    @autoreleasepool {
        BOOL okay = check("cached vertex, cold fragment", YES, NO, YES, NO, @[@"fragment"]);
        okay &= check("cold vertex, cached fragment", NO, YES, YES, NO, @[@"vertex"]);
        okay &= check("two missing stages", NO, NO, YES, NO, @[@"vertex", @"fragment"]);
        okay &= check("vertex-only miss", NO, NO, NO, NO, @[@"vertex"]);
        okay &= check("in-process miss", YES, NO, YES, YES, @[]);
        return okay ? 0 : 1;
    }
}
OBJC
} | "${CC:-clang}" -std=gnu11 -fobjc-arc -Wall -Wextra -Werror -framework Foundation -x objective-c - -o "$out/test"
"$out/test"
