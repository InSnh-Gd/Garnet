// Real deferred-copy module and extracted command-buffer entry points; GPU/scanout are mocked.
#import <Foundation/Foundation.h>
#import <IOSurface/IOSurface.h>
#include <pthread.h>
#include <stdatomic.h>
#include "garnet_dispflip.h"
#include "garnet_display_commits.h"
#include "garnet_cblog.h"
typedef void *VkCommandBuffer;
typedef unsigned *VkBuffer;
typedef int VkResult;
#define VK_SUCCESS 0
#define VK_ERROR_UNKNOWN (-13)
#define VK_NULL_HANDLE NULL
#define RDNA4LOG(...) ((void)0)
#define RDNA4LOGR(...) ((void)0)
static NSMutableArray *work;
static void enqueue(dispatch_block_t block) { [work addObject:[block copy]]; }
#define dispatch_async(q, b) enqueue(b)
#define dispatch_after(t, q, b) enqueue(b)
static void drain(void) {
    unsigned count = 0;
    while (work.count && count++ < 100) {
        dispatch_block_t block = work.firstObject; [work removeObjectAtIndex:0];
        @autoreleasepool { block(); }
    }
    if (work.count) abort();
}
static uint64_t testClockNs = 1000000000ull;
static uint64_t rdna4_now(void) { return testClockNs; }
static struct { pthread_mutex_t mu; rdna4df_t sm; _Atomic uint64_t seq; uint32_t pw, ph, pitch; rdna4_display_commit *commits; NSMutableIndexSet *incompleteSources; NSHashTable *incompleteImages; uint64_t incompleteSinceNs, incompleteLastFallbackNs; BOOL incompleteCapLogged; } RDNA4S = { .mu = PTHREAD_MUTEX_INITIALIZER };
static struct { rdna4df_t sm; } instances[3];
#define RDNA4XI(i) instances[i]
typedef struct { unsigned w, h, pitch_bytes; } rdna4x_desc_t;
static const rdna4x_desc_t *rdna4x_desc(uint32_t inst) { (void)inst; return NULL; }
static dispatch_queue_t rdna4s_present_queue(void) { return NULL; }
static unsigned flags[3], pixels[3], visible, copies, errors, released, attempts;
static BOOL failCopy, hangCopy;
static dispatch_block_t quarantine, beforeCopy;
static BOOL rdna4_is_ws(void) { return YES; }
static BOOL rdna4s_testmode(void) { return NO; }
@interface RDNA4Texture : NSObject { @public unsigned value, surfaceID; IOSurfaceRef surface; BOOL linear, gpuOnly; }
- (NSUInteger)width; - (NSUInteger)height; - (NSUInteger)bytesPerPixel; - (NSUInteger)rdna4IOSBytesPerRow; - (NSUInteger)rdna4IOSOffset;
- (VkBuffer)rdna4DispBuf; - (IOSurfaceRef)rdna4UCSurface; - (IOSurfaceRef)rdna4IOSRef;
- (unsigned)rdna4DispSid;
- (BOOL)rdna4IsIOS; - (BOOL)rdna4IOSLinear; - (BOOL)rdna4IsDisp; - (BOOL)rdna4DispImgOnly;
- (RDNA4Texture *)rootResource;
@end
@implementation RDNA4Texture
- (instancetype)init {
    if ((self = [super init])) {
        surfaceID = 2;
        surface = IOSurfaceCreate((__bridge CFDictionaryRef)@{
            (id)kIOSurfaceWidth:@2, (id)kIOSurfaceHeight:@2, (id)kIOSurfaceBytesPerElement:@4 });
    }
    return self;
}
- (void)dealloc { if (surface) CFRelease(surface); }
- (NSUInteger)width { return 2; } - (NSUInteger)height { return 2; }
- (NSUInteger)bytesPerPixel { return 4; } - (NSUInteger)rdna4IOSBytesPerRow { return 8; }
- (NSUInteger)rdna4IOSOffset { return 0; }
- (VkBuffer)rdna4DispBuf { return &value; }
- (unsigned)rdna4DispSid { return surfaceID; }
- (IOSurfaceRef)rdna4UCSurface { return surface; } - (IOSurfaceRef)rdna4IOSRef { return gpuOnly ? NULL : surface; }
- (BOOL)rdna4IsIOS { return !gpuOnly; } - (BOOL)rdna4IOSLinear { return linear; } - (BOOL)rdna4IsDisp { return gpuOnly; }
- (BOOL)rdna4DispImgOnly { return gpuOnly; }
- (RDNA4Texture *)rootResource { return self; }
@end
static int rdna4s_plan(const rdna4df_wr_t *wr, int inst, int tent, rdna4df_plan_t *plan) {
    (void)wr; (void)inst; attempts++;
    return rdna4df_plan_ex(&RDNA4S.sm, flags, RDNA4DF_K_FULL, (rdna4df_rect_t){0,0,2,2}, tent, plan);
}
static void rdna4_full_barrier(VkCommandBuffer cmd) { (void)cmd; }
static void rdna4s_record_copy(VkCommandBuffer cmd, VkBuffer src, uint64_t srcOffset, const rdna4df_plan_t *plan) {
    (void)cmd; (void)srcOffset; pixels[plan->slot] = *src; copies++;
}
static BOOL rdna4_oneshot(NSError **err, void (^record)(VkCommandBuffer), BOOL *unresolved) {
    (void)err; *unresolved = hangCopy;
    if (hangCopy) { quarantine = [^{ record(NULL); } copy]; return NO; }
    if (failCopy) return NO;
    if (beforeCopy) { dispatch_block_t action = beforeCopy; beforeCopy = nil; action(); }
    record(NULL); return YES;
}
static void rdna4x_do_release_locked(int inst) { (void)inst; released++; }
static void rdna4s_do_release_locked(void) { released++; }
static void rdna4cbl_skip(uint32_t sid, uint64_t seq, int slot, const char *why, int result) {
    (void)sid; (void)seq; (void)slot; (void)why; (void)result;
}
static void rdna4s_complete_run(int inst, int slot, VkResult r, uint64_t seq, uint32_t sid, void *crc, int cls, uint64_t commit, int tries) {
    (void)inst; (void)crc; (void)cls; (void)commit; (void)tries;
    if (rdna4df_complete_surf(&RDNA4S.sm, slot, r, seq, sid)) { visible = pixels[slot]; flags[slot] = 0; }
}
static void rdna4s_complete(int inst, int slot, VkResult r, uint64_t seq, uint32_t sid, void *crc, int cls, uint64_t commit) {
    rdna4s_complete_run(inst, slot, r, seq, sid, crc, cls, commit, 0);
}
#ifndef TEST_LEGACY
#define GARNET_DISPLAY_LATE_ONLY 1
#include "garnet_surface.m"
#else
static void rdna4s_late_complete(RDNA4Texture *t, int i, uint64_t s, uint32_t sid, int c, uint64_t commit) {
    (void)t; (void)i; (void)s; (void)sid; (void)c; (void)commit;
}
#endif
typedef void rdna4crc_rec_t;
static BOOL rdna4s_crc_on(void) { return NO; }
static rdna4crc_rec_t *rdna4s_crc_check(RDNA4Texture *t, int s, uint64_t seq) { (void)t; (void)s; (void)seq; return NULL; }
static void rdna4s_crc_note(uint32_t sid, uint64_t seq) { (void)sid; (void)seq; }
@interface RDNA4CommandSegment : NSObject {
@public
    NSUInteger hostWritebackCopies;
    NSUInteger hostLinearWrites;
    NSMutableIndexSet *displaySourceWrites, *displaySourceReads;
    NSMutableDictionary<NSNumber *, RDNA4Texture *> *displaySourceOverwrites;
    NSHashTable<RDNA4Texture *> *displayImageWrites, *displayImageReads, *displayImageOverwrites;
}
@end
@implementation RDNA4CommandSegment
@end
@interface Fixture : NSObject {
@public
    BOOL _dlate, _dcrc, _shaderFallback, _displayIncomplete; RDNA4Texture *_dtex;
    int _dslot, _dinst, _dcls, _cbcls; uint64_t _dseq, _dcid, _dbid, _tcommit; uint32_t _dsid;
    uint32_t _cbr[RDNA4CB_MAXR], _ncbr;
    rdna4_display_commit _displayCommits[4];
    RDNA4CommandSegment *_curSegment;
    struct { RDNA4Texture *t; uint32_t mask; } _dw[4]; int _ndw;
}
- (void)rdna4DispDone:(VkResult)r; - (void)rdna4DispSeq;
@end
@interface Fixture (CommitTracking)
- (void)rdna4DisplayCommit; - (void)rdna4DisplayUncommit;
- (void)rdna4NoteHostWriteback:(RDNA4Texture *)texture;
- (void)rdna4NoteSurfaceOverwrite:(RDNA4Texture *)texture width:(uint32_t)width height:(uint32_t)height level:(uint32_t)level;
- (void)rdna4CBRead:(RDNA4Texture *)texture;
- (void)rdna4CBTraceSurface:(RDNA4Texture *)texture use:(const char *)use;
- (void)rdna4DispSourceWrites:(RDNA4CommandSegment *)segment;
@end
@implementation Fixture
- (void)rdna4CBTraceSurface:(RDNA4Texture *)texture use:(const char *)use { (void)texture; (void)use; }
#include "methods.inc"
@end
#define CHECK(name, cond) do { BOOL ok = (cond); printf("%s %s\n", ok ? "PASS" : "FAIL", name); if (!ok) errors++; } while (0)
static Fixture *fixture(RDNA4Texture *t) {
    Fixture *f = [Fixture new]; f->_dslot = -1; f->_dlate = YES; f->_dtex = t; f->_dsid = t->surfaceID;
    [f rdna4DispSeq]; return f;
}
static Fixture *fallback_writer(RDNA4Texture *texture) {
    Fixture *writer = [Fixture new];
    writer->_dslot = -1; writer->_tcommit = 2; writer->_shaderFallback = YES;
    writer->_ndw = 1; writer->_dw[0].t = texture;
    writer->_dw[0].mask = RDNA4DF_W_PASS | RDNA4DF_W_CLEAR | RDNA4DF_W_SHADER_FALLBACK;
    [writer rdna4DisplayCommit];
    return writer;
}
static void submit_nonframe_write(RDNA4Texture *texture, unsigned value) {
    Fixture *writer = [Fixture new];
    writer->_curSegment = [RDNA4CommandSegment new];
    if ([writer respondsToSelector:@selector(rdna4NoteHostWriteback:)]) [writer rdna4NoteHostWriteback:texture];
    if ([writer respondsToSelector:@selector(rdna4DispSourceWrites:)]) [writer rdna4DispSourceWrites:writer->_curSegment];
    texture->value = value;
}
static void reset(void) {
    work = [NSMutableArray new];
    testClockNs = 1000000000ull;
    RDNA4S.commits = NULL;
    RDNA4S.incompleteSources = nil;
    RDNA4S.incompleteImages = nil;
    RDNA4S.incompleteSinceNs = 0;
    RDNA4S.incompleteLastFallbackNs = 0;
    RDNA4S.incompleteCapLogged = NO;
    rdna4df_init(&RDNA4S.sm, 0); rdna4df_activate(&RDNA4S.sm, 3);
    RDNA4S.pw = RDNA4S.ph = 2; RDNA4S.pitch = 8; atomic_store(&RDNA4S.seq, 28);
    memset(flags, 0, sizeof flags); copies = released = attempts = 0; visible = 27; failCopy = hangCopy = NO;
}
static Fixture *submit_frame(RDNA4Texture *source, RDNA4Texture *target, BOOL fallback, BOOL clear, BOOL deferred) {
    Fixture *frame = [Fixture new];
    frame->_dslot = -1;
    frame->_shaderFallback = fallback;
    frame->_curSegment = [RDNA4CommandSegment new];
    if (source) [frame rdna4CBRead:source];
    if (clear) [frame rdna4NoteSurfaceOverwrite:target width:2 height:2 level:0];
    [frame rdna4NoteHostWriteback:target];
    if (!fallback) {
        frame->_dsid = target->surfaceID;
        frame->_dlate = deferred;
        frame->_dtex = target;
        if (!deferred) {
            frame->_dslot = 0;
            RDNA4S.sm.inflight[0] = 1;
            pixels[0] = target->value;
        }
    }
    [frame rdna4DispSourceWrites:frame->_curSegment];
    [frame rdna4DispSeq];
    return frame;
}
int main(void) { @autoreleasepool {
    reset(); RDNA4Texture *t = [RDNA4Texture new]; t->value = 29;
    int use = IOSurfaceGetUseCount(t->surface);
    RDNA4S.sm.inflight[0] = 1; rdna4df_submit(&RDNA4S.sm, 0, 2, 28); pixels[0] = 28;
    Fixture *f = fixture(t);
    rdna4s_complete_run(0, 0, 0, 28, 2, NULL, 0, 0, 0);
    CHECK("real submission path suppresses the historical intermediate snapshot", visible == 27);
    [f rdna4DispDone:VK_SUCCESS];
    CHECK("slotless terminal completion queues a deferred copy even with CRC disabled", work.count == 1);
    CHECK("surface stays use-counted while waiting for a slot", IOSurfaceGetUseCount(t->surface) == use + 1);
    if (work.count) { dispatch_block_t b = work.firstObject; [work removeObjectAtIndex:0]; b(); }
    CHECK("no write into front or pending scanout slots", copies == 0 && visible == 27);
    flags[0] = RDNA4DF_SLOT_REUSABLE; drain();
    CHECK("last composite reaches scanout without any further application damage", copies == 1 && visible == 29 && RDNA4S.sm.last_seq == 29);
    CHECK("surface use-count returns to its prior value", IOSurfaceGetUseCount(t->surface) == use);
    reset();
    RDNA4S.sm.inflight[0] = 1; RDNA4S.sm.sub_commit[0] = 1;
    rdna4df_submit(&RDNA4S.sm, 0, 2, 28); pixels[0] = 28;
    Fixture *committed = [Fixture new]; committed->_tcommit = 2;
    committed->_ndw = 1; committed->_dw[0].t = t; committed->_dw[0].mask = RDNA4DF_W_DRAW_COMP;
    if ([committed respondsToSelector:@selector(rdna4DisplayCommit)]) [committed rdna4DisplayCommit];
    rdna4s_complete_run(0, 0, 0, 28, 2, NULL, 0, 0, 0);
    CHECK("committed but unsubmitted composite suppresses the old intermediate frame", visible == 27);
    CHECK("committed update remains tracked while the scheduler has not submitted it", RDNA4S.commits != NULL);
    if ([committed respondsToSelector:@selector(rdna4DisplayUncommit)]) [committed rdna4DisplayUncommit];
    CHECK("terminal completion removes borrowed commit entries", RDNA4S.commits == NULL);
    reset();
    committed = [Fixture new]; committed->_tcommit = 2;
    committed->_ndw = 1; committed->_dw[0].t = t; committed->_dw[0].mask = RDNA4DF_W_DRAW_COMP;
    if ([committed respondsToSelector:@selector(rdna4DisplayCommit)]) [committed rdna4DisplayCommit];
    f = [Fixture new]; f->_tcommit = 1; f->_dslot = 0; f->_dsid = 2;
    RDNA4S.sm.inflight[0] = 1; pixels[0] = 28;
    [f rdna4DispSeq]; [f rdna4DispDone:0];
    CHECK("old frame submitted after the newer commit is also suppressed", visible == 27);
    if ([committed respondsToSelector:@selector(rdna4DisplayUncommit)]) [committed rdna4DisplayUncommit];
#ifndef TEST_LEGACY
    reset(); f = fixture(t); f->_tcommit = 1; [f rdna4DispDone:0];
    committed = [Fixture new]; committed->_tcommit = 2;
    committed->_ndw = 1; committed->_dw[0].t = t; committed->_dw[0].mask = RDNA4DF_W_DRAW_COMP;
    if ([committed respondsToSelector:@selector(rdna4DisplayCommit)]) [committed rdna4DisplayCommit];
    flags[0] = RDNA4DF_SLOT_REUSABLE; drain();
    CHECK("deferred source waits for a newer committed composite instead of copying old pages", copies == 0 && visible == 27);
    if ([committed respondsToSelector:@selector(rdna4DisplayUncommit)]) [committed rdna4DisplayUncommit];
    reset(); t->value = 29; f = fixture(t); f->_tcommit = 1; [f rdna4DispDone:0];
    Fixture *fallback = fallback_writer(t);
    flags[0] = RDNA4DF_SLOT_REUSABLE; drain();
    CHECK("committed placeholder write blocks a deferred read of the same surface", copies == 0 && visible == 27);
    [fallback rdna4DisplayUncommit];
    reset(); t->value = 29; f = fixture(t); f->_tcommit = 1; [f rdna4DispDone:0];
    fallback = fallback_writer(t); [fallback rdna4DispSeq]; t->value = 99; [fallback rdna4DispDone:0];
    flags[0] = RDNA4DF_SLOT_REUSABLE; drain();
    CHECK("retired placeholder clear cannot leak through a deferred copy of older work", copies == 0 && visible == 27);
    t->value = 30; f = fixture(t); f->_tcommit = 3; [f rdna4DispDone:0]; drain();
    CHECK("later clean submission can copy the same surface", copies == 1 && visible == 30);
    reset(); t->value = 29;
    fallback = fallback_writer(t); [fallback rdna4DispSeq];
    f = fixture(t); f->_tcommit = 3; [f rdna4DispDone:0];
    [fallback rdna4DispSeq]; t->value = 99; [fallback rdna4DispDone:0];
    flags[0] = RDNA4DF_SLOT_REUSABLE; drain();
    CHECK("a later segment of an earlier placeholder command invalidates the deferred frame", copies == 0 && visible == 27);
    reset(); t->value = 29; f = fixture(t); f->_tcommit = 1; [f rdna4DispDone:0];
    RDNA4Texture *unrelated = [RDNA4Texture new]; unrelated->surfaceID = 3;
    fallback = fallback_writer(unrelated); [fallback rdna4DispSeq]; [fallback rdna4DispDone:0];
    flags[0] = RDNA4DF_SLOT_REUSABLE; drain();
    CHECK("placeholder on an unrelated surface leaves the good deferred frame eligible", copies == 1 && visible == 29);
    t->surfaceID = IOSurfaceGetID(t->surface);
    unrelated->surfaceID = IOSurfaceGetID(unrelated->surface);
    reset();
    unrelated->value = 99;
    fallback = submit_frame(nil, t, YES, YES, NO);
    f = submit_frame(t, unrelated, NO, YES, NO);
    [fallback rdna4DispDone:0]; [f rdna4DispDone:0];
    CHECK("a final pass submitted before its placeholder producer retires cannot present its pixels", visible == 27 && !RDNA4S.sm.inflight[0] && !RDNA4S.sm.gpu_fail);
    f = submit_frame(unrelated, t, NO, YES, YES);
    [f rdna4DispDone:0]; flags[0] = RDNA4DF_SLOT_REUSABLE; drain();
    CHECK("the second destination cannot publish an incomplete result by deferred copy", visible == 27 && copies == 0);
    t->value = 30;
    f = submit_frame(nil, t, NO, NO, NO);
    [f rdna4DispDone:0];
    CHECK("a partial rewrite cannot promote an incomplete surface", visible == 27);
    f = submit_frame(nil, t, NO, YES, NO);
    [f rdna4DispDone:0];
    CHECK("a clean full repaint allows presentation to resume", visible == 30);
    RDNA4Texture *independent = [RDNA4Texture new];
    independent->surfaceID = IOSurfaceGetID(independent->surface);
    independent->value = 31;
    f = submit_frame(t, independent, NO, YES, NO);
    [f rdna4DispDone:0];
    CHECK("clean producer pixels are eligible on a later final pass", visible == 31);
    reset();
    fallback = submit_frame(nil, t, YES, YES, NO); [fallback rdna4DispDone:0];
    Fixture *reader = [Fixture new];
    reader->_curSegment = [RDNA4CommandSegment new];
    [reader rdna4CBRead:t];
    [reader rdna4DispSourceWrites:reader->_curSegment];
    CHECK("a read-only segment records its incomplete input without touching scanout", reader->_displayIncomplete && visible == 27);
    Fixture *repaint = [Fixture new];
    repaint->_curSegment = [RDNA4CommandSegment new];
    [repaint rdna4NoteSurfaceOverwrite:t width:1 height:2 level:0];
    [repaint rdna4NoteHostWriteback:t];
    [repaint rdna4DispSourceWrites:repaint->_curSegment];
    CHECK("a partial attachment clear keeps the IOSurface incomplete", [RDNA4S.incompleteSources containsIndex:t->surfaceID]);
    repaint->_curSegment = [RDNA4CommandSegment new];
    [repaint rdna4NoteSurfaceOverwrite:t width:2 height:2 level:0];
    [repaint rdna4CBRead:t];
    [repaint rdna4NoteHostWriteback:t];
    [repaint rdna4DispSourceWrites:repaint->_curSegment];
    CHECK("an attachment read after a full clear sees new contents", !repaint->_displayIncomplete && ![RDNA4S.incompleteSources containsIndex:t->surfaceID]);
    reset();
    fallback = submit_frame(nil, t, YES, YES, NO); [fallback rdna4DispDone:0];
    RDNA4Texture *alias = [RDNA4Texture new];
    CFRelease(alias->surface);
    alias->surface = (IOSurfaceRef)CFRetain(t->surface);
    alias->surfaceID = t->surfaceID;
    repaint = [Fixture new];
    repaint->_curSegment = [RDNA4CommandSegment new];
    [repaint rdna4NoteSurfaceOverwrite:t width:2 height:2 level:0];
    [repaint rdna4CBRead:alias];
    [repaint rdna4NoteHostWriteback:t];
    [repaint rdna4DispSourceWrites:repaint->_curSegment];
    CHECK("a cleared image does not validate another wrapper of the same incomplete pages",
          repaint->_displayIncomplete && [RDNA4S.incompleteSources containsIndex:t->surfaceID]);
    reset();
    RDNA4Texture *protected = [RDNA4Texture new];
    protected->gpuOnly = YES;
    protected->surfaceID = IOSurfaceGetID(protected->surface);
    protected->value = 99;
    fallback = submit_frame(nil, protected, YES, YES, NO);
    f = submit_frame(protected, t, NO, YES, NO);
    [fallback rdna4DispDone:0]; [f rdna4DispDone:0];
    CHECK("fallback GPU-only image cannot feed a later mapped final frame", f->_displayIncomplete && visible == 27);
    CHECK("GPU-only rendering adds neither host copy nor host visibility counts",
          !fallback->_curSegment->hostWritebackCopies && !fallback->_curSegment->hostLinearWrites);
    f = submit_frame(nil, protected, NO, NO, NO);
    [f rdna4DispDone:0];
    CHECK("an incomplete GPU-only image cannot go directly to scanout after a partial rewrite", visible == 27);

    RDNA4Texture *protectedSibling = [RDNA4Texture new];
    CFRelease(protectedSibling->surface);
    protectedSibling->surface = (IOSurfaceRef)CFRetain(protected->surface);
    protectedSibling->surfaceID = protected->surfaceID;
    protectedSibling->gpuOnly = YES;
    protectedSibling->value = 31;
    f = submit_frame(nil, protectedSibling, NO, YES, NO);
    [f rdna4DispDone:0];
    CHECK("a separate GPU image sharing the surface ID can be presented", visible == 31);
    f = submit_frame(protected, t, NO, YES, NO);
    [f rdna4DispDone:0];
    CHECK("repainting a sibling GPU image does not clear the stale root", f->_displayIncomplete && visible == 31);
    protected->value = 32;
    f = submit_frame(nil, protected, NO, YES, NO);
    [f rdna4DispDone:0];
    CHECK("repainting the entire protected image makes it presentable again", visible == 32);

    // SkyLight ping-pong composite (compA <-> compB -> scanA/scanB without MTLLoadActionClear)
    // must stay suppressed during fallback and recover automatically once fallback is quiet.
    reset();
    RDNA4Texture *compA = t, *compB = unrelated;
    RDNA4Texture *scanA = [RDNA4Texture new], *scanB = [RDNA4Texture new];
    scanA->surfaceID = IOSurfaceGetID(scanA->surface);
    scanB->surfaceID = IOSurfaceGetID(scanB->surface);
    scanA->value = 41; scanB->value = 42;
    fallback = submit_frame(nil, compA, YES, NO, NO); [fallback rdna4DispDone:0];
    testClockNs += 5000000ull;
    f = submit_frame(compA, scanA, NO, NO, NO); [f rdna4DispDone:0];
    testClockNs += 16000000ull;
    f = submit_frame(compA, compB, NO, NO, NO); [f rdna4DispDone:0];
    f = submit_frame(compB, scanB, NO, NO, NO); [f rdna4DispDone:0];
    testClockNs += 16000000ull;
    f = submit_frame(compB, compA, NO, NO, NO); [f rdna4DispDone:0];
    f = submit_frame(compA, scanA, NO, NO, NO); [f rdna4DispDone:0];
    CHECK("ping-pong composite stays suppressed during active fallback window",
          visible == 27 && [RDNA4S.incompleteSources containsIndex:compA->surfaceID] &&
          [RDNA4S.incompleteSources containsIndex:compB->surfaceID]);
    testClockNs += RDNA4DF_INCOMPLETE_QUIET_NS;
    scanB->value = 43;
    f = submit_frame(compA, compB, NO, NO, NO); [f rdna4DispDone:0];
    f = submit_frame(compB, scanB, NO, NO, NO); [f rdna4DispDone:0];
    CHECK("ping-pong composite recovers without clear after fallback quiet window",
          visible == 43 && RDNA4S.incompleteSources.count == 0);

    // Continuous shader fallback (untranslatable shader on screen) hits the bounded cap
    // so the desktop does not freeze permanently, and resets once fallback stops.
    reset();
    scanA->value = 51;
    for (unsigned step = 0; step < 120; ++step) {
        fallback = submit_frame(nil, compA, YES, NO, NO); [fallback rdna4DispDone:0];
        f = submit_frame(compA, scanA, NO, NO, NO); [f rdna4DispDone:0];
        testClockNs += 16000000ull;
    }
    CHECK("continuous fallback stays suppressed before the suppression cap", visible == 27);
    testClockNs += 200000000ull; // past RDNA4DF_INCOMPLETE_MAX_NS
    scanA->value = 52;
    fallback = submit_frame(nil, compA, YES, NO, NO); [fallback rdna4DispDone:0];
    f = submit_frame(compA, scanA, NO, NO, NO); [f rdna4DispDone:0];
    CHECK("continuous fallback resumes presentation once the suppression cap is reached",
          visible == 52 && RDNA4S.incompleteSources.count == 0);
    testClockNs += 16000000ull;
    scanA->value = 53;
    fallback = submit_frame(nil, compA, YES, NO, NO); [fallback rdna4DispDone:0];
    f = submit_frame(compA, scanA, NO, NO, NO); [f rdna4DispDone:0];
    CHECK("continued fallback after cap keeps presenting without oscillating", visible == 53);
    testClockNs += RDNA4DF_INCOMPLETE_QUIET_NS;
    scanA->value = 54;
    f = submit_frame(compA, scanA, NO, NO, NO); [f rdna4DispDone:0];
    CHECK("quiet period after capped episode resets suppression for future misses",
          visible == 54 && RDNA4S.incompleteSinceNs == 0);
    scanA->value = 55;
    fallback = submit_frame(nil, compA, YES, NO, NO); [fallback rdna4DispDone:0];
    f = submit_frame(compA, scanA, NO, NO, NO); [f rdna4DispDone:0];
    CHECK("a new fallback episode suppresses again after reset", visible == 54);

    reset(); t->value = 29; f = fixture(t); [f rdna4DispDone:0];
    submit_nonframe_write(t, 99);
    flags[0] = RDNA4DF_SLOT_REUSABLE; drain();
    CHECK("non-presentable write prevents copying a replaced source", copies == 0 && visible == 27);
    reset(); t->value = 29; f = fixture(t); [f rdna4DispDone:0];
    beforeCopy = ^{ submit_nonframe_write(t, 99); };
    flags[0] = RDNA4DF_SLOT_REUSABLE; drain();
    CHECK("source replaced after slot reservation never presents", copies == 1 && visible == 27 && !RDNA4S.sm.inflight[0]);
    reset(); t->value = 29; f = fixture(t); [f rdna4DispDone:0];
    submit_nonframe_write(unrelated, 99);
    flags[0] = RDNA4DF_SLOT_REUSABLE; drain();
    CHECK("non-presentable write on another surface leaves deferred copy eligible", copies == 1 && visible == 29);
    reset(); t->value = 29; t->linear = YES; f = fixture(t); [f rdna4DispDone:0];
    Fixture *segmentedWriter = [Fixture new];
    RDNA4CommandSegment *earlierSegment = [RDNA4CommandSegment new];
    segmentedWriter->_curSegment = earlierSegment;
    [segmentedWriter rdna4NoteHostWriteback:unrelated];
    RDNA4CommandSegment *laterSegment = [RDNA4CommandSegment new];
    segmentedWriter->_curSegment = laterSegment;
    [segmentedWriter rdna4NoteHostWriteback:t];
    [segmentedWriter rdna4NoteHostWriteback:unrelated];
    [segmentedWriter rdna4NoteHostWriteback:t];
    [segmentedWriter rdna4DispSourceWrites:earlierSegment];
    CHECK("earlier segment only invalidates its own sources", rdna4df_late_current(&RDNA4S.sm, f->_dseq));
    CHECK("linear and repeated writes record unique sources and separate visibility counts",
          laterSegment->displaySourceWrites.count == 2 && laterSegment->hostWritebackCopies == 1 && laterSegment->hostLinearWrites == 2);
    [segmentedWriter rdna4DispSourceWrites:laterSegment]; t->value = 99;
    flags[0] = RDNA4DF_SLOT_REUSABLE; drain();
    CHECK("later segment invalidates its overwritten linear source", copies == 0 && visible == 27);
    t->linear = NO;
    reset();
    RDNA4S.sm.inflight[0] = 1; rdna4df_submit(&RDNA4S.sm, 0, t->surfaceID, 28); pixels[0] = 28;
    submit_nonframe_write(t, 99);
    rdna4s_complete_run(0, 0, 0, 28, t->surfaceID, NULL, 0, 0, 0);
    CHECK("non-presentable write leaves independent snapshot eligible", visible == 28);
    reset(); f = fixture(t); [f rdna4DispDone:VK_ERROR_UNKNOWN]; drain();
    CHECK("failed GPU work never queues or presents a deferred copy", copies == 0 && work.count == 0 && visible == 27);
    reset(); f = fixture(t); [f rdna4DispDone:0];
    Fixture *newer = fixture(t); t->value = 30; [newer rdna4DispDone:0]; flags[1] = RDNA4DF_SLOT_REUSABLE; drain();
    CHECK("newer completed update coalesces older queued source", copies == 1 && visible == 30 && RDNA4S.sm.last_seq == 30);
    reset(); f = fixture(t); [f rdna4DispDone:0]; RDNA4S.sm.last_seq = 31; flags[0] = RDNA4DF_SLOT_REUSABLE; drain();
    CHECK("late completion never regresses a newer displayed frame", copies == 0 && visible == 27);
    reset(); f = fixture(t); [f rdna4DispDone:0]; drain();
    CHECK("persistent slot exhaustion is bounded and releases source ownership", attempts == 3 && copies == 0 && IOSurfaceGetUseCount(t->surface) == use);
    reset(); f = fixture(t); [f rdna4DispDone:0]; failCopy = YES; flags[0] = RDNA4DF_SLOT_REUSABLE; drain();
    CHECK("copy failure releases its reservation without displaying it", visible == 27 && RDNA4S.sm.gpu_fail == 1 && !RDNA4S.sm.inflight[0]);
    reset(); f = fixture(t); [f rdna4DispDone:0]; hangCopy = YES; flags[0] = RDNA4DF_SLOT_REUSABLE; drain();
    CHECK("unresolved copy fails closed and retains its slot and source", released == 1 && RDNA4S.sm.state == RDNA4DF_OFF && RDNA4S.sm.inflight[0] && IOSurfaceGetUseCount(t->surface) == use + 1);
    quarantine = nil;
#endif
    printf("%s test-display-late (%u failures); mocked GPU and scanout\n", errors ? "FAIL" : "PASS", errors);
    return errors ? 1 : 0;
} }
