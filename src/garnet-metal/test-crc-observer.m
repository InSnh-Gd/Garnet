// Extracted production completion observer: noslot must work without mapping VRAM.
#import <Foundation/Foundation.h>
#import <IOSurface/IOSurface.h>
#include <pthread.h>
#include <stdatomic.h>
#include "garnet_dispflip.h"
#include "garnet_crc.h"
static void fixtureLog(const char *format, ...) { (void)format; }
#define RDNA4LOG(...) fixtureLog(__VA_ARGS__)
#define RDNA4LOG_KEY(...) fixtureLog(__VA_ARGS__)
#define RDNA4LOG_WARN(...) fixtureLog(__VA_ARGS__)
#define RDNA4LOG_ERR(...) fixtureLog(__VA_ARGS__)
#define VK_SUCCESS 0
#define VK_WHOLE_SIZE (~0ull)
typedef int VkResult;
static struct { void *dev; } RDNA4R;
static struct { pthread_mutex_t mu; uint32_t pitch, ph; void *map[3], *mem[3]; } RDNA4S = { .mu = PTHREAD_MUTEX_INITIALIZER };
static struct { pthread_mutex_t mu; rdna4crc_stats_t st; _Atomic uint32_t step; _Atomic int noslot; int mapfail; struct { uint32_t sid; uint64_t seq; } surf[8]; } RDNA4C = { .mu = PTHREAD_MUTEX_INITIALIZER };
static unsigned maps, failures;
static BOOL mapFails;
static uint64_t rdna4_now(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
static VkResult vkMapMemory(void *dev, void *mem, uint64_t off, uint64_t size, unsigned flags, void **out) {
    (void)dev; (void)off; (void)size; (void)flags; maps++;
    *out = mapFails ? NULL : mem; return mapFails ? -1 : 0;
}
@interface RDNA4Texture : NSObject { @public IOSurfaceRef surface; }
- (const uint8_t *)rdna4IBase;
- (IOSurfaceRef)rdna4IOSRef;
@end
@implementation RDNA4Texture
- (const uint8_t *)rdna4IBase { return IOSurfaceGetBaseAddress(surface); }
- (IOSurfaceRef)rdna4IOSRef { return surface; }
@end
#include "crc-record.inc"
#include "crc-check.inc"
#define CHECK(name, cond) do { BOOL ok = (cond); printf("%s %s\n", ok ? "PASS" : "FAIL", name); failures += !ok; } while (0)
static void release(rdna4crc_rec_t *r) { if (r) { CFRelease(r->ios); free(r); } }
int main(void) { @autoreleasepool {
    RDNA4Texture *t = [RDNA4Texture new];
    t->surface = IOSurfaceCreate((__bridge CFDictionaryRef)@{
        (id)kIOSurfaceWidth:@16, (id)kIOSurfaceHeight:@8, (id)kIOSurfaceBytesPerElement:@4 });
    if (!t->surface) return 2;
    RDNA4S.pitch = (uint32_t)IOSurfaceGetBytesPerRow(t->surface); RDNA4S.ph = 8;
    NSMutableData *pixels = [NSMutableData dataWithLength:RDNA4S.pitch * RDNA4S.ph];
    memset(IOSurfaceGetBaseAddress(t->surface), 0x31, pixels.length); memset(pixels.mutableBytes, 0x31, pixels.length);
    RDNA4S.mem[0] = pixels.mutableBytes; atomic_store(&RDNA4C.step, 1);
    mapFails = YES; atomic_store(&RDNA4C.noslot, 1);
    rdna4crc_rec_t *r = rdna4s_crc_check(t, 0, 1);
    CHECK("noslot checks the source even when VRAM mapping is unavailable", r && maps == 0 && RDNA4C.st.checked == 1 && RDNA4C.st.slot_n == 0);
    release(r);
    mapFails = NO; atomic_store(&RDNA4C.noslot, 0);
    r = rdna4s_crc_check(t, 0, 2);
    CHECK("slot mode compares both memories after its first successful mapping", r && RDNA4C.st.slot_n == 1 && RDNA4C.st.slot_ne_src == 0);
    release(r); unsigned priorMaps = maps;
    ((uint8_t *)pixels.mutableBytes)[RDNA4S.pitch] ^= 1;
    r = rdna4s_crc_check(t, 0, 3);
    CHECK("top-row slot corruption is detected and the existing mapping is reused", r && maps == priorMaps && RDNA4C.st.slot_ne_src == 1);
    release(r); CFRelease(t->surface);
    printf("CRC_OBSERVER_%s failures=%u; simulated pixels\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
} }
