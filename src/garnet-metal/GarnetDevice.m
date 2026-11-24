// GarnetDevice.m - Metal driver bundle for the RDNA4 native stack (milestone #9, step 9c/9d).
// Spec: an internal design note section 4 and 5 (rows 9c/9d/9e); RE: NATIVE-S3-RE-KERNEL.md sections 1, 3.
//
// Superclass binding: MTLIOAccelDevice is an exported ObjC class of Metal.framework (the SDK's Metal.tbd lists
// it under objc-classes for x86_64-macos, and pvm.x86 - AppleParavirtGPUMetal - links _OBJC_CLASS_$_MTLIOAccelDevice
// the same way).  So the class is declared here WITHOUT ivars (non-fragile ABI: the subclass adds none, the real
// layout comes from Metal at load time) and linked at build time with -framework Metal.  No runtime class pair.
//
// Build-time switches (see build.sh):
//   RDNA4_LAZY=1   supportLazyInitialization -> YES (9c/9d, default).  RDNA4_LAZY=0 for 9e (base returns NO: CONFIRMED,
//                -[MTLIOAccelDevice supportLazyInitialization] at 0x7ff80f5a9cd3 is `xor eax,eax; ret`).
//   RDNA4_9D=1     the 9d feature/limit queries (default).  RDNA4_9D=0 leaves them to the MTLIOAccelDevice base.
#import <Foundation/Foundation.h>
#include <stdatomic.h>
#import <Metal/Metal.h>
#import <os/log.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <sys/event.h>
#include <crt_externs.h>
#include <CommonCrypto/CommonDigest.h>
#include <dispatch/dispatch.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#import <IOKit/IOKitLib.h>
#include <IOSurface/IOSurfaceRef.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include "rdna4_native_blit.h"
#include "rdna4_indirect_compute.h"
#include "rdna4_luma_workgroup.h"
#include "rdna4_heapalloc.h"
#include "rdna4_heap_backing.h"
#include "rdna4_devcaps.h"
#include "rdna4_system_profile.h"
#include "rdna4_gather_caps.h"
#include "rdna4_buffer_ranges.h"
#include "rdna4_buffer_registry.h"
#include "rdna4_argument_scan.h"
#include "rdna4_argument_scan.inc"
#include "rdna4_plane.h"
#include "rdna4_pitch.h"
#include "rdna4_texture_transfer.h"
#include "rdna4_fallback_spv.h"
#include "rdna4_hotswap.h"
#include "rdna4_dumpacl.h"
#include "rdna4_scanabi.h"    // S5.2a: scanout ABI structs (verbatim copy of the Mesa header)
#include "rdna4_dispflip.h"   // S5.2a: the pure D-copy state machine
#include "rdna4_display_commits.h"
#include "rdna4_m6x.h"        // bundle 13 (M6 Stage 1b): the monitor B's scanout decisions (instance 2: routing, enable, pool budget, keep-alive tick, re-copy)
#include "rdna4_m6route.h"    // bundle 11 (M6 Stage 1a): which display an IOSurface belongs to (the kernel's IOSurface ID -> instance table; host test: test-m6route.c)
#include "rdna4_texdesc.h"    // bundle 9 (app crash study item 2): descriptor -> Vulkan image mapping incl. cube and 2D array (host tests: test-texdesc.c, test-vkimage.c)
#include "rdna4_depth.h"      // bundle 10: depth/stencil formats and pipeline state, MSAA, multisample resolve (host tests: test-depth.c, test-vkimage.c)
#include "rdna4_cienv.h"      // bundle 9 (app crash study item 1): CI_USE_MTL_DAG_FOR_CIKL_SRC=0 unless the process already set it (host test: test-cienv.c)
#include "rdna4_xlate.h"      // bundle 14: in-process shader translation for admitted applications (NATIVE-S8-INPROC.md; host tests: test-xlate.c, test-xlate-corpus.sh)
#include "rdna4_gate.h"       // GPU-apps G4 (kext 0.0.640): the pure load policy outside WindowServer - fail-closed gates, the kernel probe, the per-process cache path (host test: test-gate.c)
#include "rdna4_crc.h"        // native #12: opt-in per-frame CRC diagnostic (row sampling, accounting; host test: test-crc.c)
#include "rdna4_cblog.h"      // native #12 Stage 0b: pure cross-queue RAW-inversion bookkeeping (host test: test-cblog.c)
#include "rdna4_t1.h"        // native #12: T1 timing histograms + T2 pipeline-cache file format (host test: test-t1.c)
#include "rdna4_impcache.h"  // P4 import cache + classify cache decisions; build 16: F2 default-on, F1 fallback decision, failure-injection hook (host test: test-impcache.c)
#include "rdna4_ledger.h"    // build 16 (P5): live-import ledger (Step 0) + the F3 use-count policy (host test: test-ledger.c)
#include "rdna4_ioalias.h"   // build 16 (P1): per-command-buffer aliasing of IOSurface wrappers (host test: test-ioalias.c)
#include "rdna4_census.h"    // build 18: the unimplemented-selector census (+load compares our classes with the running system's Metal protocols; logged once for a process that gets a device)
#include "rdna4_occ.h"       // build 18 (P2): occlusion queries - setVisibilityResultMode:offset: over Vulkan occlusion queries, the measured Metal semantics, the fail-safe (host test: test-occ.c)
#include "rdna4_intfmt.h"    // bundle 19 (missing menus): integer colour-format rules - the integer clear conversion measured on Apple's Metal, the fallback write mask (host test: test-intfmt.c, test-intclear-semantics.m)
#include "rdna4_pool.h"      // P1 memory pooling decisions (fence-gated deferred frees, 4 MiB slabs, recycle cache; host test: test-pool.c)
#include "rdna4_nocopy.h"
#include "rdna4_argument_limit_snapshot.h"
#include "rdna4_argument_limit_snapshot.inc"
#include "rdna4_drawcache.h" // P5b per-draw redundancy: descriptor-set signatures, last-pipeline caches, render-pass cache (host test: test-drawcache.c)
#include "rdna4_gl_surface_pure.h" // standalone GL-on-Metal surface table (RX4Darwin 1a034e6a, BSD 3-Clause)
#define RDNA4S_TRACE(...) ((void)0)
#include "rdna4_stitch_reflection.h" // AIRR -> MTLPSBIN stitching reflection
#undef RDNA4S_TRACE

#ifndef RDNA4_LAZY
#define RDNA4_LAZY 1
#endif
#ifndef RDNA4_9D
#define RDNA4_9D 1
#endif

// Forward declaration, deliberately without ivars.
@interface MTLIOAccelDevice : NSObject
- (instancetype)initWithAcceleratorPort:(uint32_t)port;
- (void)lazyInitialize;
- (id)compiler;
@end

// Metal's generic resource base (exported, F8). Declared without ivars for the same reason.
@interface _MTLResource : NSObject
@end

// Metal's generic heap base (exported; Apple-silicon chain AGXG16GFamilyHeap <- IOGPUMetalHeap <- _MTLHeap <- _MTLAllocation <- _MTLObjectWithLabel). No ivars declared.
@interface _MTLHeap : NSObject
@end

// Metal's generic queue / command buffer / encoder / pipeline bases (exported, F8). No ivars declared.
@interface _MTLCommandQueue : NSObject
- (instancetype)initWithDevice:(id)dev descriptor:(id)desc;
- (void)commandBufferDidComplete:(id)cb startTime:(uint64_t)st completionTime:(uint64_t)ct error:(NSError *)err;
@end
@interface _MTLCommandBuffer : NSObject
- (instancetype)initWithQueue:(id)q retainedReferences:(BOOL)r;
- (void)didScheduleWithStartTime:(uint64_t)st endTime:(uint64_t)et error:(NSError *)err;
- (void)setCurrentCommandEncoder:(id)e;
- (void)commit;
- (BOOL)commitAndWaitUntilSubmitted;
- (void)waitUntilScheduled;
- (void)waitUntilCompleted;
- (MTLCommandBufferStatus)status;
@end
@interface _MTLCommandEncoder : NSObject
- (instancetype)initWithCommandBuffer:(id)cb;
- (void)endEncoding;
@end
@interface _MTLRenderPipelineState : NSObject
@end

// Not an Apple GPU (0x1000013) nor any CPU type; no shipped metallib slice matches it (test-binary-archive checks).
#define RDNA4_TARGET_ARCH_CPUTYPE 0x01000044u
#define RDNA4_TARGET_ARCH_SUBTYPE 0x00000004u
typedef struct { uint16_t major; uint8_t minor; uint8_t update; } RDNA4TargetArchVersion;
_Static_assert(sizeof(RDNA4TargetArchVersion) == 4, "Metal target version ABI");

@interface GarnetDevice : MTLIOAccelDevice {
    NSString *_rdna4Model;
    rdna4dc_native_memory _rdna4NativeMemory;
    uint32_t _rdna4AcceleratorPort;
    uint64_t _rdna4RegistryID;
}
- (rdna4dc_caps)rdna4MemoryCaps;
- (rdna4_gather_caps)rdna4GatherCaps;
@end

// The system's AIRR reader preserves Metal binding names, types and struct layouts.
// These private initializers use an eight-byte flags aggregate on the target OS.
typedef struct { uint64_t bits; } RDNA4ReflectionFlags;
@interface NSObject (RDNA4ReflectionConstruction)
- (NSData *)newSerializedDescriptor;
- (instancetype)initWithSerializedData:(NSData *)data serializedStageInputDescriptor:(NSData *)descriptor device:(id)device options:(NSUInteger)options flags:(RDNA4ReflectionFlags)flags;
- (instancetype)initWithVertexData:(NSData *)vertex fragmentData:(NSData *)fragment serializedVertexDescriptor:(NSData *)descriptor device:(id)device options:(NSUInteger)options flags:(RDNA4ReflectionFlags)flags;
@end

static void rdna4_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void rdna4_log(const char *fmt, ...) {
    char buf[2048]; va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    os_log(OS_LOG_DEFAULT, "GarnetMetal: %{public}s", buf);
    fprintf(stderr, "GarnetMetal: %s\n", buf);
}
#define RDNA4LOG(fmt, ...) rdna4_log(fmt, ##__VA_ARGS__)
// #12 R5: per-command-buffer / per-encoder lines are rate limited per call site: the first 8, then every 1024th (errors and WARN lines stay RDNA4LOG).
#define RDNA4LOGR(fmt, ...) do { static _Atomic uint64_t n_; uint64_t k_ = atomic_fetch_add(&n_, 1); if (k_ < 8 || (k_ & 1023) == 0) rdna4_log(fmt, ##__VA_ARGS__); } while (0)

// ---------------------------------------------------------------------------------------------------------------
// Load policy (NATIVE-S4-M11 11b, L1-L3). Every Metal-enumerating process loads this bundle; only WindowServer, a root
// tool that opts in with RDNA4M_ALLOW=1, or (GPU-apps G4, kext 0.0.640) a process the KERNEL admits as an allow-listed application (decided by a
// successful RDNA4N open + QueryInfo, never by name; rdna4_gate.h) may get a device. All test hooks are honoured only with RDNA4M_ALLOW=1 as root.
// Outside WindowServer the safety gates below FAIL CLOSED (a gate that cannot be read declines): see rdna4_gate.h.
//   /private/tmp/rdna4m-off              exists -> decline (L1 kill file); outside WindowServer also when the stat is denied (anything but ENOENT / ENOTDIR)
//   /private/tmp/rdna4m-starts           one epoch-seconds line per WindowServer init; >= 3 within 300 s -> decline (L2)
//   nub property "Garnet,Ready" = 0    -> decline (L3); WindowServer: absent (old kexts) -> proceed; elsewhere it must be present and 1
//   nub property "Garnet,AutoDisarmed" = 1 -> decline (#12); absent -> ignored (the kext does not publish it today)
//   /private/tmp/rdna4m-headless-no      exists -> isHeadless NO (default YES); read once per process
//   /private/tmp/rdna4m-noplanes         exists -> P2: multi-plane IOSurfaces refused as before (default: plane p of a 2/3-plane surface is accepted); read once per process
//   /private/tmp/rdna4m-noflip           exists -> S5.2a D-copy present OFF for the process (the kernel v1 copy continues); read once per process
//   RDNA4M_TEST_IGNORE_KILL=1            (root + RDNA4M_ALLOW=1, NOT WindowServer) -> rdna4m-off is IGNORED in that process only (S5.2b test bypass)
//   RDNA4M_TEST_FALLBACK_AS_WS=1         (root + RDNA4M_ALLOW=1) -> a spvcache miss gives a FALLBACK pipeline as in WindowServer (hot-swap oracle: mtlprobe hotswap)
//   RDNA4M_TEST_HIDE_BUNDLE_SPV=1        (root + RDNA4M_ALLOW=1) -> the bundle's Resources/spvcache is NOT consulted (only the side directory)
//   RDNA4M_TEST_SIDE_DIR=<dir>           (root + RDNA4M_ALLOW=1) -> replaces the side spvcache directory /private/var/tmp/rdna4m-spv
//   RDNA4M_TEST_DISPFLIP=1               (root + RDNA4M_ALLOW=1) -> a surface may be accepted as a display surface without the CoreDisplay backtrace signal (mtlprobe dispflip sets it)
// ---------------------------------------------------------------------------------------------------------------
#define RDNA4_KILL_FILE      "/private/tmp/rdna4m-off"
#define RDNA4_STARTS_FILE    "/private/tmp/rdna4m-starts"
#define RDNA4_OK_FILE        "/private/tmp/rdna4m-ok"
#define RDNA4_HEADLESS_NO    "/private/tmp/rdna4m-headless-no"

static BOOL rdna4_is_ws(void) {
    static int v = -1;
    if (v < 0) { const char *p = getprogname(); v = (p && !strcmp(p, "WindowServer")) ? 1 : 0; }
    return v == 1;
}
static BOOL rdna4_allow(void) {
    const char *e = getenv("RDNA4M_ALLOW");
    return e && !strcmp(e, "1") && geteuid() == 0;
}
static BOOL rdna4_force_fallback(void) {
    const char *e = getenv("RDNA4M_FORCE_FALLBACK");
    return rdna4_allow() && e && !strcmp(e, "1");
}
// Test hook (root + RDNA4M_ALLOW=1): this process takes the WindowServer branch of the fallback rule (a spvcache miss yields a fallback pipeline,
// not an error) while the lookup stays real, so the hot-swap can be exercised from mtlprobe.
static BOOL rdna4_test_fb_as_ws(void) {
    const char *e = getenv("RDNA4M_TEST_FALLBACK_AS_WS");
    return rdna4_allow() && e && !strcmp(e, "1");
}
// GPU-apps G4: 1 once the kernel admitted this (non-WindowServer, non-RDNA4M_ALLOW) process as an allow-listed application (rdna4_admit's probe succeeded).
static _Atomic int rdna4_app_admitted;
static BOOL rdna4_fallback_ok(void) { return rdna4g_fallback_ok(rdna4_is_ws(), rdna4_force_fallback(), rdna4_test_fb_as_ws(), atomic_load(&rdna4_app_admitted)) ? YES : NO; }

// Crash counter (#12 R3): counts only ABNORMAL ends. One line per instance start: "<epoch> <pid>". An instance that committed a
// first successful command buffer appends "<pid> <start epoch> <now>" to RDNA4_OK_FILE (rdna4_mark_clean) and no longer counts: normal
// logouts, logins and restarts do not trip the rule; >= 3 instances within 300 s that never got clean still decline.
// Old-format lines (no pid) count as before. Paths are RDNA4_STARTS_FILE / RDNA4_OK_FILE, or (test hook, needs RDNA4M_ALLOW)
// RDNA4M_TEST_COUNTER=<path> (ok file <path>.ok), which also enables the counter outside WindowServer.
static char rdna4_ok_path[1100]; static long long rdna4_start_epoch; static _Atomic int rdna4_counter_on, rdna4_clean_done;
static BOOL rdna4_counter_tripped(int *countOut) {
    const char *path = RDNA4_STARTS_FILE;
    const char *tp = getenv("RDNA4M_TEST_COUNTER");
    if (rdna4_allow() && tp && tp[0]) { path = tp; snprintf(rdna4_ok_path, sizeof rdna4_ok_path, "%s.ok", tp); }
    else if (!rdna4_is_ws()) return NO;
    else snprintf(rdna4_ok_path, sizeof rdna4_ok_path, "%s", RDNA4_OK_FILE);
    time_t now = time(NULL); int count = 1, cleaned = 0;
    NSMutableSet *clean = [NSMutableSet set];
    FILE *f = fopen(rdna4_ok_path, "r");
    if (f) { char line[96]; while (fgets(line, sizeof line, f)) { long long pid = 0, st = 0; if (sscanf(line, "%lld %lld", &pid, &st) == 2) [clean addObject:[NSString stringWithFormat:@"%lld/%lld", pid, st]]; } fclose(f); }
    f = fopen(path, "r");
    if (f) { char line[64]; while (fgets(line, sizeof line, f)) {
        long long t = 0, pid = 0; int n = sscanf(line, "%lld %lld", &t, &pid);
        if (n < 1 || t <= 0 || now - t < 0 || now - t > 300) continue;
        if (n == 2 && [clean containsObject:[NSString stringWithFormat:@"%lld/%lld", pid, t]]) { cleaned++; continue; }
        count++; }
        fclose(f); }
    rdna4_start_epoch = (long long)now;
    int fd = open(path, O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (fd >= 0) { char b[48]; int n = snprintf(b, sizeof b, "%lld %d\n", (long long)now, (int)getpid()); (void)!write(fd, b, (size_t)n); close(fd); }
    else RDNA4LOG("counter: cannot append to %s (errno %d)", path, errno);
    atomic_store(&rdna4_counter_on, 1);
    RDNA4LOG("counter: %d abnormal start(s) in the last 300 s including this one (%d earlier instance(s) reached clean and are not counted)", count, cleaned);
    if (countOut) *countOut = count;
    return count >= 3;
}
// Called after the first successfully completed command buffer of an admitted instance that is counted.
static void rdna4_mark_clean(void) {
    if (!atomic_load(&rdna4_counter_on) || atomic_exchange(&rdna4_clean_done, 1)) return;
    int fd = open(rdna4_ok_path, O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (fd < 0) { RDNA4LOG("clean marker: cannot append to %s (errno %d)", rdna4_ok_path, errno); return; }
    char b[96]; int n = snprintf(b, sizeof b, "%d %lld %lld\n", (int)getpid(), rdna4_start_epoch, (long long)time(NULL)); (void)!write(fd, b, (size_t)n); close(fd);
    RDNA4LOG("clean marker written to %s (first completed command buffer)", rdna4_ok_path);
}

// RDNA4G_FLAG_ABSENT (-1) = property absent (or filtered by a sandbox: the registry cannot tell), RDNA4G_FLAG_NONUB (-2) = the nub could not be reached, 0 = false/zero, 1 = true.
// Reads a numeric/boolean property from the accelerator's parent (the nub). WindowServer treats -1 and -2 alike (proceed); everything else declines on them (rdna4_gate.h).
static int rdna4_nub_flag(uint32_t port, CFStringRef name) {
    io_registry_entry_t parent = 0;
    if (IORegistryEntryGetParentEntry((io_registry_entry_t)port, kIOServicePlane, &parent) != KERN_SUCCESS || !parent) return RDNA4G_FLAG_NONUB;
    CFTypeRef v = IORegistryEntryCreateCFProperty(parent, name, kCFAllocatorDefault, 0);
    IOObjectRelease(parent);
    if (!v) return RDNA4G_FLAG_ABSENT;
    int r = RDNA4G_FLAG_ABSENT;
    if (CFGetTypeID(v) == CFNumberGetTypeID()) { int x = 0; CFNumberGetValue((CFNumberRef)v, kCFNumberIntType, &x); r = x ? 1 : 0; }
    else if (CFGetTypeID(v) == CFBooleanGetTypeID()) r = CFBooleanGetValue((CFBooleanRef)v) ? 1 : 0;
    CFRelease(v);
    return r;
}

static NSString *rdna4_model_name(uint32_t port) {
    id value = CFBridgingRelease(IORegistryEntrySearchCFProperty(port, kIOServicePlane, CFSTR("model"),
        kCFAllocatorDefault, kIORegistryIterateRecursively | kIORegistryIterateParents));
    NSString *model = nil;
    if ([value isKindOfClass:NSString.class]) model = value;
    else if ([value isKindOfClass:NSData.class] && [value length] > 0 && [value length] <= 1024) {
        NSData *data = value;
        const char *end = memchr(data.bytes, 0, data.length);
        model = [[NSString alloc] initWithBytes:data.bytes length:end ? (NSUInteger)(end - (const char *)data.bytes) : data.length
            encoding:NSUTF8StringEncoding];
    }
    model = [model stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    return model.length ? [model copy] : [NSString stringWithUTF8String:rdna4dc_fallback().name];
}

static BOOL rdna4_number_u64(CFTypeRef field, uint64_t *out) {
    int64_t number = 0;
    if (!field || CFGetTypeID(field) != CFNumberGetTypeID() || CFNumberIsFloatType((CFNumberRef)field) ||
        !CFNumberGetValue((CFNumberRef)field, kCFNumberSInt64Type, &number) || number < 0) return NO;
    *out = (uint64_t)number;
    return YES;
}
static rdna4dc_native_memory rdna4_nub_memory(uint32_t port) {
    rdna4dc_native_memory m = {0};
    CFTypeRef value = IORegistryEntrySearchCFProperty(port, kIOServicePlane, CFSTR(RDNA4DC_MEMORY_CAPS_PROPERTY),
        kCFAllocatorDefault, kIORegistryIterateRecursively | kIORegistryIterateParents);
    if (!value) return m;
    if (CFGetTypeID(value) == CFDictionaryGetTypeID()) {
        const CFStringRef keys[] = {CFSTR("version"), CFSTR("physical"), CFSTR("pool"),
            CFSTR("visible"), CFSTR("applicationBudget"), CFSTR("maxBuffer")};
        uint64_t fields[6] = {0};
        BOOL valid = YES;
        for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
            CFTypeRef field = CFDictionaryGetValue((CFDictionaryRef)value, keys[i]);
            if (!rdna4_number_u64(field, &fields[i])) {
                valid = NO; break;
            }
        }
        if (valid) {
            m = (rdna4dc_native_memory){.version = fields[0], .physical = fields[1], .pool = fields[2],
                .visible = fields[3], .application_budget = fields[4], .max_buffer = fields[5]};
            CFTypeRef composite = CFDictionaryGetValue((CFDictionaryRef)value, CFSTR("compositeMaxBuffer"));
            (void)rdna4_number_u64(composite, &m.composite_max_buffer);
        }
    }
    CFRelease(value);
    return m;
}
// "Garnet,Ready": absent (old kexts) -> proceed. RDNA4M_TEST_READY overrides.
static int rdna4_nub_ready(uint32_t port) {
    const char *th = getenv("RDNA4M_TEST_READY");
    if (rdna4_allow() && th && th[0]) return atoi(th) ? 1 : 0;
    return rdna4_nub_flag(port, CFSTR("Garnet,Ready"));
}
// #12 R3: "Garnet,AutoDisarmed" = 1 (kernel disarmed the display pipe after a fault) -> decline; absent -> ignore. RDNA4M_TEST_AUTODISARMED overrides.
static int rdna4_nub_autodisarmed(uint32_t port) {
    const char *th = getenv("RDNA4M_TEST_AUTODISARMED");
    if (rdna4_allow() && th && th[0]) return atoi(th) ? 1 : 0;
    return rdna4_nub_flag(port, CFSTR("Garnet,AutoDisarmed"));
}

// The kernel probe (GPU-apps G4): for a process that is neither WindowServer nor an RDNA4M_ALLOW root tool the ONLY way in is the kernel's own decision. Open an RDNA4N connection exactly
// as Mesa's winsys does (IOServiceOpen type 'GRNT'), say Hello, read QueryInfo (HUNG), and close it again at once; the real, lazy RADV connection is opened later by the same process.
// G6 (bundle 8): the connection is opened on the ACCELERATOR PORT this bundle holds (the GarnetAccelerator the aux kext published), not on a GarnetDriver looked up by name: a sandboxed application's profile
// admits user clients only on an IOAccelerator service, and the accelerator's newUserClient hands out the same native client. The port is the caller's (Metal's) object: it is NOT released here.
// A successful open IS the allow-list verdict (uid >= 501, rdna4-apps=1, rdna4-multisession=1, the process on the kernel's list); any failure - including a sandbox that denies the service
// lookup or the open - declines. Returns NULL = admitted.
static const char *rdna4_app_probe(uint32_t port) {
    io_service_t svc = (io_service_t)port;   // G6: the accelerator port (0 = none); never released here
    io_connect_t conn = 0; kern_return_t kr = KERN_FAILURE, hk = KERN_FAILURE, ik = KERN_FAILURE; uint32_t infoFlags = 0, budgetFlags = 0;
    if (svc) {
        for (int tries = 0;; tries++) {
            kr = IOServiceOpen(svc, mach_task_self(), RDNA4G_UC_TYPE, &conn);
            if (!rdna4g_probe_retry((uint32_t)kr, tries)) break;
            usleep(10000);
        }
        if (kr == KERN_SUCCESS) {
            uint64_t in[2] = { 1ull, RDNA4G_HELLO_F_MINOR }, out[4] = { 0 }; uint32_t outCnt = 4;
            hk = IOConnectCallScalarMethod(conn, RDNA4G_SEL_HELLO, in, 2, out, &outCnt);
            if (hk == KERN_SUCCESS) {
                uint8_t info[RDNA4G_INFO_SIZE]; size_t isz = sizeof info; memset(info, 0, sizeof info);
                ik = IOConnectCallStructMethod(conn, RDNA4G_SEL_QUERYINFO, NULL, 0, info, &isz);
                if (ik == KERN_SUCCESS && isz == sizeof info) { memcpy(&infoFlags, info + RDNA4G_INFO_FLAGS_OFF, sizeof infoFlags); memcpy(&budgetFlags, info + RDNA4G_INFO_BUDGET_OFF, sizeof budgetFlags); } else if (ik == KERN_SUCCESS) ik = KERN_FAILURE;
            }
            IOServiceClose(conn);   // the session is released at once; Mesa opens its own later
        }
    }
    const int v = rdna4g_probe_verdict(svc != 0, (uint32_t)kr, (uint32_t)hk, (uint32_t)ik, infoFlags, budgetFlags);
    if (v != RDNA4G_PROBE_OK) RDNA4LOG("APP probe: declined (%s): open 0x%x hello 0x%x info 0x%x flags 0x%x budget 0x%x", rdna4g_probe_text(v), (unsigned)kr, (unsigned)hk, (unsigned)ik, infoFlags, budgetFlags);
    else RDNA4LOG("APP probe: the kernel admitted this process (allow-listed application): GPU path enabled for %s", getprogname());
    return v == RDNA4G_PROBE_OK ? NULL : rdna4g_probe_text(v);
}

static const char *rdna4_denylist_decline(int cls) {
    if (!rdna4g_denylist_applies(cls)) return NULL;
    NSString *bidStr = [[NSBundle mainBundle] bundleIdentifier];
    const char *bid = bidStr ? [bidStr UTF8String] : NULL;
    const char *proc = getprogname();
    const char *envPath = rdna4_allow() ? getenv("RDNA4M_TEST_DENYLIST") : NULL;
    const char *path = (envPath && envPath[0]) ? envPath : RDNA4G_DENYLIST_PATH;
    int openErr = 0;
    const int hit = rdna4g_denylist_eval(cls, path, bid, proc, &openErr);
    if (hit != RDNA4G_DENY_NONE) {
        RDNA4LOG("admit: declined by denylist (%s) bundle=%s proc=%s",
                 hit == RDNA4G_DENY_BUILTIN ? "builtin" : hit == RDNA4G_DENY_BUNDLE ? "bundle" : "proc",
                 bid ? bid : "-", proc ? proc : "-");
        return "application is on the Garnet denylist";
    }
    if (openErr != 0 && !rdna4g_denylist_open_fallback(openErr))
        RDNA4LOG("admit: denylist read errno=%d (%s); falling back to kernel appdeny", openErr, strerror(openErr));
    return NULL;
}

// NULL = admit, else the reason for declining (static string).
static const char *rdna4_admit(uint32_t port) {
    const int cls = rdna4g_class(rdna4_is_ws(), rdna4_allow());
    const int isWs = cls == RDNA4G_CLASS_WS;
    // 0.0.641 (G4 review HIGH 2): root and daemons (euid < 501) that are neither WindowServer nor an RDNA4M_ALLOW tool are declined BY CLASS, first of all, exactly as in 0.0.632: no kernel open for them.
    if (rdna4g_declined_by_class(cls, (uint32_t)geteuid())) return RDNA4G_DECLINE_BY_CLASS_TEXT;
    // The kill file. Outside WindowServer a stat that fails for any reason but "no such file" (a sandbox answers EPERM) declines: fail CLOSED (rdna4_gate.h).
    struct stat st;
    const int srcRc = stat(RDNA4_KILL_FILE, &st), srcErr = errno;
    const int ks = rdna4g_kill_state(srcRc, srcErr);
    int killIgnored = 0;
    if (ks == RDNA4G_KILL_PRESENT) {
        // S5.2b root test path: ignored ONLY by a non-WindowServer root process with RDNA4M_ALLOW=1 and RDNA4M_TEST_IGNORE_KILL=1 (WindowServer never is).
        killIgnored = rdna4df_off_file_ignored((int)geteuid(), getenv("RDNA4M_ALLOW"), getenv("RDNA4M_TEST_IGNORE_KILL"), rdna4_is_ws());
        if (!killIgnored) return "kill file " RDNA4_KILL_FILE " exists";
        RDNA4LOG("admit: kill file " RDNA4_KILL_FILE " exists but is IGNORED (root test bypass RDNA4M_ALLOW=1 + RDNA4M_TEST_IGNORE_KILL=1, not WindowServer)");
    }
    if (rdna4g_kill_declines(isWs, ks, killIgnored)) {
        RDNA4LOG("admit: kill file " RDNA4_KILL_FILE " could not be checked (stat errno %d): declining outside WindowServer (fail closed)", srcErr);
        return "kill file " RDNA4_KILL_FILE " could not be read (fail closed outside WindowServer)";
    }
    const int rdy = rdna4_nub_ready(port);
    if (rdna4g_ready_declines(isWs, rdy)) return rdy == 0 ? "nub property Garnet,Ready is 0" : "nub property Garnet,Ready is absent or unreadable (fail closed outside WindowServer)";
    if (rdna4g_autodisarm_declines(isWs, rdna4_nub_autodisarmed(port))) return "nub property Garnet,AutoDisarmed is 1";
    int count = 0;
    if (rdna4_counter_tripped(&count)) return "crash counter: >= 3 starts within 300 s";
    if (cls == RDNA4G_CLASS_OTHER) {   // not WindowServer, not an RDNA4M_ALLOW root tool: only the kernel can admit it
        const char *denyWhy = rdna4_denylist_decline(cls);
        if (denyWhy) return denyWhy;
        const char *why = rdna4_app_probe(port);
        if (why) return why;
        atomic_store(&rdna4_app_admitted, 1);
    }
    return NULL;
}
static BOOL rdna4_headless(void) {
    static BOOL v; static dispatch_once_t once;
    dispatch_once(&once, ^{
        struct stat st;
        int result = stat(RDNA4_HEADLESS_NO, &st);
        int error = result ? errno : 0;
        v = result != 0;
        RDNA4LOG("[DEBUG-render] isHeadless=%d markerResult=%d errno=%d process=%s",
                 v, result, error, getprogname());
    });
    return v;
}

#include "rdna4_ca_sw_compat.h"
#include "rdna4_compositor_observation.h"
#include "rdna4_coredisplay_compat.h"
#include "rdna4_system_report_impl.inc"

// Private implementation modules share this translation unit, in dependency order.
// See STRUCTURE.md; assemble-device.py supplies the same source to text-based tests.
#include "rdna4_backend_impl.inc"
#include "rdna4_resource_caches_impl.inc"
#include "rdna4_surface_impl.inc"
#include "rdna4_shader_dump_impl.inc"
#include "rdna4_buffer_impl.inc"
#include "rdna4_command_support_impl.inc"
#include "rdna4_texture_impl.inc"
#include "rdna4_heap_impl.inc"
#include "rdna4_sampler_runtime_impl.inc"
#include "rdna4_depth_fence_impl.inc"
#include "rdna4_shader_runtime_impl.inc"
#include "rdna4_tessellation.h"
#include "rdna4_icb_abi.h"
#include "rdna4_binary_archive_impl.inc"
#include "rdna4_render_pipeline_impl.inc"
#include "rdna4_stage_bindings_impl.inc"
#include "rdna4_compute_pipeline_impl.inc"
#include "rdna4_command_queue_impl.inc"
#include "rdna4_display_late_impl.inc"
#include "rdna4_surface_capture.inc"
#include "rdna4_present_capture.inc"
#include "rdna4_command_buffer_impl.inc"
#include "rdna4_counters_impl.inc"
#include "rdna4_render_encoder_impl.inc"
#include "rdna4_compute_encoder_impl.inc"
#include "rdna4_blit_encoder_impl.inc"
#include "rdna4_gl_drawable_impl.inc"
#include "rdna4_residency_set_impl.inc"
#include "rdna4_io_queue_impl.inc"
@implementation RDNA4CommandBuffer (Encoders)
- (id)renderCommandEncoderWithDescriptor:(MTLRenderPassDescriptor *)d {
    (void)[self vk];
    RDNA4LOGR("cb renderCommandEncoderWithDescriptor:");
    return [[RDNA4RenderEncoder alloc] initWithCommandBuffer:self descriptor:d];
}
- (id)blitCommandEncoder {
    RDNA4LOGR("cb blitCommandEncoder");
    return [[RDNA4BlitEncoder alloc] initWithCommandBuffer:self blit:0];
}
- (id)blitCommandEncoderWithDescriptor:(MTLBlitPassDescriptor *)d {
    RDNA4LOGR("cb blitCommandEncoderWithDescriptor:");
    return [[RDNA4BlitEncoder alloc] initWithCommandBuffer:self descriptor:d];
}
- (id)computeCommandEncoder { (void)[self vk]; RDNA4LOGR("cb computeCommandEncoder"); return [[RDNA4ComputeEncoder alloc] initWithCommandBuffer:self compute:0]; }
- (id)computeCommandEncoderWithDescriptor:(MTLComputePassDescriptor *)d {
    (void)[self vk];
    RDNA4LOGR("cb computeCommandEncoderWithDescriptor:");
    return [[RDNA4ComputeEncoder alloc] initWithCommandBuffer:self descriptor:d];
}
- (id)computeCommandEncoderWithDispatchType:(NSUInteger)t { (void)t; return [self computeCommandEncoder]; }
@end

// MTLGPUFamily values: rdna4_gate.h (RDNA4G_FAMILY_*, rdna4g_supports_family).

static NSData *rdna4_function_reflection_data(id function) {
    SEL selector = sel_registerName("reflectionData");
    id data = [function respondsToSelector:selector] ? ((id (*)(id, SEL))objc_msgSend)(function, selector) : nil;
    return [data isKindOfClass:NSData.class] && [data length] ? data : nil;
}

#include "rdna4_argument.inc"
#include "rdna4_argument_summary.inc"
#include "rdna4_argument_summary_owners.inc"

@implementation GarnetDevice

- (id)compiler {
    id native = [super compiler];
    if (native) return native;
    static RDNA4ReflectionCompiler *compiler;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ compiler = [RDNA4ReflectionCompiler new]; });
    return compiler;
}
- (id<MTLBinaryArchive>)newBinaryArchiveWithDescriptor:(MTLBinaryArchiveDescriptor *)descriptor error:(NSError **)error {
    return [[RDNA4BinaryArchive alloc] initWithDevice:self descriptor:descriptor error:error];
}
// Metal's archive, dynamic-library and MTL4-archive loaders all call +[MTLLoader sliceIDForDevice:...], which asserts
// (MTLReportFailure -> abort) when -targetDeviceArchitecture is nil; the _MTLDevice default is nil for a non-Apple
// device. Describe an architecture no shipped slice carries (Apple GPU slices are CPU_TYPE 0x1000013), so every
// loader that bypasses -newBinaryArchiveWithDescriptor:error: (e.g. +[_MTLBinaryArchive archiveTypeAtURL:...],
// MTLDynamicLibraryContainer, MTL4ArchiveImpl) finds no matching slice and reports an error instead of aborting.
- (id)targetDeviceArchitecture {
    static id arch;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        Class cls = objc_getClass("MTLTargetDeviceArchitecture");
        if (!cls) return;
        id a = [[cls alloc] init];
        if ([a respondsToSelector:@selector(setCpuType:)]) ((void (*)(id, SEL, uint32_t))objc_msgSend)(a, @selector(setCpuType:), RDNA4_TARGET_ARCH_CPUTYPE);
        if ([a respondsToSelector:@selector(setSubType:)]) ((void (*)(id, SEL, uint32_t))objc_msgSend)(a, @selector(setSubType:), RDNA4_TARGET_ARCH_SUBTYPE);
        if ([a respondsToSelector:@selector(setVersion:)]) ((void (*)(id, SEL, RDNA4TargetArchVersion))objc_msgSend)(a, @selector(setVersion:), (RDNA4TargetArchVersion){ 1, 0, 0 });
        if ([a respondsToSelector:@selector(setDriverVersion:)]) ((void (*)(id, SEL, uint64_t))objc_msgSend)(a, @selector(setDriverVersion:), 1);
        arch = a;
    });
    return arch;
}
// A non-nil architecture only makes slice lookup safe; it does not add native
// machine-code support. Do not inherit affirmative capability answers from Metal.
- (BOOL)supportsQueryTextureLOD {
    const rdna4x_backend_capabilities caps = rdna4_query_caps();
    return rdna4x_query_supported(&caps) ? YES : NO;
}
- (BOOL)supportsDynamicLibraries { return NO; }
- (BOOL)supportsRenderDynamicLibraries { return NO; }
- (BOOL)supportsBinaryArchives { return NO; }
// Native dynamic libraries and MTL4 archives are machine-code containers too: decline them like binary archives.
- (id)newDynamicLibraryWithURL:(NSURL *)url error:(NSError **)error {
    if (error) *error = [NSError errorWithDomain:MTLDynamicLibraryDomain code:MTLDynamicLibraryErrorUnsupported
        userInfo:@{ NSLocalizedDescriptionKey: @"Native Metal dynamic libraries are unavailable on this device." }];
    RDNA4_ONCE("native dynamic library (URL) declined");
    return nil;
}
- (id)newDynamicLibrary:(id<MTLLibrary>)library error:(NSError **)error {
    if (error) *error = [NSError errorWithDomain:MTLDynamicLibraryDomain code:MTLDynamicLibraryErrorUnsupported
        userInfo:@{ NSLocalizedDescriptionKey: @"Native Metal dynamic libraries are unavailable on this device." }];
    RDNA4_ONCE("native dynamic library declined");
    return nil;
}
- (id)newArchiveWithURL:(NSURL *)url error:(NSError **)error {
    if (error) *error = [NSError errorWithDomain:MTLBinaryArchiveDomain code:MTLBinaryArchiveErrorInvalidFile
        userInfo:@{ NSLocalizedDescriptionKey: @"Native Metal 4 archives are unavailable on this device." }];
    RDNA4_ONCE("native MTL4 archive declined");
    return nil;
}
- (id)newIndirectArgumentBufferLayoutWithStructType:(MTLStructType *)type {
    return [[RDNA4ArgumentLayout alloc] initWithType:type device:self depth:0];
}
- (id)newArgumentEncoderWithLayout:(id)layout {
    if (![layout isKindOfClass:RDNA4ArgumentLayout.class]) return nil;
    return [[RDNA4ArgumentEncoder alloc] initWithLayout:layout device:self];
}
- (id)newArgumentEncoderWithArguments:(NSArray<MTLArgumentDescriptor *> *)arguments {
    RDNA4ArgumentLayout *layout = [[RDNA4ArgumentLayout alloc] initWithArguments:arguments device:self];
    return [self newArgumentEncoderWithLayout:layout];
}
- (id)newIndirectArgumentEncoderWithLayout:(id)layout { return [self newArgumentEncoderWithLayout:layout]; }
- (id)newArgumentEncoderWithBufferBinding:(id<MTLBufferBinding>)binding API_AVAILABLE(macos(13.0)) {
    if (binding.type != MTLBindingTypeBuffer || !binding.bufferStructType) return nil;
    return [self newArgumentEncoderWithLayout:[self newIndirectArgumentBufferLayoutWithStructType:binding.bufferStructType]];
}

// S5.2a: scanout stats / release for `mtlprobe dispflip` (root test path; absent on every other Metal device).
- (NSDictionary *)rdna4ScanoutStats { return rdna4s_stats(); }
- (NSDictionary *)rdna4ScanoutRelease { return rdna4s_release_request(); }

// MTLAddDevice asserts [newDevice conformsToProtocol:@protocol(MTLDevice)] (Metal line 1054; seen on the PC
//. Apple's AppleParavirtDevice declares MTLDeviceSPI + MTLDevice + NSObject in its class
// protocol list (pvm.x86 baseProtocols); MTLIOAccelDevice declares none. Add the same conformance at load.
+ (void)load {
    const char *names[] = { "MTLDeviceSPI", "MTLDevice" };
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        Protocol *p = objc_getProtocol(names[i]);
        BOOL added = p ? class_addProtocol(self, p) : NO;
        os_log(OS_LOG_DEFAULT, "GarnetMetal: +load protocol %{public}s found %d added %d", names[i], p != nil, added);
    }
    const char *bnames[] = { "MTLBuffer", "MTLResource" };   // RDNA4Buffer conformance (no compile-time adoption)
    for (unsigned i = 0; i < sizeof(bnames) / sizeof(bnames[0]); i++) {
        Protocol *p = objc_getProtocol(bnames[i]);
        BOOL added = p ? class_addProtocol([RDNA4Buffer class], p) : NO;
        os_log(OS_LOG_DEFAULT, "GarnetMetal: +load RDNA4Buffer protocol %{public}s found %d added %d", bnames[i], p != nil, added);
    }
    { const char *n[] = { "MTLTexture", "MTLResource" }; rdna4_add_protocols([RDNA4Texture class], n, 2, "RDNA4Texture"); }
    { const char *n[] = { "MTLRenderPipelineState" }; rdna4_add_protocols([RDNA4RenderPipelineState class], n, 1, "RDNA4RenderPipelineState"); }
    { const char *n[] = { "MTLCommandQueue" }; rdna4_add_protocols([RDNA4CommandQueue class], n, 1, "RDNA4CommandQueue"); }
    { const char *n[] = { "MTLCommandBuffer" }; rdna4_add_protocols([RDNA4CommandBuffer class], n, 1, "RDNA4CommandBuffer"); }
    { const char *n[] = { "MTLRenderCommandEncoder", "MTLCommandEncoder" }; rdna4_add_protocols([RDNA4RenderEncoder class], n, 2, "RDNA4RenderEncoder"); }
    { const char *n[] = { "MTLBlitCommandEncoder", "MTLCommandEncoder" }; rdna4_add_protocols([RDNA4BlitEncoder class], n, 2, "RDNA4BlitEncoder"); }
    { const char *n[] = { "MTLComputeCommandEncoder", "MTLCommandEncoder" }; rdna4_add_protocols([RDNA4ComputeEncoder class], n, 2, "RDNA4ComputeEncoder"); }
    { const char *n[] = { "MTLComputePipelineState" }; rdna4_add_protocols([RDNA4ComputePipelineState class], n, 1, "RDNA4ComputePipelineState"); }
    { const char *n[] = { "MTLSamplerState" }; rdna4_add_protocols([RDNA4SamplerState class], n, 1, "RDNA4SamplerState"); }
    { const char *n[] = { "MTLDepthStencilState" }; rdna4_add_protocols([RDNA4DepthStencilState class], n, 1, "RDNA4DepthStencilState"); }
    { const char *n[] = { "MTLFence" }; rdna4_add_protocols([RDNA4Fence class], n, 1, "RDNA4Fence"); }
    { const char *n[] = { "MTLHeap" }; rdna4_add_protocols([RDNA4Heap class], n, 1, "RDNA4Heap"); }
    { const char *n[] = { "MTLArgumentEncoder" }; rdna4_add_protocols([RDNA4ArgumentEncoder class], n, 1, "RDNA4ArgumentEncoder"); }
    { const char *n[] = { "MTLBinaryArchive" }; rdna4_add_protocols([RDNA4BinaryArchive class], n, 1, "RDNA4BinaryArchive"); }
    { const char *n[] = { "MTLCounter" }; rdna4_add_protocols([RDNA4Counter class], n, 1, "RDNA4Counter"); }
    { const char *n[] = { "MTLCounterSet" }; rdna4_add_protocols([RDNA4CounterSet class], n, 1, "RDNA4CounterSet"); }
    { const char *n[] = { "MTLCounterSampleBuffer" }; rdna4_add_protocols([RDNA4CounterSampleBuffer class], n, 1, "RDNA4CounterSampleBuffer"); }
    {   // build 18: the census (rdna4_census.h): which instance methods of the running system's Metal protocols do our classes lack? Computed here, logged by a process that gets a device (or printed with RDNA4M_CENSUS=1).
        const rdna4cen_pair pairs[] = { { [RDNA4RenderEncoder class], "RDNA4RenderEncoder", "MTLRenderCommandEncoder" }, { [RDNA4ComputeEncoder class], "RDNA4ComputeEncoder", "MTLComputeCommandEncoder" },
            { [RDNA4BlitEncoder class], "RDNA4BlitEncoder", "MTLBlitCommandEncoder" }, { [RDNA4CommandBuffer class], "RDNA4CommandBuffer", "MTLCommandBuffer" }, { self, "GarnetDevice", "MTLDevice" },
            { [RDNA4Texture class], "RDNA4Texture", "MTLTexture" }, { [RDNA4Buffer class], "RDNA4Buffer", "MTLBuffer" },
            { [RDNA4RenderPipelineState class], "RDNA4RenderPipelineState", "MTLRenderPipelineState" }, { [RDNA4ComputePipelineState class], "RDNA4ComputePipelineState", "MTLComputePipelineState" },   // (extra, beyond the contract: where RenderBox's newVisibleFunctionTableWithDescriptor:stage: lives)
            { [RDNA4CommandQueue class], "RDNA4CommandQueue", "MTLCommandQueue" }, { [RDNA4SamplerState class], "RDNA4SamplerState", "MTLSamplerState" }, { [RDNA4DepthStencilState class], "RDNA4DepthStencilState", "MTLDepthStencilState" },
            { [RDNA4Fence class], "RDNA4Fence", "MTLFence" }, { [RDNA4Heap class], "RDNA4Heap", "MTLHeap" }, { Nil, NULL, NULL } };
        unsigned total = rdna4cen_run(pairs);
        if (getenv("RDNA4M_CENSUS")) { for (NSString *l in rdna4cen_lines) fprintf(stderr, "%s\n", l.UTF8String); fprintf(stderr, "census: %u selector(s) missing in all\n", total); }
    }
}
// build 18: the census lines saved by +load, logged once by a process that got a device.
static void rdna4_census_log(void) {
    static _Atomic int done_; if (atomic_exchange(&done_, 1)) return;
    for (NSString *l in rdna4cen_lines) RDNA4LOG("%s", l.UTF8String);
    RDNA4LOG("census: %lu (class, selector) pair(s) missing in this process", (unsigned long)rdna4cen_missing_set.count);
}

// lazyInitialize: log, then run the base (only if some superclass implements it).
- (void)lazyInitialize {
    Class sup = class_getSuperclass([GarnetDevice class]);
    BOOL has = class_getInstanceMethod(sup, @selector(lazyInitialize)) != NULL;
    RDNA4LOG("lazyInitialize (super implements: %d)", has);
    if (has) [super lazyInitialize];
}

// ---- 10a: buffers ----
- (id)newBufferWithLength:(NSUInteger)length options:(MTLResourceOptions)options {
    RDNA4LOGR("newBufferWithLength:%lu options:0x%lx", (unsigned long)length, (unsigned long)options);
    if (length == 0) { RDNA4LOG("newBuffer: refused length"); return nil; }
    if (((options >> 4) & 0xF) > 2) {   // 11e: Shared, Managed, Private
        RDNA4LOG("newBuffer: storage mode %lu not supported (Memoryless)", (unsigned long)((options >> 4) & 0xF));
        return nil;
    }
    NSError *err = nil;
    RDNA4Buffer *b = [[RDNA4Buffer alloc] initWithDevice:self length:length options:options error:&err];
    if (!b) RDNA4LOG("newBuffer: failed: %s", err.localizedDescription.UTF8String);
    return b;
}

- (id)newBufferWithBytes:(const void *)bytes length:(NSUInteger)length options:(MTLResourceOptions)options {
    RDNA4LOGR("newBufferWithBytes:length:%lu options:0x%lx", (unsigned long)length, (unsigned long)options);
    RDNA4Buffer *b = [self newBufferWithLength:length options:options];
    if (b && [b contents] && bytes) memcpy([b contents], bytes, length);
    else if (b && bytes) RDNA4LOG("newBufferWithBytes: buffer has no CPU mapping (Private): contents NOT copied");
    return b;
}

// ---- 11e: samplers ----
- (id)newSamplerStateWithDescriptor:(MTLSamplerDescriptor *)d {
    RDNA4LOGR("newSamplerStateWithDescriptor:");
    NSError *err = nil;
    RDNA4SamplerState *s = [[RDNA4SamplerState alloc] initWithDevice:self descriptor:d error:&err];
    if (!s) RDNA4LOG("newSamplerState: failed: %s", err.localizedDescription.UTF8String);
    return s;
}

// Compute misses use bounded waits then temporary placeholders for WS/admitted
// apps. The existing hot-swap registration replaces them when translation arrives.
- (id)rdna4NewComputePipeline:(id)fn descriptor:(MTLComputePipelineDescriptor *)desc options:(NSUInteger)opts error:(NSError **)error {
    NSError *e = nil;
    NSError *inprocFailure = nil;
    RDNA4_LK_SCOPE;   // build 18 (P3): the descriptor variants install the kernel's link context inside this scope; a plain function-based call must never see an old one
    if (desc) rdna4x_linkage_compute(desc);
    else if (rdna4_lk_for(fn) == nil) rdna4_lk_install(2, nil);
    RDNA4ComputePipelineState *p = desc
        ? [[RDNA4ComputePipelineState alloc] initWithDevice:self descriptor:desc options:opts error:&e]
        : [[RDNA4ComputePipelineState alloc] initWithDevice:self function:fn error:&e];
    if (p) return p;
    if ((opts & RDNA4_PIPELINE_OPT_FAIL_ON_ARCHIVE_MISS) || [e.domain isEqualToString:MTLBinaryArchiveDomain]) {
        RDNA4LOG("compute pipeline binary archive failure: %s", e.localizedDescription.UTF8String);
        if (error) *error = e;
        return nil;
    }
    const BOOL inproc = (e.code == 41 && rdna4x_active()) ? YES : NO;
    // build 16 (P4): the first attempt of an application that translates in process is NOT a failure: it is a miss in the bundle's own cache, followed by "inproc OK" and a created pipeline (0 COMPUTE FALLBACK lines in the b14 trial).
    if (inproc) RDNA4LOG("compute pipeline: not in the bundle's spvcache (%s); trying the in-process translation", e.localizedDescription.UTF8String);
    else RDNA4LOG("compute pipeline creation failed: %s", e.localizedDescription.UTF8String);
    const BOOL backendReady = !inproc || rdna4_radv_open(&inprocFailure);
    if (inproc && backendReady && rdna4x_translate_fn_with_error(fn, "kernel", rdna4_now() + (uint64_t)RDNA4G_SYNC_WAIT_APP_MS * 1000000ull, &inprocFailure)) {   // translate in process first; skip the daemon wait
        NSError *e3 = nil;
        RDNA4ComputePipelineState *p3 = desc
            ? [[RDNA4ComputePipelineState alloc] initWithDevice:self descriptor:desc options:opts error:&e3]
            : [[RDNA4ComputePipelineState alloc] initWithDevice:self function:fn error:&e3];
        if (p3) return p3;
        inprocFailure = e3 ?: rdna4_err(100, @"pipeline initializer returned nil without an NSError after in-process translation");
        RDNA4LOG("compute pipeline creation failed after the in-process translation: %s", inprocFailure.localizedDescription.UTF8String);
    }
    if (!inproc && e.code == 41 && rdna4g_syncwait_applies(rdna4_is_ws(), atomic_load(&rdna4_app_admitted), rdna4_force_fallback(), rdna4_test_fb_as_ws())) {
        // Dump before waiting: WS gets 250 ms, admitted apps 3 s, subject to the process strikes budget.
        NSMutableArray *dp = [NSMutableArray array], *dn = [NSMutableArray array];
        rdna4_dump_function(fn, "kernel", dp, dn);
        NSString *ksha = rdna4_fn_sha(fn);
        if (ksha && rdna4_sync_wait("kernel", @[ ksha ], YES)) {
            NSError *e2 = nil;
            RDNA4ComputePipelineState *p2 = desc
                ? [[RDNA4ComputePipelineState alloc] initWithDevice:self descriptor:desc options:opts error:&e2]
                : [[RDNA4ComputePipelineState alloc] initWithDevice:self function:fn error:&e2];
            if (p2) return p2;
            RDNA4LOG("compute pipeline creation failed after the wait: %s", e2.localizedDescription.UTF8String);
        }
    }
    if (e.code == 41 && rdna4_fallback_ok()) {
        // WS/admitted apps need a valid pipeline to avoid MPS/Metal aborts.
        // Its dispatches are temporary no-ops until hot-swap installs the real shader.
        NSMutableArray *paths = [NSMutableArray array], *notes = [NSMutableArray array];
        if (!inproc) rdna4_dump_function(fn, "kernel", paths, notes);
        RDNA4LOG("COMPUTE FALLBACK %s (%s); %s: %s", ([fn respondsToSelector:@selector(name)] ? [fn name] : @"?").UTF8String, e.localizedDescription.UTF8String, inproc ? "not dumped (in-process translation did not produce it)" : "AIR dumped", [paths componentsJoinedByString:@"; "].UTF8String);
        return [[RDNA4ComputePipelineState alloc] initPlaceholderWithDevice:self function:fn];
    }
    if (e.code == 41) {
        if (inproc) {
            NSString *failure = inprocFailure.localizedDescription ?: @"no in-process failure reason was reported";
            NSString *message = [failure hasPrefix:@"in-process translation failed for"]
                ? [NSString stringWithFormat:@"GarnetMetal: compute pipeline: %@", failure]
                : [NSString stringWithFormat:@"GarnetMetal: compute pipeline: pipeline creation after in-process translation failed: %@", failure];
            e = [NSError errorWithDomain:@"GarnetMetal" code:100 userInfo:@{
                NSLocalizedDescriptionKey:message,
                NSUnderlyingErrorKey:inprocFailure ?: rdna4_err(100, message)
            }];
        } else {
            NSMutableArray *paths = [NSMutableArray array], *notes = [NSMutableArray array];
            rdna4_dump_function(fn, "kernel", paths, notes);
            NSString *status = paths.count ? @"AIR dumped for offline translation" : @"AIR dump did not produce a file";
            e = rdna4_err(100, [NSString stringWithFormat:@"GarnetMetal: compute pipeline not in spvcache; %@: %@%@",
                status, [paths componentsJoinedByString:@"; "],
                notes.count ? [@" | problems: " stringByAppendingString:[notes componentsJoinedByString:@"; "]] : @""]);
        }
        RDNA4LOG("%s", e.localizedDescription.UTF8String);
    }
    if (error) *error = e;
    return nil;
}
- (id)rdna4NewComputePipeline:(id)fn error:(NSError **)error {
    return [self rdna4NewComputePipeline:fn descriptor:nil options:0 error:error];
}
- (id)newComputePipelineStateWithFunction:(id)fn error:(NSError **)error { RDNA4LOG("newComputePipelineStateWithFunction:error:"); return [self rdna4NewComputePipeline:fn error:error]; }
- (id)rdna4NewComputePipeline:(id)fn descriptor:(MTLComputePipelineDescriptor *)desc stageInputDescriptor:(MTLStageInputOutputDescriptor *)descriptor options:(NSUInteger)o reflection:(MTLComputePipelineReflection **)r error:(NSError **)error {
    if (r) *r = nil;
    RDNA4ComputePipelineState *pipeline = [self rdna4NewComputePipeline:fn descriptor:desc options:o error:error];
    if (!pipeline || !(o & 3u)) return pipeline;   // BindingInfo | BufferTypeInfo
    NSData *data = rdna4_function_reflection_data(fn);
    Class factory = NSClassFromString(@"MTLComputePipelineReflectionInternal");
    MTLComputePipelineReflection *reflection = nil;
    if (data && [factory instancesRespondToSelector:@selector(initWithSerializedData:serializedStageInputDescriptor:device:options:flags:)])
        reflection = [[factory alloc] initWithSerializedData:data serializedStageInputDescriptor:[descriptor newSerializedDescriptor]
            device:self options:o flags:(RDNA4ReflectionFlags){0}];
    if (!reflection) {
        if (error) *error = rdna4_err(101, @"Compute function reflection is unavailable");
        return nil;
    }
    if (!rdna4_argument_reflection(fn, rdna4nc_stage_arguments(reflection, RDNA4NC_COMPUTE))) {
        if (error) *error = rdna4_err(101, @"Compute argument pointer reflection does not match its encoded layout");
        return nil;
    }
    [pipeline rdna4SetReflection:reflection];
    if (r) *r = reflection;
    return pipeline;
}
- (id)rdna4NewComputePipeline:(id)fn stageInputDescriptor:(MTLStageInputOutputDescriptor *)descriptor options:(NSUInteger)o reflection:(MTLComputePipelineReflection **)r error:(NSError **)error {
    return [self rdna4NewComputePipeline:fn descriptor:nil stageInputDescriptor:descriptor options:o reflection:r error:error];
}
- (id)newComputePipelineStateWithFunction:(id)fn options:(NSUInteger)o reflection:(MTLComputePipelineReflection **)r error:(NSError **)error {
    RDNA4LOG("newComputePipelineStateWithFunction:options:reflection:error:"); return [self rdna4NewComputePipeline:fn stageInputDescriptor:nil options:o reflection:r error:error]; }
- (void)newComputePipelineStateWithFunction:(id)fn completionHandler:(void (^)(id, NSError *))h {
    if (!h) return;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ @autoreleasepool {
        NSError *e = nil; id p = [self rdna4NewComputePipeline:fn error:&e]; h(p, e);
    }}); }
- (void)newComputePipelineStateWithFunction:(id)fn options:(NSUInteger)o completionHandler:(void (^)(id, id, NSError *))h {
    if (!h) return;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ @autoreleasepool {
        NSError *e = nil; MTLComputePipelineReflection *r = nil;
        id p = [self rdna4NewComputePipeline:fn stageInputDescriptor:nil options:o reflection:&r error:&e]; h(p, r, e);
    }}); }
// native #12 luma: QuartzCore's CA::OGL::MetalContext::get_compute_pipeline (compute_average_luma, glass backdrop tint, Reduce Transparency off) sends
// -newComputePipelineStateWithDescriptor:error: (CONFIRMED: selector reference at QuartzCore 0x7ff80d2e6c74 resolves through the cache). That selector was not
// implemented here, the inherited one returned nil although the spvcache has the kernel, and QuartzCore aborts on nil (abort_with_payload, function=compute_average_luma).
static BOOL rdna4_compute_desc_wants_icb(MTLComputePipelineDescriptor *d, NSError **error) {
#ifndef RDNA4_COMPUTE_ICB_ENABLED
    if (d && [d respondsToSelector:@selector(supportIndirectCommandBuffers)] && d.supportIndirectCommandBuffers) {
        if (error) *error = rdna4_err(61, @"compute indirect command buffers are outside the Metal 2 baseline");
        return YES;
    }
#else
    (void)d; (void)error;
#endif
    return NO;
}
- (id)newComputePipelineStateWithDescriptor:(MTLComputePipelineDescriptor *)d error:(NSError **)error {
    if (rdna4_compute_desc_wants_icb(d, error)) return nil;
    RDNA4LOG("newComputePipelineStateWithDescriptor:error:");
    return [self rdna4NewComputePipeline:d.computeFunction descriptor:d options:0 error:error];
}
- (void)newComputePipelineStateWithDescriptor:(MTLComputePipelineDescriptor *)d completionHandler:(void (^)(id, NSError *))h {
    if (!h) return;
    MTLComputePipelineDescriptor *saved = [d copy];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ @autoreleasepool {
        NSError *e = nil;
        if (rdna4_compute_desc_wants_icb(saved, &e)) { h(nil, e); return; }
        id p = [self rdna4NewComputePipeline:saved.computeFunction descriptor:saved options:0 error:&e]; h(p, e);
    }}); }
- (id)newComputePipelineStateWithDescriptor:(MTLComputePipelineDescriptor *)d options:(NSUInteger)o reflection:(MTLComputePipelineReflection **)r error:(NSError **)error {
    if (rdna4_compute_desc_wants_icb(d, error)) return nil;
    RDNA4LOG("newComputePipelineStateWithDescriptor:options:reflection:error:");
    return [self rdna4NewComputePipeline:d.computeFunction descriptor:d stageInputDescriptor:d.stageInputDescriptor options:o reflection:r error:error];
}
- (void)newComputePipelineStateWithDescriptor:(MTLComputePipelineDescriptor *)d options:(NSUInteger)o completionHandler:(void (^)(id, id, NSError *))h {
    if (!h) return;
    MTLComputePipelineDescriptor *saved = [d copy];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ @autoreleasepool {
        NSError *e = nil; MTLComputePipelineReflection *r = nil;
        if (rdna4_compute_desc_wants_icb(saved, &e)) { h(nil, nil, e); return; }
        id p = [self rdna4NewComputePipeline:saved.computeFunction descriptor:saved stageInputDescriptor:saved.stageInputDescriptor options:o reflection:&r error:&e]; h(p, r, e);
    }}); }

// ---- 10c: textures ----
- (id)newTextureWithDescriptor:(MTLTextureDescriptor *)d {
    RDNA4LOGR("newTextureWithDescriptor: %lux%lu pf %lu usage 0x%lx", (unsigned long)d.width, (unsigned long)d.height, (unsigned long)d.pixelFormat, (unsigned long)d.usage);
    NSError *err = nil;
    RDNA4Texture *t = [[RDNA4Texture alloc] initWithDevice:self descriptor:d error:&err];
    if (!t) RDNA4LOG("newTextureWithDescriptor NIL: pf %lu type %lu usage 0x%lx storage %lu %lux%lux%lu mips %lu array %lu samples %lu (%s)", (unsigned long)d.pixelFormat, (unsigned long)d.textureType,
                   (unsigned long)d.usage, (unsigned long)d.storageMode, (unsigned long)d.width, (unsigned long)d.height, (unsigned long)d.depth, (unsigned long)d.mipmapLevelCount,
                   (unsigned long)d.arrayLength, (unsigned long)d.sampleCount, err.localizedDescription.UTF8String);
    return t;
}

// 11h.6: IOSurface-backed texture (plane 0 of a single-plane 32 bpp surface); nil + log when the surface cannot be imported.
- (id)newTextureWithDescriptor:(MTLTextureDescriptor *)d iosurface:(IOSurfaceRef)s plane:(NSUInteger)plane {
    RDNA4LOGR("newTextureWithDescriptor:iosurface:plane: %lux%lu pf %lu usage 0x%lx plane %lu", (unsigned long)d.width, (unsigned long)d.height, (unsigned long)d.pixelFormat, (unsigned long)d.usage, (unsigned long)plane);
    NSError *err = nil;
    RDNA4Texture *t = [[RDNA4Texture alloc] initWithDevice:self descriptor:d iosurface:s plane:plane error:&err];
    if (!t) RDNA4LOG("newTextureWithDescriptor:iosurface:plane: NIL: pf %lu type %lu usage 0x%lx storage %lu %lux%lu plane %lu (%s)", (unsigned long)d.pixelFormat, (unsigned long)d.textureType,
                   (unsigned long)d.usage, (unsigned long)d.storageMode, (unsigned long)d.width, (unsigned long)d.height, (unsigned long)plane, err.localizedDescription.UTF8String);
    return t;
}

// ---- 12: client-memory buffers and textures (QuartzCore: -newTiledTextureWithBytesNoCopy:length:descriptor:offset:bytesPerRow:, 4 per second at the login screen) ----
- (id)newBufferWithBytesNoCopy:(void *)p length:(NSUInteger)len options:(MTLResourceOptions)o deallocator:(void (^)(void *, NSUInteger))d {
    RDNA4LOG("newBufferWithBytesNoCopy:length:%lu options:0x%lx", (unsigned long)len, (unsigned long)o);
    NSError *e = nil; RDNA4Buffer *b = [[RDNA4Buffer alloc] initWithDevice:self bytesNoCopy:p length:len options:o deallocator:d error:&e];
    if (!b) RDNA4LOG("newBufferWithBytesNoCopy: NIL: %s", e.localizedDescription.UTF8String);   // a refused buffer never owned the memory: the deallocator is NOT called
    return b;
}
- (id)rdna4NoCopyTexture:(void *)p length:(NSUInteger)len deallocator:(void (^)(void *, NSUInteger))dl descriptor:(MTLTextureDescriptor *)d offset:(NSUInteger)off bytesPerRow:(NSUInteger)bpr what:(const char *)what {
    RDNA4Buffer *b = [self newBufferWithBytesNoCopy:p length:len options:MTLResourceStorageModeShared deallocator:dl];
    if (!b) { RDNA4LOG("%s: NIL (buffer over the client memory refused); pf %lu type %lu usage 0x%lx storage %lu %lux%lu", what, (unsigned long)d.pixelFormat, (unsigned long)d.textureType,
                     (unsigned long)d.usage, (unsigned long)d.storageMode, (unsigned long)d.width, (unsigned long)d.height); return nil; }
    return [b newTextureWithDescriptor:d offset:off bytesPerRow:bpr];   // the texture keeps the buffer, the buffer keeps the memory until the deallocator runs
}
- (id)newTiledTextureWithBytesNoCopy:(void *)p length:(NSUInteger)len descriptor:(MTLTextureDescriptor *)d offset:(NSUInteger)off bytesPerRow:(NSUInteger)bpr {
    return [self rdna4NoCopyTexture:p length:len deallocator:nil descriptor:d offset:off bytesPerRow:bpr what:"newTiledTextureWithBytesNoCopy:length:descriptor:offset:bytesPerRow:"];
}
- (id)newTiledTextureWithBytesNoCopy:(void *)p length:(NSUInteger)len deallocator:(void (^)(void *, NSUInteger))dl descriptor:(MTLTextureDescriptor *)d offset:(NSUInteger)off bytesPerRow:(NSUInteger)bpr {
    return [self rdna4NoCopyTexture:p length:len deallocator:dl descriptor:d offset:off bytesPerRow:bpr what:"newTiledTextureWithBytesNoCopy:length:deallocator:descriptor:offset:bytesPerRow:"];
}
- (id)newTextureWithBytesNoCopy:(void *)p length:(NSUInteger)len descriptor:(MTLTextureDescriptor *)d deallocator:(void (^)(void *, NSUInteger))dl {
    const RDNA4Fmt *f = rdna4_fmt(d.pixelFormat);
    return [self rdna4NoCopyTexture:p length:len deallocator:dl descriptor:d offset:0 bytesPerRow:f ? d.width * f->bpp : 0 what:"newTextureWithBytesNoCopy:length:descriptor:deallocator:"];
}

// The heap query is the same cold/hot path: open RADV once, create an unbound
// temporary resource with the constructor's createInfo, ask requirements2, and
// destroy it without allocating memory. SizeAndAlign includes the heap's
// placement extent, not just the bytes charged to -usedSize.
static BOOL rdna4_heap_query_extent(VkMemoryRequirements mr, BOOL hostImport,
                                    NSUInteger *size, NSUInteger *align) {
    if (!size || !align || !mr.size || !mr.alignment) return NO;
    uint64_t gran = RDNA4R.lim.bufferImageGranularity ? RDNA4R.lim.bufferImageGranularity : 1;
    uint64_t host = hostImport ? RDNA4R.hostAlign : 1;
    if (!gran || (gran & (gran - 1)) || !host || (host & (host - 1))) return NO;
    uint64_t a = MAX((uint64_t)mr.alignment, gran);
    if (hostImport) a = MAX(a, host);
    uint64_t extent = (uint64_t)rdna4_alup((NSUInteger)mr.size, (NSUInteger)gran);
    if (extent == UINT64_MAX) return NO;
    if (hostImport) extent = (uint64_t)rdna4_alup((NSUInteger)extent, (NSUInteger)host);
    if (extent == UINT64_MAX || extent > NSUIntegerMax || a > NSUIntegerMax) return NO;
    *size = (NSUInteger)extent;
    *align = (NSUInteger)a;
    return YES;
}

static BOOL rdna4_heap_buffer_size_align(NSUInteger len, NSUInteger opts,
                                         NSUInteger *size, NSUInteger *align) {
    if (size) *size = 0;
    if (align) *align = 0;
    if (!len || !size || !align) return NO;
    NSError *error = nil;
    if (!rdna4_radv_open(&error)) {
        RDNA4LOG("heapBufferSizeAndAlignWithLength: RADV open failed: %s; size 0",
                 error.localizedDescription.UTF8String);
        return NO;
    }
    unsigned sm = (unsigned)((opts >> 4) & 0xF);
    if (sm > 2) {
        RDNA4LOG("heapBufferSizeAndAlignWithLength: storage mode Memoryless is not supported; size 0");
        return NO;
    }
    rdna4dc_caps caps = rdna4_device_caps();
    if (!rdna4dc_buffer_ok(&caps, len)) {
        RDNA4LOG("heapBufferSizeAndAlignWithLength: length %lu exceeds the Vulkan buffer limit; size 0",
                 (unsigned long)len);
        return NO;
    }
    BOOL hostImport = sm < 2 && len > (64ull << 20);
    if (hostImport && (!RDNA4R.hostExt || !rdna4_vkGetMHPP)) {
        RDNA4LOG("heapBufferSizeAndAlignWithLength: large Shared/Managed buffers require coherent host import; size 0");
        return NO;
    }
    if (!RDNA4P.bmr2) {
        RDNA4LOG("heapBufferSizeAndAlignWithLength: vkGetBufferMemoryRequirements2 is unavailable; size 0");
        return NO;
    }
    VkExternalMemoryBufferCreateInfo external = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT };
    VkBufferCreateInfo create = rdna4_heap_buffer_create_info(len, hostImport ? &external : NULL);
    VkBuffer buffer = VK_NULL_HANDLE;
    VkResult result = vkCreateBuffer(RDNA4R.dev, &create, NULL, &buffer);
    if (result != VK_SUCCESS || !buffer) {
        RDNA4LOG("heapBufferSizeAndAlignWithLength: vkCreateBuffer = %d handle %p; size 0",
                 result, (void *)buffer);
        return NO;
    }
    VkMemoryRequirements mr;
    BOOL queried = rdna4_requirements2(buffer, VK_NULL_HANDLE, &mr, NULL, NULL, YES);
    vkDestroyBuffer(RDNA4R.dev, buffer, NULL);
    if (!queried) {
        RDNA4LOG("heapBufferSizeAndAlignWithLength: buffer requirements2 query failed; size 0");
        return NO;
    }
    if (!rdna4_heap_query_extent(mr, hostImport, size, align)) {
        RDNA4LOG("heapBufferSizeAndAlignWithLength: invalid buffer requirement (size %llu alignment %llu); size 0",
                 (unsigned long long)mr.size, (unsigned long long)mr.alignment);
        *size = *align = 0;
        return NO;
    }
    return YES;
}

static BOOL rdna4_heap_image_size_align(MTLTextureDescriptor *d,
                                        NSUInteger *size, NSUInteger *align) {
    if (size) *size = 0;
    if (align) *align = 0;
    if (!d || !size || !align) return NO;
    NSError *error = nil;
    if (!rdna4_radv_open(&error)) {
        RDNA4LOG("heapTextureSizeAndAlignWithDescriptor: RADV open failed: %s; size 0",
                 error.localizedDescription.UTF8String);
        return NO;
    }
    if (!RDNA4P.imr2) {
        RDNA4LOG("heapTextureSizeAndAlignWithDescriptor: vkGetImageMemoryRequirements2 is unavailable; size 0");
        return NO;
    }
    rdna4_heap_image_plan plan;
    if (!rdna4_heap_image_plan_make(d, YES, &plan, &error)) {
        RDNA4LOG("heapTextureSizeAndAlignWithDescriptor: %s; size 0",
                 error.localizedDescription.UTF8String);
        return NO;
    }
    VkImage image = VK_NULL_HANDLE;
    VkResult result = vkCreateImage(RDNA4R.dev, &plan.create, NULL, &image);
    if (result != VK_SUCCESS || !image) {
        RDNA4LOG("heapTextureSizeAndAlignWithDescriptor: vkCreateImage = %d handle %p; size 0",
                 result, (void *)image);
        return NO;
    }
    VkMemoryRequirements mr;
    BOOL queried = rdna4_requirements2(VK_NULL_HANDLE, image, &mr, NULL, NULL, YES);
    vkDestroyImage(RDNA4R.dev, image, NULL);
    if (!queried) {
        RDNA4LOG("heapTextureSizeAndAlignWithDescriptor: image requirements2 query failed; size 0");
        return NO;
    }
    if (!rdna4_heap_query_extent(mr, NO, size, align)) {
        RDNA4LOG("heapTextureSizeAndAlignWithDescriptor: invalid image requirement (size %llu alignment %llu); size 0",
                 (unsigned long long)mr.size, (unsigned long long)mr.alignment);
        *size = *align = 0;
        return NO;
    }
    return YES;
}

// ---- m11 H1: heaps (see RDNA4Heap) ----
- (id)newHeapWithDescriptor:(MTLHeapDescriptor *)d {
    RDNA4LOG("newHeapWithDescriptor: size %lu storage %lu cache %lu hazard %lu type %lu", (unsigned long)d.size, (unsigned long)d.storageMode, (unsigned long)d.cpuCacheMode, (unsigned long)d.hazardTrackingMode, (unsigned long)d.type);
    return [[RDNA4Heap alloc] initWithDevice:self descriptor:d];
}
- (MTLSizeAndAlign)heapBufferSizeAndAlignWithLength:(NSUInteger)len options:(MTLResourceOptions)o {
    NSUInteger size = 0, align = 0;
    (void)rdna4_heap_buffer_size_align(len, o, &size, &align);
    return (MTLSizeAndAlign){ size, align };
}
- (MTLSizeAndAlign)heapTextureSizeAndAlignWithDescriptor:(MTLTextureDescriptor *)d {
    NSUInteger size = 0, align = 0;
    (void)rdna4_heap_image_size_align(d, &size, &align);
    return (MTLSizeAndAlign){ size, align };
}

// ---- gap census: depth/stencil state, fences, limits ----
- (id)newDepthStencilStateWithDescriptor:(MTLDepthStencilDescriptor *)d {
    RDNA4LOG("newDepthStencilStateWithDescriptor:");
    return [[RDNA4DepthStencilState alloc] initWithDevice:self descriptor:d];
}
// MTLIOAccelDevice -newFence SEGVs on this device (measured: -[MTLIOAccelDevice newFence] -> _dispatch_sync_f on a NULL queue).
- (id)newFence { RDNA4LOG("newFence"); return [[RDNA4Fence alloc] initWithDevice:self]; }
// MTLIOAccelDevice -currentAllocatedSize SEGVs on this device (measured: it reads the IOAccel resource pool we do not have).
- (NSUInteger)currentAllocatedSize { return (NSUInteger)atomic_load(&rdna4_alloc_total); }
- (NSUInteger)maxThreadgroupMemoryLength { return rdna4_device_caps().shared_memory; }
// The decoder combines this backend offset limit with each type's alignment (resource structs can be 8).
- (NSUInteger)minConstantBufferAlignmentBytes {
    NSUInteger alignment = 4;
    if (RDNA4R.ok) {
        alignment = MAX(alignment, (NSUInteger)RDNA4R.lim.minUniformBufferOffsetAlignment);
        alignment = MAX(alignment, (NSUInteger)RDNA4R.lim.minStorageBufferOffsetAlignment);
    }
    return alignment;
}
- (MTLSize)maxThreadsPerThreadgroup {
    rdna4dc_caps caps = rdna4_device_caps();
    return MTLSizeMake(caps.workgroup_size[0], caps.workgroup_size[1], caps.workgroup_size[2]);
}
// bundle 10: 1 always; 2 / 4 / 8 when the device's framebuffer colour AND depth/stencil counts include them (RADV open); before RADV is open (asking must not open it) 4 only, which Vulkan requires of every device.
- (BOOL)supportsSampleCount:(NSUInteger)n { unsigned m = rdna4_ms_mask(); return rdna4ms_device_supports((unsigned long)n, RDNA4R.ok ? 1 : 0, m, m) ? YES : NO; }
- (BOOL)supportsTextureSampleCount:(NSUInteger)n { return [self supportsSampleCount:n]; }
- (NSUInteger)iosurfaceTextureAlignmentBytes { return 32; }
- (NSUInteger)minimumTextureBufferAlignmentForPixelFormat:(MTLPixelFormat)pf { (void)pf; return 256; }
- (NSUInteger)minimumLinearTextureAlignmentForPixelFormat:(MTLPixelFormat)pf { (void)pf; return 256; }
- (BOOL)isDepth24Stencil8PixelFormatSupported { return NO; }
- (BOOL)supportsUnalignedVertexFetch { return YES; }
- (MTLReadWriteTextureTier)readWriteTextureSupport { return MTLReadWriteTextureTier2; }
- (BOOL)areProgrammableSamplePositionsSupported { return RDNA4R.ok ? RDNA4R.sampleLocationsExt : YES; }
- (BOOL)areRasterOrderGroupsSupported { return RDNA4R.ok ? RDNA4R.fragInterlock : YES; }
- (BOOL)supportsBCTextureCompression { return RDNA4R.ok ? RDNA4R.bcCompression : YES; }
- (BOOL)supports32BitFloatFiltering { return YES; }
- (BOOL)supports32BitMSAA { return YES; }
- (BOOL)supportsVertexAmplificationCount:(NSUInteger)count { return count == 1; }
- (BOOL)supportsRaytracing { return NO; }
- (BOOL)supportsRaytracingFromRender { return NO; }
- (BOOL)supportsPrimitiveMotionBlur { return NO; }
- (BOOL)supportsFunctionPointers { return NO; }
- (BOOL)supportsFunctionPointersFromRender { return NO; }
- (BOOL)supportsPullModelInterpolation {
    const rdna4dc_metal3_caps m3 = rdna4_metal3_caps();
    return rdna4dc_metal3_pull_model_ok(&m3) ? YES : NO;
}
- (BOOL)supportsShaderBarycentricCoordinates {
    const rdna4dc_metal3_caps m3 = rdna4_metal3_caps();
    return rdna4dc_metal3_barycentric_ok(&m3) ? YES : NO;
}
- (NSArray<id<MTLCounterSet>> *)counterSets { return rdna4_device_counter_sets(); }
- (BOOL)supportsCounterSampling:(MTLCounterSamplingPoint)samplingPoint {
    return rdna4_device_supports_counter_sampling(samplingPoint);
}
- (id<MTLCounterSampleBuffer>)newCounterSampleBufferWithDescriptor:(MTLCounterSampleBufferDescriptor *)descriptor
                                                             error:(NSError **)error {
    return rdna4_device_new_counter_sample_buffer(self, descriptor, error);
}
- (void)sampleTimestamps:(MTLTimestamp *)cpuTimestamp gpuTimestamp:(MTLTimestamp *)gpuTimestamp {
    rdna4_device_sample_timestamps(cpuTimestamp, gpuTimestamp);
}
- (id)newIOFileHandleWithURL:(NSURL *)url error:(NSError **)error {
    return [[RDNA4IOFileHandle alloc] initWithDevice:self url:url
                                   compressionMethod:RDNA4IO_COMP_NONE
                                          compressed:NO error:error];
}
- (id)newIOFileHandleWithURL:(NSURL *)url compressionMethod:(NSInteger)method error:(NSError **)error {
    return [[RDNA4IOFileHandle alloc] initWithDevice:self url:url
                                   compressionMethod:method
                                          compressed:YES error:error];
}
- (id)newIOHandleWithURL:(NSURL *)url error:(NSError **)error {
    return [self newIOFileHandleWithURL:url error:error];
}
- (id)newIOHandleWithURL:(NSURL *)url compressionMethod:(NSInteger)method error:(NSError **)error {
    return [self newIOFileHandleWithURL:url compressionMethod:method error:error];
}
- (id)newIOCommandQueueWithDescriptor:(id)descriptor error:(NSError **)error {
    return [[RDNA4IOCommandQueue alloc] initWithDevice:self descriptor:descriptor error:error];
}
- (id)newResidencySetWithDescriptor:(id)descriptor error:(NSError **)error {
    return [[RDNA4ResidencySet alloc] initWithDevice:self descriptor:descriptor error:error];
}
- (void)getDefaultSamplePositions:(MTLSamplePosition *)positions count:(NSUInteger)count {
    if (!positions) return;
    static const MTLSamplePosition p1[1] = { { 0.5f, 0.5f } };
    static const MTLSamplePosition p2[2] = { { 0.75f, 0.75f }, { 0.25f, 0.25f } };
    static const MTLSamplePosition p4[4] = { { 0.375f, 0.125f }, { 0.875f, 0.375f }, { 0.125f, 0.625f }, { 0.625f, 0.875f } };
    static const MTLSamplePosition p8[8] = {
        { 0.5625f, 0.3125f }, { 0.4375f, 0.6875f }, { 0.8125f, 0.5625f }, { 0.3125f, 0.1875f },
        { 0.1875f, 0.8125f }, { 0.0625f, 0.4375f }, { 0.6875f, 0.9375f }, { 0.9375f, 0.0625f }
    };
    const MTLSamplePosition *src = count == 1 ? p1 : count == 2 ? p2 : count == 4 ? p4 : count == 8 ? p8 : NULL;
    if (src) memcpy(positions, src, count * sizeof(*positions));
}
- (id)newGLDrawable {
    rdna4_gl_surfaces_install();
    return [GarnetGLDrawable new];
}
- (int)getSurfaceMode:(unsigned long long *)mode width:(unsigned *)width
               height:(unsigned *)height forId:(unsigned)sid {
    pthread_mutex_lock(&rdna4_gl_mu);
    const rdna4gl_slot *g = rdna4gl_find_by_sid(&rdna4_gl_surfaces, sid);
    if (!g) {
        pthread_mutex_unlock(&rdna4_gl_mu);
        return kIOReturnNotFound;
    }
    if (mode) *mode = 0ULL;
    if (width) *width = g->width;
    if (height) *height = g->height;
    pthread_mutex_unlock(&rdna4_gl_mu);
    return 0;
}

// ---- 10b: queues ----
// -[_MTLCommandQueue dealloc] calls [_dev _purgeDevice] -> MTLIOAccelCommandBufferStoragePoolPurge on pools our
// device never created (SEGV in os_unfair_lock_lock, seen. We own no IOAccel pools: nothing to purge.
- (void)_purgeDevice { RDNA4LOG("_purgeDevice (no IOAccel pools; skipped)"); }

- (id)rdna4NewQueue:(id)desc {
    NSError *err = nil;
    RDNA4CommandQueue *q = [[RDNA4CommandQueue alloc] initWithDevice:self descriptor:desc error:&err];
    if (!q) RDNA4LOG("newCommandQueue: failed: %s", err.localizedDescription.UTF8String);
    return q;
}
- (id)newCommandQueue { RDNA4LOG("newCommandQueue"); return [self rdna4NewQueue:nil]; }
- (id)newCommandQueueWithMaxCommandBufferCount:(NSUInteger)n { (void)n; RDNA4LOG("newCommandQueueWithMaxCommandBufferCount:"); return [self rdna4NewQueue:nil]; }
- (id)newCommandQueueWithDescriptor:(id)d { RDNA4LOG("newCommandQueueWithDescriptor:"); return [self rdna4NewQueue:d]; }

// ---- 10d: pipeline creation from the SPIR-V cache; a miss dumps the AIR and returns an NSError (all four forms;
// MTLCompiler is nil on _MTLDevice, F1) ----
- (id)rdna4NewPipeline:(MTLRenderPipelineDescriptor *)d options:(NSUInteger)opts error:(NSError **)error {
    NSError *e = nil;
    RDNA4_LK_SCOPE;   // build 18 (P3): the link contexts live for this creation only
    rdna4x_linkage_render(d);   // bundle 14 (section 3): one LINKAGE IGNORED line per pipeline whose descriptor links functions
    RDNA4RenderPipelineState *p = [[RDNA4RenderPipelineState alloc] initWithDevice:self descriptor:d options:opts noFallback:NO error:&e];
    if (p) return p;
    if (e.code == 41 && rdna4x_active()) RDNA4LOG("render pipeline: not in the bundle's spvcache (%s); the in-process translation was already tried for its functions", e.localizedDescription.UTF8String);   // build 16 (P4)
    else RDNA4LOG("pipeline creation failed: %s", e.localizedDescription.UTF8String);
    if (error) *error = [e copy];
    return nil;
}
- (id)rdna4NewPipeline:(MTLRenderPipelineDescriptor *)d error:(NSError **)error {
    return [self rdna4NewPipeline:d options:0 error:error];
}
- (id)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)d error:(NSError **)error {
    RDNA4LOG("newRenderPipelineStateWithDescriptor:error:");
    return [self rdna4NewPipeline:d error:error];
}
- (id)rdna4NewPipeline:(MTLRenderPipelineDescriptor *)d options:(NSUInteger)opts reflection:(MTLRenderPipelineReflection **)refl error:(NSError **)error {
    if (refl) *refl = nil;
    RDNA4RenderPipelineState *pipeline = [self rdna4NewPipeline:d options:opts error:error];
    if (!pipeline || !(opts & 3u)) return pipeline;   // BindingInfo | BufferTypeInfo
    NSData *vertex = rdna4_function_reflection_data(d.vertexFunction);
    NSData *fragment = d.fragmentFunction ? rdna4_function_reflection_data(d.fragmentFunction) : nil;
    Class factory = NSClassFromString(@"MTLRenderPipelineReflectionInternal");
    MTLRenderPipelineReflection *reflection = nil;
    if (vertex && (!d.fragmentFunction || fragment) && [factory instancesRespondToSelector:@selector(initWithVertexData:fragmentData:serializedVertexDescriptor:device:options:flags:)])
        reflection = [[factory alloc] initWithVertexData:vertex fragmentData:fragment serializedVertexDescriptor:[d.vertexDescriptor newSerializedDescriptor]
            device:self options:opts flags:(RDNA4ReflectionFlags){0}];
    if (!reflection) {
        if (error) *error = rdna4_err(102, @"Render function reflection is unavailable");
        return nil;
    }
    if (!rdna4_argument_reflection(d.vertexFunction, rdna4nc_stage_arguments(reflection, RDNA4NC_VERTEX)) ||
        (d.fragmentFunction && !rdna4_argument_reflection(d.fragmentFunction, rdna4nc_stage_arguments(reflection, RDNA4NC_FRAGMENT)))) {
        if (error) *error = rdna4_err(102, @"Render argument pointer reflection does not match its encoded layout");
        return nil;
    }
    [pipeline rdna4SetReflection:reflection];
    if (refl) *refl = reflection;
    return pipeline;
}
- (id)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)d options:(NSUInteger)opts reflection:(MTLRenderPipelineReflection **)refl error:(NSError **)error {
    RDNA4LOG("newRenderPipelineStateWithDescriptor:options:reflection:error:");
    return [self rdna4NewPipeline:d options:opts reflection:refl error:error];
}
- (void)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)d completionHandler:(void (^)(id, NSError *))h {
    if (!h) return;
    MTLRenderPipelineDescriptor *saved = [d copy];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ @autoreleasepool {
        NSError *e = nil; id p = [self rdna4NewPipeline:saved error:&e]; h(p, e);
    }});
}
- (void)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor *)d options:(NSUInteger)opts completionHandler:(void (^)(id, id, NSError *))h {
    if (!h) return;
    MTLRenderPipelineDescriptor *saved = [d copy];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ @autoreleasepool {
        NSError *e = nil; MTLRenderPipelineReflection *r = nil;
        id p = [self rdna4NewPipeline:saved options:opts reflection:&r error:&e]; h(p, r, e);
    }});
}

// Diagnostic: name any selector Metal sends that we (and our superclasses) do not implement, before the
// standard exception. Seen: -[_MTLDevice initLimits] -> doesNotRecognizeSelector.
- (void)doesNotRecognizeSelector:(SEL)sel {
    os_log(OS_LOG_DEFAULT, "GarnetMetal: UNRECOGNIZED selector %{public}s", sel_getName(sel));
    NSLog(@"GarnetMetal: UNRECOGNIZED selector %s", sel_getName(sel));
    [super doesNotRecognizeSelector:sel];
}

- (instancetype)initWithAcceleratorPort:(uint32_t)port {
    os_log(OS_LOG_DEFAULT, "GarnetMetal: initWithAcceleratorPort 0x%x", port);
    NSLog(@"GarnetMetal: initWithAcceleratorPort 0x%x", port);
    const char *why = rdna4_admit(port);   // 11b: process filter, kill file, Ready, crash counter
    self = [super initWithAcceleratorPort:port];
    if (why) {
        // Declining BEFORE the base init crashed (measured: the un-initialised object is released by ARC and
        // -[_MTLDevice dealloc] -> dispatch_release(NULL) SEGVs), so the base init runs first and the object is then
        // dropped: ARC releases the fully initialised self and Metal sees nil.
        os_log(OS_LOG_DEFAULT, "GarnetMetal: initWithAcceleratorPort 0x%x DECLINED in %{public}s (pid %d): %{public}s", port, getprogname(), getpid(), why);
        fprintf(stderr, "GarnetMetal: declined in %s (pid %d): %s\n", getprogname(), getpid(), why);
        return nil;
    }
    if (self) {
        _rdna4AcceleratorPort = port;
        (void)IORegistryEntryGetRegistryEntryID((io_registry_entry_t)port, &_rdna4RegistryID);
        _rdna4Model = rdna4_model_name(port);
        _rdna4NativeMemory = rdna4_nub_memory(port);
        rdna4_argument_limit_prepare(&_rdna4NativeMemory);
    }
    os_log(OS_LOG_DEFAULT, "GarnetMetal: initWithAcceleratorPort 0x%x -> %{public}s", port, self ? "ok" : "nil");
    if (self && rdna4_is_ws()) (void)rdna4_ca_sw_compat();
    if (self && rdna4_is_ws()) {
        uint32_t inheritedPort = ((uint32_t (*)(id, SEL))objc_msgSend)(
            self, sel_getUid("acceleratorPort"));
        RDNA4LOG("[DEBUG-render] device inputPort=0x%x acceleratorPort=0x%x", port, inheritedPort);
        rdna4_coredisplay_compat(inheritedPort);
        rdna4_compositor_observe();
    }
    if (self) rdna4_census_log();   // build 18: once per process that got a device
    if (self) rdna4sp_register((id<MTLDevice>)self, port);
    if (self && !rdna4_is_ws()) rdna4_gl_surfaces_install();
    if (self && rdna4_is_ws() && !rdna4_acc_port) rdna4_acc_port = port;   // bundle 11: WindowServer reads the kernel's surface table through the parent (the nub) of this accelerator
    return self;
}

- (BOOL)supportLazyInitialization { return RDNA4_LAZY ? YES : NO; }

// System Profiler matches this name to the PCI model before any resource opens RADV.
- (NSString *)name { return _rdna4Model ?: [NSString stringWithUTF8String:rdna4dc_fallback().name]; }
- (NSString *)vendorName  { return @"AMD"; }
- (NSString *)familyName  { return @""; }
- (NSString *)productName { return [self name]; }
- (uint64_t)registryID { return _rdna4RegistryID; }
- (MTLDeviceLocation)location { return MTLDeviceLocationSlot; }
- (NSUInteger)locationNumber {
    CFTypeRef value = IORegistryEntrySearchCFProperty((io_registry_entry_t)_rdna4AcceleratorPort,
        kIOServicePlane, CFSTR("pcidebug"), kCFAllocatorDefault, kIORegistryIterateParents);
    if (!value) return 0;
    char path[128]; uint32_t slot = 0;
    if (CFGetTypeID(value) == CFStringGetTypeID() &&
        CFStringGetCString((CFStringRef)value, path, sizeof path, kCFStringEncodingUTF8))
        slot = rdna4sp_pci_location_number(path);
    CFRelease(value);
    return slot;
}
- (uint64_t)maxTransferRate {
    CFTypeRef value = IORegistryEntrySearchCFProperty((io_registry_entry_t)_rdna4AcceleratorPort,
        kIOServicePlane, CFSTR("IOPCIExpressLinkCapabilities"), kCFAllocatorDefault,
        kIORegistryIterateParents);
    uint64_t capabilities = 0, rate = 0;
    if (rdna4_number_u64(value, &capabilities)) rate = rdna4sp_pcie_rate(capabilities);
    if (value) CFRelease(value);
    return rate;
}

- (NSUInteger)maxArgumentBufferSamplerCount {
    uint32_t limit = 0;
    if (!rdna4_is_ws() &&
        rdna4_argument_limit_snapshot_public_limit(&RDNA4ALS, &limit) ==
            RDNA4_ARGUMENT_LIMIT_RESULT_OK)
        return (NSUInteger)limit;

    /*
     * A missing/incompatible export has no authoritative device-wide value.
     * Preserve the pre-existing MTLIOAccelDevice contract instead of
     * publishing a temporary 0/1/16/31/96/128 value that changes after open.
     */
    RDNA4_ONCE("maxArgumentBufferSamplerCount: immutable source unavailable; preserving the superclass Tier1 value");
    Class sc = [MTLIOAccelDevice class];
    if ([sc instancesRespondToSelector:_cmd]) {
        struct objc_super sup = { self, sc };
        return ((NSUInteger (*)(struct objc_super *, SEL))objc_msgSendSuper)(
            &sup, _cmd);
    }
    [self doesNotRecognizeSelector:_cmd];
    return 0;
}

// featureProfile: -[AppleParavirtDevice featureProfile] (pvm.x86 0xacc3) is `mov eax,0x2710; ret` = 10000 (CONFIRMED).
- (NSUInteger)featureProfile { return 10000; }
// llvmVersion: pvm.x86 0xb04f `mov eax,0x7d17` = 32023 (CONFIRMED).
- (int)llvmVersion { return 0x7d17; }

#if RDNA4_9D
- (BOOL)supportsFamily:(NSInteger)family {
    // Argument-buffer encoding and shader translation still gate Metal3.
    if (family == RDNA4G_FAMILY_METAL3 && !rdna4_is_ws()) RDNA4_ONCE("supportsFamily(Metal3) -> NO for this application: argument-buffer support is incomplete (WindowServer keeps YES)");
    return rdna4g_supports_family((long)family, rdna4_is_ws() ? 1 : 0) ? YES : NO;   // Apple1..9 (1001..1009), Mac1, MacCatalyst, Metal3, Metal4, unknown -> NO
}
- (BOOL)supportsFeatureSet:(NSUInteger)featureSet { return rdna4g_supports_feature_set((unsigned long)featureSet) ? YES : NO; }
- (BOOL)deviceSupportsFeatureSet:(NSUInteger)featureSet { return [self supportsFeatureSet:featureSet]; }
// Tier 2 argument buffers (MTLArgumentBuffersTier2 == 1) for applications; WindowServer keeps the base class's answer.
- (NSUInteger)argumentBuffersSupport {
    if (rdna4_is_ws()) {   // the base class's answer, as before build 6 (it is not in our private header, so ask the runtime)
        Class sc = [MTLIOAccelDevice class];
        if ([sc instancesRespondToSelector:@selector(argumentBuffersSupport)]) {
            struct objc_super sup = { self, sc };
            return ((NSUInteger (*)(struct objc_super *, SEL))objc_msgSendSuper)(&sup, @selector(argumentBuffersSupport));
        }
    }
    return (NSUInteger)RDNA4G_ARGBUF_TIER2; }
- (NSUInteger)maxBufferLength                { return (NSUInteger)[self rdna4MemoryCaps].buffer_length; }
- (uint64_t)recommendedMaxWorkingSetSize     { return [self rdna4MemoryCaps].working_set; }
- (BOOL)hasUnifiedMemory { return NO; }
- (BOOL)isHeadless       { return rdna4_headless(); }   // 11b: YES unless /private/tmp/rdna4m-headless-no exists (read once per process)
- (BOOL)isLowPower       { return NO; }
- (BOOL)isRemovable      { return NO; }
#endif

- (rdna4dc_caps)rdna4MemoryCaps {
    if (atomic_load_explicit(&rdna4_caps_ready, memory_order_acquire)) return RDNA4Caps;
    return rdna4dc_from_native_memory(&_rdna4NativeMemory, rdna4_is_ws());
}
- (rdna4_gather_caps)rdna4GatherCaps { return rdna4_device_gather_caps(); }

@end

// CPU ICB baseline: direct/indexed render commands, inherited/explicit buffers,
// bounded range execution, and ordered reset/copy transactions. Patch and compute
// command paths remain explicit refusals until their executable implementations exist.
#include "rdna4_icb_expand.h"
#include "rdna4_icb.inc"
