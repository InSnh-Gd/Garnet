// Real extracted methods/selection; textures, arrays (Linux), pixels and GPU work are mocked.
#include <stdio.h>
#include <limits.h>
#include <unistd.h>
#include "rdna4_dispflip.h"
#include "rdna4_cblog.h"
#include "rdna4_m6route.h"
#include "rdna4_m6x.h"
#define RDNA4_ONCE(...) ((void)0)
#ifdef __APPLE__
#import <Foundation/Foundation.h>
#define RDNA4Base NSObject
#define RDNA4Array NSMutableArray
#else
typedef unsigned char BOOL;
typedef unsigned long NSUInteger;
#define YES ((BOOL)1)
#define NO ((BOOL)0)
#define nil ((id)0)
#define NSNotFound ULONG_MAX
extern id class_createInstance(Class cls, unsigned long extra);
typedef struct { unsigned long state; id *itemsPtr; unsigned long *mutationsPtr; unsigned long extra[5]; } NSFastEnumerationState;
__attribute__((objc_root_class))
@interface RDNA4Base { Class isa; }
+ (id)new;
@end
@implementation RDNA4Base
+ (id)new { return class_createInstance(self, 0); }
@end
@interface RDNA4Array : RDNA4Base { id _items[16]; NSUInteger _count, _mutation; }
+ (id)array;
- (NSUInteger)indexOfObjectIdenticalTo:(id)t;
- (void)addObject:(id)t;
- (void)removeObjectIdenticalTo:(id)t;
- (NSUInteger)count;
- (NSUInteger)countByEnumeratingWithState:(NSFastEnumerationState *)s objects:(id *)b count:(NSUInteger)n;
@end
@implementation RDNA4Array
+ (id)array { return [self new]; }
- (NSUInteger)indexOfObjectIdenticalTo:(id)t { for (NSUInteger i = 0; i < _count; i++) if (_items[i] == t) return i; return NSNotFound; }
- (void)addObject:(id)t { if (_count == 16) __builtin_trap(); _items[_count++] = t; _mutation++; }
- (void)removeObjectIdenticalTo:(id)t {
    for (NSUInteger i = 0; i < _count;) {
        if (_items[i] != t) { i++; continue; }
        for (NSUInteger j = i + 1; j < _count; j++) _items[j - 1] = _items[j];
        _count--; _mutation++;
    }
}
- (NSUInteger)count { return _count; }
- (NSUInteger)countByEnumeratingWithState:(NSFastEnumerationState *)s objects:(id *)b count:(NSUInteger)n {
    (void)b; (void)n; if (s->state) return 0;
    s->state = 1; s->itemsPtr = _items; s->mutationsPtr = &_mutation; return _count;
}
@end
#define NSMutableArray RDNA4Array
#endif

@interface RDNA4Texture : RDNA4Base { @public RDNA4Texture *_root; unsigned sid, pixel; }
- (id)rootResource;
- (BOOL)rdna4IsIOS;
- (BOOL)rdna4IOSLinear;
- (BOOL)rdna4IsDisp;
- (BOOL)rdna4DispImgOnly;
- (id)rdna4IOSRef;
- (unsigned)rdna4DispSid;
@end
@implementation RDNA4Texture
#include "root.inc"
- (BOOL)rdna4IsIOS { return YES; }
- (BOOL)rdna4IOSLinear { return NO; }
- (BOOL)rdna4IsDisp { return YES; }
- (BOOL)rdna4DispImgOnly { return NO; }
- (id)rdna4IOSRef { return self; }
- (unsigned)rdna4DispSid { return sid; }
@end
static unsigned IOSurfaceGetID(RDNA4Texture *s) { return s->sid; }
static int rdna4s_damage_on(void) { return 0; }
@interface WriteFixture : RDNA4Base {
    RDNA4Array *_iow, *_iot, *_refs, *_eventDirty;
    uint32_t _ncbw, _cbw[RDNA4CB_MAXW];
    BOOL _scheduleAfterPublish;
    BOOL _shaderFallback;
    int _ndw;
    struct { __unsafe_unretained RDNA4Texture *t; uint32_t mask; char fns[128]; rdna4df_wr_t wr; } _dw[4];
}
- (void)rdna4IOWritten:(RDNA4Texture *)t;
- (void)rdna4NoteShaderFallback;
- (void)rdna4CBTraceSurface:(RDNA4Texture *)texture use:(const char *)use;
- (void)write:(RDNA4Texture *)t bits:(unsigned)bits pixel:(unsigned)v;
- (RDNA4Texture *)chosen;
- (BOOL)accountsFinalAfterFallback:(BOOL)fallback;
- (BOOL)counts:(unsigned)n;
@end
@implementation WriteFixture
#include "written.inc"
#include "fallback.inc"
- (void)rdna4CBTraceSurface:(RDNA4Texture *)texture use:(const char *)use {
    (void)texture;
    (void)use;
}
- (void)write:(RDNA4Texture *)t bits:(unsigned)bits pixel:(unsigned)v {
    RDNA4Texture *root = [t rootResource];
    // Model first-touch retention independently of the list being reordered.
    if (!_iow) { _iow = [RDNA4Array new]; _iot = [RDNA4Array new]; _refs = [RDNA4Array new]; }
    if ([_iot indexOfObjectIdenticalTo:root] == NSNotFound) { [_iot addObject:root]; [_refs addObject:root]; }
    if (root != t) [_refs addObject:t];
    [self rdna4IOWritten:t]; root->pixel = v;
    int i = 0; for (; i < _ndw; i++) if (_dw[i].t == root) break;
    if (i == _ndw) { _dw[i].t = root; _ndw++; }
    _dw[i].mask |= bits;   // Same per-root OR input supplied by rdna4DispNote.
}
- (RDNA4Texture *)chosen {
#include "select.inc"
    (void)nd; (void)dmask; (void)dfns; (void)dwr; return dt;
}
- (BOOL)accountsFinalAfterFallback:(BOOL)fallback {
    if (fallback) [self rdna4NoteShaderFallback];
#include "select.inc"
    (void)nd; (void)dfns; (void)dwr;
    rdna4df_t state;
    rdna4df_init(&state, 1);
    rdna4df_activate(&state, 3);
    return dt && rdna4df_account_sid(&state, dmask, dt->sid, 1);
}
- (BOOL)counts:(unsigned)n {
    BOOL roots = YES; for (RDNA4Texture *t in _iow) if ([t rootResource] != t) roots = NO;
    return roots && [_iow count] == n && [_iot count] == n && [_eventDirty count] == n &&
        _ndw == (int)n && _ncbw == n && _scheduleAfterPublish;
}
@end

#define F (RDNA4DF_W_PASS | RDNA4DF_W_DRAW_FINAL)
#define N (RDNA4DF_W_PASS | RDNA4DF_W_DRAW_COMP)
typedef struct { unsigned tex, bits, pixel; } Write;
static const struct {
    const char *name;
    Write writes[5]; unsigned n, expected, pixel, roots;
} cases[] = {
    { "A,B,A", {{0,F,1}, {1,F,2}, {0,F,3}}, 3, 0, 3, 2 },
    { "A,B control", {{0,F,1}, {1,F,2}}, 2, 1, 2, 2 },
    { "A,A,A stays unique", {{0,F,1}, {0,F,2}, {0,F,3}}, 3, 0, 3, 1 },
    { "viewA,B,nestedViewA", {{3,F,1}, {1,F,2}, {4,F,3}}, 3, 0, 3, 2 },
    { "viewA,nestedViewA one root", {{3,F,1}, {4,F,2}}, 2, 0, 2, 1 },
    { "FINAL beats later nonFINAL", {{0,F,1}, {1,N,2}}, 2, 0, 1, 2 },
    { "latest FINAL root before nonFINAL tail", {{0,F,1}, {1,F,2}, {0,F,3}, {2,N,4}}, 4, 0, 3, 3 },
    { "FINAL mask survives nonFINAL revisit", {{0,F,1}, {1,F,2}, {0,N,3}}, 3, 0, 3, 2 },
};
static int run_tests(void) {
    unsigned failed = 0;
    puts("Mock pixels/GPU; real extracted rootResource, rdna4IOWritten and rdna4Finish selection; same DP pipe");
    for (unsigned c = 0; c < sizeof cases / sizeof cases[0]; c++) {
        RDNA4Texture *tex[5]; for (unsigned i = 0; i < 5; i++) { tex[i] = [RDNA4Texture new]; tex[i]->sid = 101 + i; }
        tex[3]->_root = tex[0]; tex[4]->_root = tex[3];
        WriteFixture *cb = [WriteFixture new];
        for (unsigned i = 0; i < cases[c].n; i++) { Write w = cases[c].writes[i]; [cb write:tex[w.tex] bits:w.bits pixel:w.pixel]; }
        RDNA4Texture *chosen = [cb chosen], *expected = tex[cases[c].expected];
        // Both candidate IDs are explicitly known, unambiguous surfaces of instance 0.
        rdna4m6_t table = { .latch = 1, .have_table = 1, .n = 3, .gen = 7, .have_gen = 1,
            .e = {{101,0,0}, {102,0,0}, {103,0,0}} };
        int tentative = 0, pipe = rdna4x_plan_inst(&table, chosen->sid, 0, &tentative);
        int route = rdna4x_complete_plan(&table, chosen->sid, pipe, 0, 0, 0);
        rdna4df_t sm; rdna4df_init(&sm, 0); rdna4df_activate(&sm, 3);
        int accounted = rdna4df_account_sid(&sm, F, chosen->sid, 1);
        uint32_t flags[3] = { RDNA4DF_SLOT_REUSABLE, RDNA4DF_SLOT_REUSABLE, RDNA4DF_SLOT_REUSABLE };
        int slot = rdna4df_pick(&sm, flags);
        rdna4df_submit(&sm, slot, chosen->sid, 1);
        int present = rdna4df_complete_surf(&sm, slot, 0, 1, chosen->sid);
        if (present) rdna4df_present_result(&sm, 0);
        BOOL ok = chosen == expected && chosen->pixel == cases[c].pixel && [cb counts:cases[c].roots] &&
            pipe == 0 && !tentative && route == RDNA4X_C_PRESENT && accounted && present &&
            sm.last_seq == 1 && sm.presents == 1 && !sm.inflight[slot];
        printf("%s %s: sid=%u pixel=%u expected=%u/%u pipe=%d route=%d account=%d present=%d inflight=%u unique_roots=%d schedule_after_publish=%d\n",
            ok ? "PASS" : "FAIL", cases[c].name, chosen->sid, chosen->pixel, expected->sid, cases[c].pixel,
            pipe, route, accounted, present, sm.inflight[slot], [cb counts:cases[c].roots], 1);
        if (!ok) failed++;
    }
    RDNA4Texture *display = [RDNA4Texture new];
    display->sid = 101;
    WriteFixture *command = [WriteFixture new];
    [command write:display bits:F pixel:1];
    const BOOL noFallback = [command accountsFinalAfterFallback:NO];
    const BOOL withFallback = [command accountsFinalAfterFallback:YES];
    const BOOL suppressesIncomplete = noFallback && !withFallback;
    printf("%s FINAL with earlier offscreen fallback: clean=%d incomplete=%d\n",
        suppressesIncomplete ? "PASS" : "FAIL", noFallback, withFallback);
    if (!suppressesIncomplete) failed++;
    printf("display-write-order: %zu cases, %u failed\n", sizeof cases / sizeof cases[0] + 1, failed);
    return failed ? 1 : 0;
}
int main(void) {
#ifdef __APPLE__
    @autoreleasepool { return run_tests(); }
#else
    return run_tests();
#endif
}
