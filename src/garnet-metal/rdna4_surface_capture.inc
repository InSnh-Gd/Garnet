// Opt-in snapshots of compositor inputs after GPU completion, before their use counts are released.
#define RDNA4_CAPTURE_FLAG "/private/tmp/rdna4m-surfaces"
static BOOL rdna4_capture_on(void) {
    if (!rdna4_is_ws() && !rdna4_allow()) return NO;
    static _Atomic uint64_t next;
    static _Atomic int enabled;
    uint64_t now = rdna4_now(), due = atomic_load(&next);
    if (now >= due && atomic_compare_exchange_strong(&next, &due, now + NSEC_PER_SEC)) {
        struct stat st;
        atomic_store(&enabled, lstat(RDNA4_CAPTURE_FLAG, &st) == 0 && S_ISREG(st.st_mode) &&
            (st.st_uid == 0 || st.st_uid == geteuid()));
    }
    return atomic_load(&enabled) != 0;
}
static void rdna4_capture_surfaces(NSArray *textures, uint64_t cbid) {
    if (!rdna4_capture_on()) return;
    const uint64_t now = rdna4_now();
    static NSLock *lock;
    static NSMutableDictionary *seen;
    static dispatch_queue_t writer;
    static uint64_t used;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        lock = [NSLock new]; seen = [NSMutableDictionary dictionary];
        writer = dispatch_queue_create("org.garnet.surface-capture", DISPATCH_QUEUE_SERIAL);
    });
    [lock lock];
    for (RDNA4Texture *view in textures) {
        RDNA4Texture *t = [view rootResource];
        IOSurfaceRef surface = [t rdna4IOSRef];
        if (!surface || [t rdna4IsDisp] || ![t rdna4IBase]) continue;
        size_t w = t.width, h = t.height, row = [t rdna4IOSBytesPerRow], bpp = [t bytesPerPixel];
        if (!w || !h || !bpp || w > SIZE_MAX / bpp || row < w * bpp ||
            h - 1 > (SIZE_MAX - w * bpp) / row) continue;
        size_t bytes = (h - 1) * row + w * bpp;
        if (bytes > 12u * 1024u * 1024u || used + bytes > 64u * 1024u * 1024u) continue;
        NSString *key = [NSString stringWithFormat:@"%u-%lu-%zux%zu-%lu", IOSurfaceGetID(surface),
            (unsigned long)t.pixelFormat, w, h, (unsigned long)t.iosurfacePlane];
        NSArray *prior = seen[key];
        if (!prior && seen.count >= 128) continue;
        unsigned ordinal = prior ? [prior[0] unsignedIntValue] : 0;
        if (ordinal >= 2 || (prior && now - [prior[1] unsignedLongLongValue] < 3 * NSEC_PER_SEC)) continue;
        char directory[1024];
        if (!rdna4_dump_dir_ready(directory, sizeof directory)) continue;
        uint32_t before = IOSurfaceGetSeed(surface);
        NSData *pixels = [NSData dataWithBytes:[t rdna4IBase] length:bytes];
        uint32_t after = IOSurfaceGetSeed(surface);
        if (!pixels) continue;
        NSDictionary *metadata = @{ @"surface": @(IOSurfaceGetID(surface)), @"pixel_format": @(t.pixelFormat),
            @"iosurface_format": @(IOSurfaceGetPixelFormat(surface)), @"width": @(w), @"height": @(h),
            @"bytes_per_row": @(row), @"bytes_per_pixel": @(bpp), @"plane": @(t.iosurfacePlane),
            @"bytes": @(bytes), @"command_buffer": @(cbid), @"time_ns": @(now),
            @"seed_before": @(before), @"seed_after": @(after), @"linear": @([t rdna4IOSLinear]) };
        NSData *json = [NSJSONSerialization dataWithJSONObject:metadata options:0 error:NULL];
        if (!json) continue;
        seen[key] = @[@(ordinal + 1), @(now)]; used += bytes;
        NSString *stem = [NSString stringWithFormat:@"%s/surface-%d-%@-%u", directory, getpid(), key, ordinal];
        dispatch_async(writer, ^{
            @autoreleasepool {
                BOOL okay = rdna4_dump_write([[stem stringByAppendingString:@".raw"] fileSystemRepresentation], pixels) &&
                    rdna4_dump_write([[stem stringByAppendingString:@".json"] fileSystemRepresentation], json);
                RDNA4LOG("surface capture: %s bytes=%lu status=%s seed=%u/%u", stem.UTF8String,
                    (unsigned long)pixels.length, okay ? "saved" : "failed", before, after);
            }
        });
    }
    [lock unlock];
}
