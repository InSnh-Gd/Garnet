// Production observer and atomic dump writer; pixel sources and present result are controlled.
#import <Foundation/Foundation.h>
#import <IOSurface/IOSurface.h>
#include <CommonCrypto/CommonDigest.h>
#include <stdatomic.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <fcntl.h>
#include <unistd.h>
#include "garnet_dispflip.h"
#include "garnet_plane.h"
#include "garnet_scanabi.h"
#define RDNA4LOG(...) ((void)0)
#define RDNA4S_FOURCC_BGRA 0x42475241u
typedef void *VkBuffer;
static struct { void *dev; } RDNA4R;
static BOOL scanMoved, scanFails, pendingOnce, fetchLagsOnce, replacementPending, latchChanges;
static uint64_t expectedPresent, wrongPresent;
static unsigned scanReads, readbacks;
static int captureStatus(void *device, struct rdna4n_scan_status *status) {
    (void)device;
    memset(status, 0, sizeof *status);
    status->acquired = 1;
    status->presents = wrongPresent ? wrongPresent : expectedPresent;
    const unsigned read = scanReads++;
    status->front_slot = pendingOnce && !read ? RDNA4N_SCAN_NO_SLOT : 2;
    status->pending_slot = pendingOnce && !read ? 2 : RDNA4N_SCAN_NO_SLOT;
    status->slot[2].mc = 0x8008000000ull;
    status->slot[2].latched_frame = latchChanges && read ? 13 : 12;
    status->plane_mc = status->earliest_mc = status->slot[2].mc;
    if (fetchLagsOnce && !read) status->earliest_mc -= 65536;
    if (replacementPending) {
        status->pending_slot = 1;
        status->plane_mc -= 65536;
    }
    if (scanMoved && read) status->earliest_mc += 65536;
    return scanFails ? -1 : 0;
}
static struct { pthread_mutex_t mu; uint32_t pw, ph, pitch, sid[3]; VkBuffer buf[3]; rdna4df_t sm;
    int (*fs)(void *, struct rdna4n_scan_status *); } RDNA4S = { .mu = PTHREAD_MUTEX_INITIALIZER };
static BOOL enabled, admitted = YES, copyFails, copyUnresolved, mutexAvailable;
static uint64_t clockNow = 1;
static NSString *directory, *presentFlag, *fullFlag;
#define RDNA4_PRESENT_CAPTURE_FLAG presentFlag.fileSystemRepresentation
#define RDNA4_PRESENT_FULL_FLAG fullFlag.fileSystemRepresentation
static NSMutableArray *work;
static BOOL rdna4_is_ws(void) { return admitted; }
static BOOL rdna4_capture_on(void) { return enabled; }
static uint64_t rdna4_now(void) { return clockNow; }
static int rdna4_dump_dir_ready(char *out, size_t cap) { return snprintf(out, cap, "%s", directory.fileSystemRepresentation) < (int)cap; }
static BOOL rdna4_capture_readback(VkBuffer slot, size_t row, size_t pitch, size_t rows, uint8_t *out, BOOL *unresolved) {
    readbacks++;
    mutexAvailable = pthread_mutex_trylock(&RDNA4S.mu) == 0;
    if (mutexAvailable) pthread_mutex_unlock(&RDNA4S.mu);
    *unresolved = copyUnresolved;
    if (copyUnresolved || copyFails) return NO;
    for (size_t y = 0; y < rows; y++) memcpy(out + y * row, (const uint8_t *)slot + y * pitch, row);
    return YES;
}
static void captureAsync(dispatch_queue_t queue, dispatch_block_t block) {
    (void)queue;
    [work addObject:[block copy]];
}
#define dispatch_async captureAsync
#include "dump-write.inc"
#define GARNET_PRESENT_CAPTURE_ONLY 1
#include "garnet_surface.m"
static unsigned failures;
#define CHECK(name, cond) do { BOOL ok = (cond); printf("%s %s\n", ok ? "PASS" : "FAIL", name); failures += !ok; } while (0)
static void drain(void) {
    while (work.count) { dispatch_block_t b = work.firstObject; [work removeObjectAtIndex:0]; b(); }
}
static NSArray *records(void) {
    NSMutableArray *found = [NSMutableArray array];
    for (NSString *name in [[NSFileManager defaultManager] contentsOfDirectoryAtPath:directory error:NULL])
        if ([name hasPrefix:@"present-"] && [name hasSuffix:@".json"])
            [found addObject:[directory stringByAppendingPathComponent:name]];
    return found;
}
static NSDictionary *record(uint64_t seq) {
    for (NSString *path in records()) {
        NSDictionary *m = [NSJSONSerialization JSONObjectWithData:[NSData dataWithContentsOfFile:path] options:0 error:NULL];
        if ([m[@"sequence"] unsignedLongLongValue] == seq) return m;
    }
    return nil;
}
static NSData *pixels(uint64_t seq) {
    NSString *name = record(seq)[@"raw_file"];
    if (!name) return nil;
    return [NSData dataWithContentsOfFile:[directory stringByAppendingPathComponent:name]];
}
static void capture(uint32_t sid, uint64_t seq, int rc) {
    clockNow += NSEC_PER_SEC;
    expectedPresent = seq + 100;
    pthread_mutex_lock(&RDNA4S.mu);
    rdna4_capture_present(0, 0, sid, seq, rc, seq + 100);
    pthread_mutex_unlock(&RDNA4S.mu);
}
int main(int argc, const char **argv) { @autoreleasepool {
    if (argc != 2) return 2;
    directory = [@(argv[1]) stringByAppendingFormat:@"/capture-fixture-%d", getpid()];
    [[NSFileManager defaultManager] createDirectoryAtPath:directory withIntermediateDirectories:YES attributes:nil error:NULL];
    presentFlag = [directory stringByAppendingPathComponent:@"present-enabled"];
    fullFlag = [directory stringByAppendingPathComponent:@"full-enabled"];
    work = [NSMutableArray array];
    const NSUInteger width = 96, height = 96, stride = 512;
    IOSurfaceRef surface = IOSurfaceCreate((__bridge CFDictionaryRef)@{
        (id)kIOSurfaceWidth:@(width), (id)kIOSurfaceHeight:@(height),
        (id)kIOSurfaceBytesPerElement:@4, (id)kIOSurfaceBytesPerRow:@(stride),
        (id)kIOSurfaceAllocSize:@(stride * height), (id)kIOSurfacePixelFormat:@0x42475241 });
    if (!surface) return 2;
    uint8_t *source = IOSurfaceGetBaseAddress(surface);
    NSMutableData *slot = [NSMutableData dataWithLength:stride * height];
    memset(source, 0x11, stride * height); memset(slot.mutableBytes, 0x11, slot.length);
    RDNA4S.pw = width; RDNA4S.ph = height; RDNA4S.pitch = stride;
    rdna4df_init(&RDNA4S.sm, 0);
    rdna4df_activate(&RDNA4S.sm, 3);
    RDNA4S.sid[0] = 2;
    RDNA4S.buf[0] = slot.mutableBytes;
    uint32_t sid = IOSurfaceGetID(surface);
    capture(sid, 1, 0); drain();
    CHECK("default off does not read or store scanout", !readbacks && records().count == 0);
    enabled = YES; capture(sid, 101, 0); drain();
    CHECK("surface diagnostics alone never request readback", !readbacks && records().count == 0);
    [NSData.data writeToFile:presentFlag atomically:YES];
    admitted = NO; capture(sid, 2, 0); admitted = YES;
    capture(sid, 3, -EBUSY); capture(sid, 4, -ENODEV); drain();
    CHECK("applications and rejected presents are not captured", !readbacks && records().count == 0);
    copyFails = YES; capture(sid, 5, 0); drain(); copyFails = NO;
    CHECK("failed readback releases the slot without publishing a record", !RDNA4S.sm.pin[0] && records().count == 0);
    capture(sid, 6, 0); capture(sid, 7, 0);
    CHECK("writer backpressure keeps at most one pixel record in flight", work.count == 1);
    const uint32_t available[3] = { RDNA4DF_SLOT_REUSABLE, RDNA4DF_SLOT_REUSABLE, 0 };
    CHECK("queued readback reserves its slot while another slot stays usable", rdna4df_pick(&RDNA4S.sm, available) == 1);
    rdna4df_release_slot(&RDNA4S.sm, 1);
    drain();
    const uint32_t onlyCaptured[3] = { RDNA4DF_SLOT_REUSABLE, 0, 0 };
    CHECK("readback runs outside the scanout mutex and returns the slot to the picker",
        mutexAvailable && rdna4df_pick(&RDNA4S.sm, onlyCaptured) == 0);
    rdna4df_release_slot(&RDNA4S.sm, 0);
    NSDictionary *m = record(6); NSData *p = pixels(6);
    BOOL exact = m && [m[@"source_valid"] boolValue] && [m[@"source_unchanged"] boolValue] &&
        [m[@"source_matches_slot"] boolValue] && [m[@"width"] unsignedLongValue] == width &&
        [m[@"captured_rows"] unsignedLongValue] == 64 && [m[@"bytes_per_row"] unsignedLongValue] == width * 4 &&
        [m[@"present_id"] unsignedLongLongValue] == 106 && p.length == width * 4 * 64 * 3;
    for (NSUInteger i = 0; i < p.length; i++) exact &= ((const uint8_t *)p.bytes)[i] == 0x11;
    CHECK("accepted output and source bytes match with padding removed", exact);
    capture(sid, 8, 0);
    memset(source, 0x22, stride * height); RDNA4S.sm.submit_hi = 9;
    drain(); m = record(8); p = pixels(8);
    const uint8_t *b = p.bytes;
    CHECK("later source updates preserve the actual older slot in the evidence", m &&
        ![m[@"source_matches_slot"] boolValue] && [m[@"latest_submission"] unsignedLongLongValue] == 9 &&
        b[0] == 0x22 && b[width * 4 * 64] == 0x11 && b[width * 4 * 64 * 2] == 0x22);
    for (uint64_t seq = 10; seq < 40; seq++) { memset(slot.mutableBytes, (int)seq, slot.length); capture(sid, seq, 0); drain(); }
    CHECK("rolling records retain the last sixteen frames rather than early boot only", records().count == 16 && record(39) && !record(10));
    capture(0, 40, 0); drain(); m = record(40);
    CHECK("unavailable IOSurface still captures the accepted slot without inventing source data", m &&
        ![m[@"source_valid"] boolValue] && pixels(40).length == width * 4 * 64);
    unsigned before = (unsigned)records().count;
    clockNow += NSEC_PER_SEC; rdna4_capture_present(1, 0, sid, 41, 0, 141);
    clockNow += NSEC_PER_SEC; rdna4_capture_present(0, -1, sid, 42, 0, 142);
    RDNA4S.pitch = 1; capture(sid, 43, 0); drain();
    CHECK("unsupported instance slot and geometry do not read outside their buffers", records().count == before && !record(41) && !record(42) && !record(43));
    RDNA4S.pitch = stride;
    unlink(presentFlag.fileSystemRepresentation);
    symlink("missing", presentFlag.fileSystemRepresentation);
    capture(sid, 44, 0); drain();
    CHECK("symlink opt-in is refused", !record(44));
    unlink(presentFlag.fileSystemRepresentation);
    mkdir(presentFlag.fileSystemRepresentation, 0700);
    capture(sid, 45, 0); drain();
    CHECK("directory opt-in is refused", !record(45));
    rmdir(presentFlag.fileSystemRepresentation);
    [NSData.data writeToFile:presentFlag atomically:YES];
    RDNA4S.fs = captureStatus;
    pendingOnce = YES; scanReads = 0;
    capture(sid, 60, 0); drain(); m = record(60);
    pendingOnce = NO;
    CHECK("top capture binds the accepted present to the observed fetch", m &&
        [m[@"scanout_verified"] boolValue] && scanReads == 3 &&
        [m[@"scanout_before_present"] unsignedLongLongValue] == 160 &&
        [m[@"scanout_after_present"] unsignedLongLongValue] == 160 && pixels(60).length == width * 4 * 64 * 3);
    wrongPresent = 160;
    capture(sid, 61, 0); drain(); m = record(61);
    CHECK("a matching address for a different present preserves pixels without verifying scanout", m &&
        ![m[@"scanout_verified"] boolValue] && [m[@"scanout_before_present"] unsignedLongLongValue] == 160 &&
        pixels(61).length == width * 4 * 64 * 3);
    wrongPresent = 0;
    const size_t planeWidth = 3840, planeHeight = 2160, pitch = 15360, planeBytes = pitch * planeHeight;
    IOSurfaceRef offsetSurface = IOSurfaceCreate((__bridge CFDictionaryRef)@{
        (id)kIOSurfaceWidth:@(planeWidth), (id)kIOSurfaceHeight:@(planeHeight),
        (id)kIOSurfaceBytesPerElement:@4, (id)kIOSurfaceBytesPerRow:@(pitch),
        (id)kIOSurfaceAllocSize:@(pitch + planeBytes), (id)kIOSurfacePixelFormat:@0x42475241,
        (id)kIOSurfacePlaneInfo:@[@{
            (id)kIOSurfacePlaneWidth:@(planeWidth), (id)kIOSurfacePlaneHeight:@(planeHeight),
            (id)kIOSurfacePlaneBytesPerElement:@4, (id)kIOSurfacePlaneBytesPerRow:@(pitch),
            (id)kIOSurfacePlaneOffset:@(pitch), (id)kIOSurfacePlaneSize:@(planeBytes),
        }],
    });
    if (!offsetSurface || IOSurfaceGetPlaneCount(offsetSurface) != 1 ||
        IOSurfaceLock(offsetSurface, 0, NULL) != kIOReturnSuccess) return 2;
    uint8_t *allocation = IOSurfaceGetBaseAddress(offsetSurface);
    uint8_t *plane = IOSurfaceGetBaseAddressOfPlane(offsetSurface, 0);
    if (!allocation || plane != allocation + pitch) return 2;
    NSMutableData *topSlot = [NSMutableData dataWithLength:pitch * 64];
    memset(allocation, 0xff, pitch);
    for (size_t row = 0; row < 64; row++) memset(plane + row * pitch, (int)row, pitch);
    memcpy(topSlot.mutableBytes, plane, topSlot.length);
    IOSurfaceUnlock(offsetSurface, 0, NULL);
    RDNA4S.pw = (uint32_t)planeWidth; RDNA4S.ph = (uint32_t)planeHeight; RDNA4S.pitch = (uint32_t)pitch;
    RDNA4S.buf[0] = topSlot.mutableBytes;
    capture(IOSurfaceGetID(offsetSurface), 63, 0); drain(); m = record(63); p = pixels(63);
    CHECK("top capture compares the scanout slot with plane zero, not the allocation prefix", m &&
        [m[@"scanout_verified"] boolValue] && [m[@"source_valid"] boolValue] &&
        [m[@"source_matches_slot"] boolValue] && [m[@"source_unchanged"] boolValue] &&
        p.length == 3 * topSlot.length && !memcmp(p.bytes, topSlot.bytes, topSlot.length) &&
        !memcmp((const uint8_t *)p.bytes + 2 * topSlot.length, topSlot.bytes, topSlot.length));
    CFRelease(offsetSurface);
    RDNA4S.pw = width; RDNA4S.ph = height; RDNA4S.pitch = stride;
    RDNA4S.buf[0] = slot.mutableBytes;
    wrongPresent = 160;
    [NSData.data writeToFile:fullFlag atomically:YES];
    clockNow += 20 * NSEC_PER_SEC;
    capture(sid, 62, 0); drain(); m = record(62);
    CHECK("full capture cannot verify a different present at the same address", m && ![m[@"scanout_verified"] boolValue]);
    wrongPresent = 0;
    memset(source, 0x55, stride * height);
    memset(slot.mutableBytes, 0x55, slot.length);
    ((uint8_t *)slot.mutableBytes)[(height - 1) * stride] = 0x66;
    clockNow += 20 * NSEC_PER_SEC;
    pendingOnce = YES; scanReads = 0; const unsigned beforeFull = readbacks;
    capture(sid, 46, 0);
    CHECK("accepted present returns before waiting for a latch or reading pixels", scanReads == 0 && readbacks == beforeFull);
    drain(); m = record(46); p = pixels(46);
    pendingOnce = NO;
    CHECK("full capture checks the last row and records hardware binding", m &&
        [m[@"captured_rows"] unsignedLongValue] == height &&
        [m[@"scanout_verified"] boolValue] && scanReads == 3 && ![m[@"source_matches_slot"] boolValue] &&
        p.length == width * height * 4 &&
        ((const uint8_t *)p.bytes)[(height - 1) * width * 4] == 0x66);
    fetchLagsOnce = YES; scanReads = 0; clockNow += 20 * NSEC_PER_SEC;
    capture(sid, 51, 0); drain(); m = record(51);
    CHECK("full capture waits for fetch to reach its already latched slot", m &&
        [m[@"scanout_verified"] boolValue] && scanReads == 3);
    fetchLagsOnce = NO; replacementPending = YES; scanReads = 0; clockNow += 20 * NSEC_PER_SEC;
    capture(sid, 52, 0); drain(); m = record(52);
    CHECK("a replacement pending on another slot ends latch verification", m &&
        ![m[@"scanout_verified"] boolValue] && scanReads == 1);
    replacementPending = NO;
    scanMoved = YES; scanReads = 0; clockNow += 20 * NSEC_PER_SEC;
    capture(sid, 47, 0); drain(); m = record(47);
    CHECK("a changed hardware surface cannot verify a full capture", m &&
        ![m[@"scanout_verified"] boolValue]);
    scanMoved = NO; scanFails = YES; clockNow += 20 * NSEC_PER_SEC;
    capture(sid, 48, 0); drain(); m = record(48);
    CHECK("failed hardware status cannot verify a full capture", m &&
        ![m[@"scanout_verified"] boolValue]);
    scanFails = NO;
    latchChanges = YES; scanReads = 0; clockNow += 20 * NSEC_PER_SEC;
    capture(sid, 50, 0); drain(); m = record(50);
    CHECK("a different latch on the same slot cannot verify a full capture", m && ![m[@"scanout_verified"] boolValue]);
    latchChanges = NO;
    RDNA4S.pw = 3840; RDNA4S.ph = 2160; RDNA4S.pitch = 15616;
    NSMutableData *desktop = [NSMutableData dataWithLength:15616u * 2160u];
    memset(desktop.mutableBytes, 0x37, desktop.length);
    ((uint8_t *)desktop.mutableBytes)[2159u * 15616u] = 0x91;
    RDNA4S.buf[0] = desktop.mutableBytes;
    clockNow += 20 * NSEC_PER_SEC;
    capture(0, 49, 0); drain(); m = record(49); p = pixels(49);
    CHECK("4K full capture includes the Dock rows and removes row padding", m &&
        [m[@"full_frame"] boolValue] && [m[@"scanout_verified"] boolValue] &&
        [m[@"captured_rows"] unsignedLongValue] == 2160 && p.length == 33177600 &&
        ((const uint8_t *)p.bytes)[2159u * 15360u] == 0x91 &&
        ((const uint8_t *)p.bytes)[33177599] == 0x37);
    CHECK("full metadata binds the entire payload to the boot and hardware latch", m &&
        [m[@"capture_mode"] isEqual:@"full"] && [[NSUUID alloc] initWithUUIDString:m[@"boot_uuid"]] &&
        m[@"full_frame"] == (__bridge id)kCFBooleanTrue &&
        [m[@"scanout_latched_frame"] unsignedLongLongValue] == 12 &&
        [m[@"slot_sha256"] isEqual:rdna4_sha256hex(p)]);
    RDNA4S.pitch = 65536;
    clockNow += 20 * NSEC_PER_SEC;
    const unsigned beforeOversize = readbacks;
    capture(0, 51, 0); drain();
    CHECK("a padded full frame over the budget is refused before GPU access",
        readbacks == beforeOversize && !RDNA4S.sm.pin[0] && !record(51));
    RDNA4S.pitch = 15616;
    copyUnresolved = YES;
    clockNow += 20 * NSEC_PER_SEC;
    capture(0, 52, 0); drain();
    copyUnresolved = NO;
    clockNow += 20 * NSEC_PER_SEC;
    capture(0, 53, 0); drain();
    CHECK("an unresolved GPU read retains the slot and stops further captures",
        RDNA4S.sm.pin[0] == 1 && readbacks == beforeOversize + 1 && !record(52) && !record(53));
    CFRelease(surface);
    printf("PRESENT_CAPTURE_%s failures=%u; simulated pixels, no physical scanout\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
} }
