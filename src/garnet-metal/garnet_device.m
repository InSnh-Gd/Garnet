// garnet_device.m - GarnetMetal modular compilation unit.
#if !defined(GARNET_INTERNAL) && !defined(GARNET_METAL_MODULAR_BUILD)
#define GARNET_INTERNAL static
#endif
#if !defined(GARNET_SYSTEM_REPORT_ONLY)
#ifdef GARNET_METAL_MODULAR_BUILD
#include "garnet_metal_internal.h"
#endif
#include "garnet_ca_sw_compat.h"
#include "garnet_coredisplay_compat.h"
#include "garnet_census.h"
#endif

// SPDisplaysReporter merges only the PCI function's immediate children. Our accelerator
// is below Bringup/MetalNub, so its metadata is lost behind the NDRV framebuffer.
static pthread_mutex_t rdna4sp_mu = PTHREAD_MUTEX_INITIALIZER;
static NSMutableDictionary<NSString *, NSDictionary *> *rdna4sp_devices;
static _Atomic(IMP) rdna4sp_original;
static Ivar rdna4sp_properties;

static NSDictionary *rdna4sp_merge(NSDictionary *report, NSDictionary *properties,
                                  NSDictionary *devices) {
    NSString *path = properties[@"io_registry_entry_path_key"];
    if (![path isKindOfClass:[NSString class]]) return report;
    NSDictionary *device = devices[path];
    id framebufferOwner = properties[@"CFBundleIdentifier"];
    if (!device || (![framebufferOwner isEqual:@"org.garnet.driver"] &&
                    ![framebufferOwner isEqual:@"com.apple.iokit.IONDRVSupport"]))
        return report;
    NSMutableDictionary *result = [report mutableCopy];
    NSString *model = device[@"model"];
    if (model.length) {
        result[@"sppci_model"] = model;
        result[@"_name"] = model;
    }
    // Report the verified accelerator's actual owner, without changing the framebuffer's registry identity.
    result[@"sppci_kextinfo"] = device[@"bundle"];
    result[@"spdisplays_mtlgpufamilysupport"] = device[@"family"];
    [result removeObjectForKey:@"spdisplays_metal"];
    result[@"_metal_path"] = device[@"acceleratorPath"];
    result[@"_spdisplays_regid"] = device[@"registryID"];
    return result;
}
static id rdna4sp_dictionary(id object, SEL command) {
    IMP original = atomic_load_explicit(&rdna4sp_original, memory_order_acquire);
    NSDictionary *report = ((id (*)(id, SEL))original)(object, command);
    id properties = object_getIvar(object, rdna4sp_properties);
    if (![report isKindOfClass:[NSDictionary class]] || ![properties isKindOfClass:[NSDictionary class]])
        return report;
    pthread_mutex_lock(&rdna4sp_mu);
    NSDictionary *devices = [rdna4sp_devices copy];
    pthread_mutex_unlock(&rdna4sp_mu);
    return rdna4sp_merge(report, properties, devices);
}
static void rdna4sp_install(void) {
    pthread_mutex_lock(&rdna4sp_mu);
    if (!atomic_load(&rdna4sp_original)) {
        Class cls = NSClassFromString(@"SPDisplaysDevice");
        Method method = cls ? class_getInstanceMethod(cls, sel_getUid("dictionaryRepresentation")) : NULL;
        Ivar properties = cls ? class_getInstanceVariable(cls, "properties") : NULL;
        Dl_info image;
        IMP original = method ? method_getImplementation(method) : NULL;
        if (original && properties && ivar_getTypeEncoding(properties)[0] == '@' &&
            !strcmp(method_getTypeEncoding(method), "@16@0:8") &&
            dladdr((void *)original, &image) && image.dli_fname &&
            !strcmp(image.dli_fname, "/System/Library/SystemProfiler/SPDisplaysReporter.spreporter/Contents/MacOS/SPDisplaysReporter")) {
            rdna4sp_properties = properties;
            atomic_store_explicit(&rdna4sp_original, original, memory_order_release);
            method_setImplementation(method, (IMP)rdna4sp_dictionary);
        }
    }
    pthread_mutex_unlock(&rdna4sp_mu);
}
static id rdna4sp_property(io_registry_entry_t entry, CFStringRef key) {
    return CFBridgingRelease(IORegistryEntryCreateCFProperty(entry, key, kCFAllocatorDefault, 0));
}
static uint32_t rdna4sp_pci_word(id value) {
    uint32_t word = 0;
    if ([value isKindOfClass:[NSData class]] && [value length] == sizeof word) memcpy(&word, [value bytes], sizeof word);
    return word;
}
GARNET_INTERNAL void rdna4sp_register(id<MTLDevice> device, io_service_t accelerator) {
    if (strcmp(getprogname(), "system_profiler") && strcmp(getprogname(), "System Information")) return;
    uint64_t registryID = 0;
    if (!IOObjectConformsTo(accelerator, "GarnetAccelerator") ||
        IORegistryEntryGetRegistryEntryID(accelerator, &registryID) != KERN_SUCCESS ||
        registryID != device.registryID) return;
    id bundle = rdna4sp_property(accelerator, CFSTR("CFBundleIdentifier"));
    if (![bundle isEqual:@"org.garnet.accel"] ||
        ![rdna4sp_property(accelerator, CFSTR("MetalPluginName")) isEqual:@"GarnetMetal"] ||
        ![rdna4sp_property(accelerator, CFSTR("MetalPluginClassName")) isEqual:@"GarnetDevice"]) return;
    io_registry_entry_t entry = accelerator;
    if (IOObjectRetain(entry) != KERN_SUCCESS) return;
    io_string_t pciPath, acceleratorPath;
    BOOL found = NO;
    for (unsigned depth = 0; depth < 16; depth++) {
        if (IOObjectConformsTo(entry, "IOPCIDevice")) {
            found = rdna4sp_pci_word(rdna4sp_property(entry, CFSTR("vendor-id"))) == 0x1002 &&
                rdna4sp_pci_word(rdna4sp_property(entry, CFSTR("device-id"))) == 0x7590 &&
                IORegistryEntryGetPath(entry, kIOServicePlane, pciPath) == KERN_SUCCESS;
            break;
        }
        io_registry_entry_t parent = 0;
        kern_return_t result = IORegistryEntryGetParentEntry(entry, kIOServicePlane, &parent);
        IOObjectRelease(entry); entry = parent;
        if (result != KERN_SUCCESS || !entry) break;
    }
    if (entry) IOObjectRelease(entry);
    if (!found || IORegistryEntryGetPath(accelerator, kIOServicePlane, acceleratorPath) != KERN_SUCCESS) return;
    NSString *family = nil;
    if ([device supportsFamily:(MTLGPUFamily)5002]) family = @"spdisplays_metal4";
    else if ([device supportsFamily:(MTLGPUFamily)5001]) family = @"spdisplays_metal3";
    else if ([device supportsFamily:MTLGPUFamilyMac2]) family = @"spdisplays_metal2";
    NSMutableDictionary *metadata = [@{ @"bundle": bundle, @"acceleratorPath": @(acceleratorPath),
        @"registryID": [NSString stringWithFormat:@"%llx", (unsigned long long)registryID] } mutableCopy];
    metadata[@"family"] = family;
    metadata[@"model"] = device.name;
    pthread_mutex_lock(&rdna4sp_mu);
    if (!rdna4sp_devices) rdna4sp_devices = [NSMutableDictionary dictionary];
    rdna4sp_devices[@(pciPath)] = [metadata copy];
    pthread_mutex_unlock(&rdna4sp_mu);
    // GUI rendering can initialize Metal before the reporter bundle is loaded.
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        (void)[NSNotificationCenter.defaultCenter addObserverForName:NSBundleDidLoadNotification
            object:nil queue:nil usingBlock:^(NSNotification *note) { (void)note; rdna4sp_install(); }];
    });
    rdna4sp_install();
}
#if !defined(GARNET_SYSTEM_REPORT_ONLY)
// Standalone OpenGL-on-Metal surface interposition and GarnetGLDrawable bridge.
// Adapted from RX4Darwin/AMDRadeonRDNA4.kext (tools/metal/native/RDNA4MTLDevice.mm,
// commit 1a034e6a, next @ dded456d, Copyright (c) 2026 Sunneva N. Mariu, BSD 3-Clause).
// Redirects SkyLight's per-window IOAccel*Surface calls in non-WindowServer GL
// processes while backing drawables with BGRA IOSurfaces; uses only standalone
// Garnet paths.
#if defined(__x86_64__)
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#endif

static pthread_mutex_t rdna4_gl_mu = PTHREAD_MUTEX_INITIALIZER;
static rdna4gl_table rdna4_gl_surfaces;

static void rdna4_gl_release_retired(void *retired[RDNA4GL_NUM_BUFFERS], unsigned n) {
    for (unsigned i = 0; i < n; i++) {
        if (retired[i]) CFRelease((IOSurfaceRef)retired[i]);
    }
}

static void rdna4_gl_ensure_buffers_locked(rdna4gl_slot *s,
                                           void *retired[RDNA4GL_NUM_BUFFERS],
                                           unsigned *n_retired) {
    if (n_retired) *n_retired = 0u;
    if (!s || !s->active || s->width == 0u || s->height == 0u) return;
    if (!rdna4gl_needs_buffer(s, 0u) && !rdna4gl_needs_buffer(s, 1u)) return;

    uint64_t bpr = ((uint64_t)s->width * 4u + 63u) & ~63ull;
    uint64_t allocSize = bpr * (uint64_t)s->height;
    NSDictionary *props = @{
        (__bridge NSString *)kIOSurfaceWidth: @(s->width),
        (__bridge NSString *)kIOSurfaceHeight: @(s->height),
        (__bridge NSString *)kIOSurfaceBytesPerElement: @4,
        (__bridge NSString *)kIOSurfaceBytesPerRow: @(bpr),
        (__bridge NSString *)kIOSurfaceAllocSize: @(allocSize),
        (__bridge NSString *)kIOSurfacePixelFormat: @((uint32_t)0x42475241u)
    };
    IOSurfaceRef b0 = IOSurfaceCreate((__bridge CFDictionaryRef)props);
    IOSurfaceRef b1 = IOSurfaceCreate((__bridge CFDictionaryRef)props);
    if (!b0 || !b1) {
        if (b0) CFRelease(b0);
        if (b1) CFRelease(b1);
        return;
    }
    rdna4gl_install_buffers(s, (void *)b0, (void *)b1, retired, n_retired);
}

#if defined(__x86_64__)
static kern_return_t (*rdna4_real_gl_create)(io_service_t, uint32_t, uint32_t, uint64_t *);
static kern_return_t (*rdna4_real_gl_destroy)(uint64_t);
static kern_return_t (*rdna4_real_gl_control)(uint64_t, uint32_t, uint32_t, uint32_t *);
static kern_return_t (*rdna4_real_gl_set_shape)(uint64_t, const void *, uint32_t, uint32_t);
static kern_return_t (*rdna4_real_gl_set_scale)(uint64_t, uint32_t, const void *);
static kern_return_t (*rdna4_real_gl_wait)(uint64_t);

static kern_return_t rdna4_gl_create_surface(io_service_t accel, uint32_t sid,
                                             uint32_t options, uint64_t *out) {
    if (!IOObjectConformsTo(accel, "GarnetAccelerator"))
        return rdna4_real_gl_create ? rdna4_real_gl_create(accel, sid, options, out)
                                    : kIOReturnBadArgument;
    (void)options;
    pthread_mutex_lock(&rdna4_gl_mu);
    int ok = rdna4gl_create(&rdna4_gl_surfaces, sid, out);
    uint64_t connect = out ? *out : 0ULL;
    pthread_mutex_unlock(&rdna4_gl_mu);
    if (!ok) {
        RDNA4LOG("GL surface table full for sid 0x%x", sid);
        return kIOReturnNoResources;
    }
    RDNA4LOGR("GL surface 0x%x: created connect=0x%llx", sid, (unsigned long long)connect);
    return KERN_SUCCESS;
}

static kern_return_t rdna4_gl_destroy_surface(uint64_t c) {
    if (!rdna4gl_is_connect(c))
        return rdna4_real_gl_destroy ? rdna4_real_gl_destroy(c) : kIOReturnBadArgument;
    void *retired[RDNA4GL_NUM_BUFFERS] = { NULL, NULL };
    unsigned nret = 0u;
    pthread_mutex_lock(&rdna4_gl_mu);
    (void)rdna4gl_destroy(&rdna4_gl_surfaces, c, retired, &nret);
    pthread_mutex_unlock(&rdna4_gl_mu);
    rdna4_gl_release_retired(retired, nret);
    return KERN_SUCCESS;
}

static kern_return_t rdna4_gl_set_shape(uint64_t c, const void *region,
                                        uint32_t options, uint32_t index) {
    if (!rdna4gl_is_connect(c))
        return rdna4_real_gl_set_shape ? rdna4_real_gl_set_shape(c, region, options, index)
                                       : kIOReturnBadArgument;
    void *retired[RDNA4GL_NUM_BUFFERS] = { NULL, NULL };
    unsigned nret = 0u;
    uint32_t sid = 0u, w = 0u, h = 0u;
    pthread_mutex_lock(&rdna4_gl_mu);
    int ok = rdna4gl_set_shape(&rdna4_gl_surfaces, c, region, region ? 12u : 0u,
                               retired, &nret);
    if (ok) {
        const rdna4gl_slot *s = rdna4gl_find_by_connect(&rdna4_gl_surfaces, c);
        if (s) { sid = s->sid; w = s->width; h = s->height; }
    }
    pthread_mutex_unlock(&rdna4_gl_mu);
    rdna4_gl_release_retired(retired, nret);
    if (ok) {
        RDNA4LOGR("GL surface 0x%x: shape %ux%u (options 0x%x, index %u)",
                  sid, w, h, options, index);
    }
    return KERN_SUCCESS;
}

static kern_return_t rdna4_gl_surface_control(uint64_t c, uint32_t cmd,
                                              uint32_t arg, uint32_t *out) {
    if (!rdna4gl_is_connect(c))
        return rdna4_real_gl_control ? rdna4_real_gl_control(c, cmd, arg, out)
                                     : kIOReturnBadArgument;
    if (out) *out = 0u;
    if (cmd == RDNA4GL_CTRL_GET_BUFFER) {
        uint32_t sid = 0u;
        IOSurfaceRef retained = NULL;
        pthread_mutex_lock(&rdna4_gl_mu);
        const rdna4gl_slot *s = rdna4gl_find_by_connect(&rdna4_gl_surfaces, c);
        if (s) sid = s->sid;
        IOSurfaceRef b = (IOSurfaceRef)rdna4gl_control_buffer(&rdna4_gl_surfaces, c, cmd, arg);
        if (b) retained = (IOSurfaceRef)CFRetain(b);
        pthread_mutex_unlock(&rdna4_gl_mu);
        if (retained) {
            if (out) *out = (uint32_t)IOSurfaceCreateMachPort(retained);
            CFRelease(retained);
        }
        RDNA4LOGR("GL surface 0x%x: buffer %u shown (%s)",
                  sid, arg, retained ? "ours" : "none");
        return KERN_SUCCESS;
    }
    RDNA4LOGR("GL surface control 0x%llx: command 0x%x arg 0x%x",
              (unsigned long long)c, cmd, arg);
    return KERN_SUCCESS;
}

static kern_return_t rdna4_gl_set_scale(uint64_t c, uint32_t options, const void *scale) {
    if (rdna4gl_is_connect(c)) return KERN_SUCCESS;
    return rdna4_real_gl_set_scale ? rdna4_real_gl_set_scale(c, options, scale)
                                   : kIOReturnBadArgument;
}

static kern_return_t rdna4_gl_wait_surface(uint64_t c) {
    if (rdna4gl_is_connect(c)) return KERN_SUCCESS;
    return rdna4_real_gl_wait ? rdna4_real_gl_wait(c) : kIOReturnBadArgument;
}

// Rebind an x86_64 __TEXT,__stubs jmpq *slot(%rip) entry in `image`.
static BOOL rdna4_gl_rebind_import(const struct mach_header_64 *image,
                                   const char *name, void *to,
                                   void *real_fn_slot) {
    void *target = dlsym(RTLD_DEFAULT, name);
    if (!image || !target || !to || !real_fn_slot) return NO;
    unsigned long size = 0;
    uint8_t *stubs = getsectiondata(image, "__TEXT", "__stubs", &size);
    if (!stubs || size < 6) return NO;
    BOOL done = NO;
    mach_vm_size_t page_size = (mach_vm_size_t)vm_page_size;
    for (unsigned long i = 0; i + 6 <= size; i += 6) {
        if (stubs[i] != 0xff || stubs[i + 1] != 0x25) continue;
        int32_t disp = 0;
        memcpy(&disp, stubs + i + 2, sizeof disp);
        uintptr_t slot_addr = (uintptr_t)(stubs + i + 6) + (intptr_t)disp;
        void *cur = NULL;
        mach_vm_size_t copied = 0;
        if (mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)slot_addr,
                                   sizeof cur, (mach_vm_address_t)(uintptr_t)&cur,
                                   &copied) != KERN_SUCCESS || copied != sizeof cur)
            continue;
        if (cur != target) continue;
        mach_vm_address_t address = (mach_vm_address_t)slot_addr;
        mach_vm_address_t region = address;
        mach_vm_size_t region_size = 0;
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object = MACH_PORT_NULL;
        kern_return_t kr = mach_vm_region(mach_task_self(), &region, &region_size,
            VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &count, &object);
        if (MACH_PORT_VALID(object)) mach_port_deallocate(mach_task_self(), object);
        if (kr != KERN_SUCCESS || address < region || region_size < sizeof(void *) ||
            address - region > region_size - sizeof(void *) ||
            !(info.protection & VM_PROT_READ))
            continue;
        mach_vm_address_t page = address & ~(page_size - 1);
        if (mach_vm_protect(mach_task_self(), page, page_size, FALSE,
                            info.protection | VM_PROT_WRITE | VM_PROT_COPY) != KERN_SUCCESS)
            continue;
        memcpy(real_fn_slot, &target, sizeof target);
        _Atomic(void *) *slot = (_Atomic(void *) *)(uintptr_t)slot_addr;
        atomic_store_explicit(slot, to, memory_order_release);
        (void)mach_vm_protect(mach_task_self(), page, page_size, FALSE, info.protection);
        done = YES;
    }
    return done;
}

static void rdna4_gl_surfaces_install(void) {
    if (rdna4_is_ws()) return;
    static pthread_mutex_t install_mu = PTHREAD_MUTEX_INITIALIZER;
    static int installed;
    pthread_mutex_lock(&install_mu);
    if (installed) {
        pthread_mutex_unlock(&install_mu);
        return;
    }
    const struct mach_header_64 *sky = NULL;
    BOOL hasGL = NO;
    const uint32_t count = _dyld_image_count();
    for (uint32_t i = 0; i < count; i++) {
        const char *n = _dyld_get_image_name(i);
        if (!n) continue;
        if (strstr(n, "/SkyLight.framework/"))
            sky = (const struct mach_header_64 *)_dyld_get_image_header(i);
        if (strstr(n, "AppleMetalOpenGLRenderer") ||
            strstr(n, "/OpenGL.framework/Versions/A/OpenGL"))
            hasGL = YES;
    }
    if (!sky || !hasGL) {
        pthread_mutex_unlock(&install_mu);
        return;
    }
    // Patch consumer hooks before IOAccelCreateSurface so no tagged handle can be observed unhooked.
    BOOL consumers_ok =
        rdna4_gl_rebind_import(sky, "IOAccelDestroySurface", (void *)rdna4_gl_destroy_surface, &rdna4_real_gl_destroy) &&
        rdna4_gl_rebind_import(sky, "IOAccelSurfaceControl", (void *)rdna4_gl_surface_control, &rdna4_real_gl_control) &&
        rdna4_gl_rebind_import(sky, "IOAccelSetSurfaceFramebufferShape", (void *)rdna4_gl_set_shape, &rdna4_real_gl_set_shape) &&
        rdna4_gl_rebind_import(sky, "IOAccelSetSurfaceScale", (void *)rdna4_gl_set_scale, &rdna4_real_gl_set_scale) &&
        rdna4_gl_rebind_import(sky, "IOAccelWaitForSurface", (void *)rdna4_gl_wait_surface, &rdna4_real_gl_wait);
    BOOL ok = consumers_ok &&
        rdna4_gl_rebind_import(sky, "IOAccelCreateSurface", (void *)rdna4_gl_create_surface, &rdna4_real_gl_create);
    installed = 1;
    pthread_mutex_unlock(&install_mu);
    RDNA4LOG("GL window surfaces in %s: %s",
             getprogname(), ok ? "ours" : "NOT redirected (SkyLight imports not found)");
}
#else
static void rdna4_gl_surfaces_install(void) {}
#endif


@implementation GarnetGLDrawable
- (int)setDrawableSurface:(unsigned)surface mode:(unsigned long long)mode
           colorDepthMode:(unsigned)depth face:(unsigned)face level:(unsigned)level
                 volatile:(unsigned)vol fixedSource:(unsigned)fixed
             scaleOptions:(unsigned)scale scaledWidth:(unsigned)w
             scaledHeight:(unsigned)h {
    (void)mode; (void)depth; (void)face; (void)level;
    (void)vol; (void)fixed; (void)scale; (void)w; (void)h;
    _sid = surface;
    RDNA4LOGR("GL drawable: surface 0x%x %ux%u", surface, w, h);
    return 0;
}

// Returns +1 retained IOSurfaceRef: the GL-on-Metal renderer CFReleases it after creating textures.
- (IOSurfaceRef)lookupIOSurfaceAtIndex:(unsigned long long)i {
    if (i >= RDNA4GL_NUM_BUFFERS) return NULL;
    void *retired[RDNA4GL_NUM_BUFFERS] = { NULL, NULL };
    unsigned nret = 0u;
    IOSurfaceRef ret = NULL;
    pthread_mutex_lock(&rdna4_gl_mu);
    rdna4gl_slot *g = rdna4gl_find_by_sid(&rdna4_gl_surfaces, _sid);
    if (g) {
        rdna4_gl_ensure_buffers_locked(g, retired, &nret);
        if (g->buffers[i]) ret = (IOSurfaceRef)CFRetain((IOSurfaceRef)g->buffers[i]);
    }
    pthread_mutex_unlock(&rdna4_gl_mu);
    rdna4_gl_release_retired(retired, nret);
    return ret;
}

- (unsigned)width {
    pthread_mutex_lock(&rdna4_gl_mu);
    const rdna4gl_slot *g = rdna4gl_find_by_sid(&rdna4_gl_surfaces, _sid);
    unsigned w = g ? g->width : 0u;
    pthread_mutex_unlock(&rdna4_gl_mu);
    return w;
}

- (unsigned)height {
    pthread_mutex_lock(&rdna4_gl_mu);
    const rdna4gl_slot *g = rdna4gl_find_by_sid(&rdna4_gl_surfaces, _sid);
    unsigned h = g ? g->height : 0u;
    pthread_mutex_unlock(&rdna4_gl_mu);
    return h;
}

- (unsigned)surfaceWidth { return [self width]; }
- (unsigned)surfaceHeight { return [self height]; }
- (int)clearDrawable { _sid = 0u; return 0; }
- (int)setSwapInterval:(int)interval limit:(int)limit { (void)interval; (void)limit; return 0; }
- (int)setSwapRectX:(unsigned)x y:(unsigned)y w:(unsigned)w h:(unsigned)h {
    (void)x; (void)y; (void)w; (void)h;
    return 0;
}
- (unsigned long long)windowMode { return 0; }
- (int)signalSharedEvent:(id)e value:(unsigned long long)v operation:(unsigned long long)op {
    (void)op;
    if (e && [e respondsToSelector:@selector(setSignaledValue:)])
        [(id<MTLSharedEvent>)e setSignaledValue:v];
    return 0;
}
RDNA4_DNR(GarnetGLDrawable)
@end
#endif // !defined(GARNET_SYSTEM_REPORT_ONLY)
#if !defined(GARNET_SYSTEM_REPORT_ONLY)
@implementation RDNA4CommandBuffer (Encoders)
- (id)renderCommandEncoderWithDescriptor:(MTLRenderPassDescriptor *)d {
    [self rdna4EnsureGfxSegment];
    (void)[self vk];
    RDNA4LOGR("cb renderCommandEncoderWithDescriptor:");
    return [[RDNA4RenderEncoder alloc] initWithCommandBuffer:self descriptor:d];
}
- (id)blitCommandEncoder {
    [self rdna4EnsureGfxSegment];
    RDNA4LOGR("cb blitCommandEncoder");
    return [[RDNA4BlitEncoder alloc] initWithCommandBuffer:self blit:0];
}
- (id)blitCommandEncoderWithDescriptor:(MTLBlitPassDescriptor *)d {
    [self rdna4EnsureGfxSegment];
    RDNA4LOGR("cb blitCommandEncoderWithDescriptor:");
    return [[RDNA4BlitEncoder alloc] initWithCommandBuffer:self descriptor:d];
}
- (id)computeCommandEncoder {
    if (![self rdna4TrySwitchToComputeQueue]) (void)[self vk];
    RDNA4LOGR("cb computeCommandEncoder");
    return [[RDNA4ComputeEncoder alloc] initWithCommandBuffer:self compute:0];
}
- (id)computeCommandEncoderWithDescriptor:(MTLComputePassDescriptor *)d {
    if (![self rdna4TrySwitchToComputeQueue]) (void)[self vk];
    RDNA4LOGR("cb computeCommandEncoderWithDescriptor:");
    return [[RDNA4ComputeEncoder alloc] initWithCommandBuffer:self descriptor:d];
}
- (id)computeCommandEncoderWithDispatchType:(NSUInteger)t {
    if (![self rdna4TrySwitchToComputeQueue]) (void)[self vk];
    RDNA4LOGR("cb computeCommandEncoderWithDispatchType:%lu", (unsigned long)t);
    return [[RDNA4ComputeEncoder alloc] initWithCommandBuffer:self dispatchType:t];
}
- (id)resourceStateCommandEncoder {
    [self rdna4EnsureGfxSegment];
    return rdna4_cb_new_resource_state_encoder(self, nil);
}
- (id)resourceStateCommandEncoderWithDescriptor:(id)d {
    [self rdna4EnsureGfxSegment];
    return rdna4_cb_new_resource_state_encoder(self, d);
}
- (id)accelerationStructureCommandEncoder {
    if (![self rdna4TrySwitchToComputeQueue]) (void)[self vk];
    RDNA4LOGR("cb accelerationStructureCommandEncoder");
    return [[GarnetAccelerationStructureCommandEncoder alloc] initWithCommandBuffer:self descriptor:nil];
}
- (id)accelerationStructureCommandEncoderWithDescriptor:(id)d {
    if (![self rdna4TrySwitchToComputeQueue]) (void)[self vk];
    RDNA4LOGR("cb accelerationStructureCommandEncoderWithDescriptor:");
    return [[GarnetAccelerationStructureCommandEncoder alloc] initWithCommandBuffer:self descriptor:d];
}
@end

// MTLGPUFamily values: garnet_gate.h (RDNA4G_FAMILY_*, rdna4g_supports_family).

GARNET_INTERNAL NSData *rdna4_function_reflection_data(id function) {
    SEL selector = sel_registerName("reflectionData");
    id data = [function respondsToSelector:selector] ? ((id (*)(id, SEL))objc_msgSend)(function, selector) : nil;
    return [data isKindOfClass:NSData.class] && [data length] ? data : nil;
}

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
    return (id<MTLBinaryArchive>)[[RDNA4BinaryArchive alloc] initWithDevice:self descriptor:descriptor error:error];
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
- (BOOL)supportsDynamicLibraries { return rdna4_dynlib_vft_translator_ready(); }
- (BOOL)supportsRenderDynamicLibraries { return rdna4_dynlib_vft_translator_ready(); }
- (BOOL)supportsBinaryArchives { return rdna4x_active(); }
- (id<MTLDynamicLibrary>)newDynamicLibraryWithURL:(NSURL *)url error:(NSError **)error {
    return (id<MTLDynamicLibrary>)[[RDNA4DynamicLibrary alloc] initWithDevice:self url:url error:error];
}
- (id<MTLDynamicLibrary>)newDynamicLibrary:(id<MTLLibrary>)library error:(NSError **)error {
    return (id<MTLDynamicLibrary>)[[RDNA4DynamicLibrary alloc] initWithDevice:self library:library error:error];
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
        if (!p || (!added && !class_conformsToProtocol(self, p)))
            os_log_error(rdna4_oslog(), "GarnetMetal: +load protocol %{public}s found %d added %d", names[i], p != nil, added);
        else if (rdna4_verbose_log())
            os_log(rdna4_oslog(), "GarnetMetal: +load protocol %{public}s found %d added %d", names[i], p != nil, added);
    }
    const char *bnames[] = { "MTLBuffer", "MTLResource" };   // RDNA4Buffer conformance (no compile-time adoption)
    for (unsigned i = 0; i < sizeof(bnames) / sizeof(bnames[0]); i++) {
        Protocol *p = objc_getProtocol(bnames[i]);
        BOOL added = p ? class_addProtocol([RDNA4Buffer class], p) : NO;
        if (!p || (!added && !class_conformsToProtocol([RDNA4Buffer class], p)))
            os_log_error(rdna4_oslog(), "GarnetMetal: +load RDNA4Buffer protocol %{public}s found %d added %d", bnames[i], p != nil, added);
        else if (rdna4_verbose_log())
            os_log(rdna4_oslog(), "GarnetMetal: +load RDNA4Buffer protocol %{public}s found %d added %d", bnames[i], p != nil, added);
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
    { const char *n[] = { "MTLDynamicLibrary" }; rdna4_add_protocols([RDNA4DynamicLibrary class], n, 1, "RDNA4DynamicLibrary"); }
    { const char *n[] = { "MTLFunctionHandle" }; rdna4_add_protocols([RDNA4FunctionHandle class], n, 1, "RDNA4FunctionHandle"); }
    { const char *n[] = { "MTLVisibleFunctionTable", "MTLResource" }; rdna4_add_protocols([RDNA4VisibleFunctionTable class], n, 2, "RDNA4VisibleFunctionTable"); }
    { const char *n[] = { "MTLCounter" }; rdna4_add_protocols([RDNA4Counter class], n, 1, "RDNA4Counter"); }
    { const char *n[] = { "MTLCounterSet" }; rdna4_add_protocols([RDNA4CounterSet class], n, 1, "RDNA4CounterSet"); }
    { const char *n[] = { "MTLCounterSampleBuffer" }; rdna4_add_protocols([RDNA4CounterSampleBuffer class], n, 1, "RDNA4CounterSampleBuffer"); }
    { const char *n[] = { "MTLHeap" }; rdna4_add_protocols([RDNA4SparseHeap class], n, 1, "RDNA4SparseHeap"); }
    { const char *n[] = { "MTLResourceStateCommandEncoder", "MTLCommandEncoder" }; rdna4_add_protocols([RDNA4ResourceStateEncoder class], n, 2, "RDNA4ResourceStateEncoder"); }
    { const char *n[] = { "MTLAccelerationStructure", "MTLResource" }; rdna4_add_protocols([GarnetAccelerationStructure class], n, 2, "GarnetAccelerationStructure"); }
    { const char *n[] = { "MTLAccelerationStructureCommandEncoder", "MTLCommandEncoder" }; rdna4_add_protocols([GarnetAccelerationStructureCommandEncoder class], n, 2, "GarnetAccelerationStructureCommandEncoder"); }
    { const char *n[] = { "MTLIntersectionFunctionTable", "MTLResource" }; rdna4_add_protocols([RDNA4IntersectionFunctionTable class], n, 2, "RDNA4IntersectionFunctionTable"); }
    { const char *n[] = { "MTLRenderPipelineState" }; rdna4_add_protocols([RDNA4MeshRenderPipelineState class], n, 1, "RDNA4MeshRenderPipelineState"); }
    {   // build 18: the census (garnet_census.h): which instance methods of the running system's Metal protocols do our classes lack? Computed here, logged by a process that gets a device (or printed with RDNA4M_CENSUS=1).
        const rdna4cen_pair pairs[] = { { [RDNA4RenderEncoder class], "RDNA4RenderEncoder", "MTLRenderCommandEncoder" }, { [RDNA4ComputeEncoder class], "RDNA4ComputeEncoder", "MTLComputeCommandEncoder" },
            { [RDNA4BlitEncoder class], "RDNA4BlitEncoder", "MTLBlitCommandEncoder" }, { [RDNA4CommandBuffer class], "RDNA4CommandBuffer", "MTLCommandBuffer" }, { self, "GarnetDevice", "MTLDevice" },
            { [RDNA4Texture class], "RDNA4Texture", "MTLTexture" }, { [RDNA4Buffer class], "RDNA4Buffer", "MTLBuffer" },
            { [RDNA4RenderPipelineState class], "RDNA4RenderPipelineState", "MTLRenderPipelineState" }, { [RDNA4ComputePipelineState class], "RDNA4ComputePipelineState", "MTLComputePipelineState" },   // (extra, beyond the contract: where RenderBox's newVisibleFunctionTableWithDescriptor:stage: lives)
            { [RDNA4CommandQueue class], "RDNA4CommandQueue", "MTLCommandQueue" }, { [RDNA4SamplerState class], "RDNA4SamplerState", "MTLSamplerState" }, { [RDNA4DepthStencilState class], "RDNA4DepthStencilState", "MTLDepthStencilState" },
            { [RDNA4Fence class], "RDNA4Fence", "MTLFence" }, { [RDNA4Heap class], "RDNA4Heap", "MTLHeap" }, { Nil, NULL, NULL } };
        unsigned total = rdna4cen_run(pairs);
        if (garnet_getenv("GARNET_CENSUS", "RDNA4M_CENSUS")) { for (NSString *l in rdna4cen_lines) fprintf(stderr, "%s\n", l.UTF8String); fprintf(stderr, "census: %u selector(s) missing in all\n", total); }
    }
}
// build 18: the census lines saved by +load, logged once by a process that got a device.
static void rdna4_census_log(void) {
    if (!rdna4_verbose_log() && !garnet_getenv("GARNET_CENSUS", "RDNA4M_CENSUS")) return;
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
    if (length == 0) { RDNA4LOG_WARN("newBuffer: refused length"); return nil; }
    if (((options >> 4) & 0xF) > 2) {   // 11e: Shared, Managed, Private
        RDNA4LOG_WARN("newBuffer: storage mode %lu not supported (Memoryless)", (unsigned long)((options >> 4) & 0xF));
        return nil;
    }
    NSError *err = nil;
    RDNA4Buffer *b = [[RDNA4Buffer alloc] initWithDevice:self length:length options:options error:&err];
    if (!b) RDNA4LOG_ERR("newBuffer: failed: %s", err.localizedDescription.UTF8String);
    return b;
}

- (id)newBufferWithBytes:(const void *)bytes length:(NSUInteger)length options:(MTLResourceOptions)options {
    RDNA4LOGR("newBufferWithBytes:length:%lu options:0x%lx", (unsigned long)length, (unsigned long)options);
    RDNA4Buffer *b = [self newBufferWithLength:length options:options];
    if (b && [b contents] && bytes) memcpy([b contents], bytes, length);
    else if (b && bytes) RDNA4LOG_WARN("newBufferWithBytes: buffer has no CPU mapping (Private): contents NOT copied");
    return b;
}

// ---- 11e: samplers ----
- (id)newSamplerStateWithDescriptor:(MTLSamplerDescriptor *)d {
    RDNA4LOGR("newSamplerStateWithDescriptor:");
    NSError *err = nil;
    RDNA4SamplerState *s = [[RDNA4SamplerState alloc] initWithDevice:self descriptor:d error:&err];
    if (!s) RDNA4LOG_ERR("newSamplerState: failed: %s", err.localizedDescription.UTF8String);
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
        RDNA4LOG_WARN("compute pipeline binary archive failure: %s", e.localizedDescription.UTF8String);
        if (error) *error = e;
        return nil;
    }
    const BOOL inproc = (e.code == 41 && rdna4x_active()) ? YES : NO;
    // build 16 (P4): the first attempt of an application that translates in process is NOT a failure: it is a miss in the bundle's own cache, followed by "inproc OK" and a created pipeline (0 COMPUTE FALLBACK lines in the b14 trial).
    if (inproc) RDNA4LOG("compute pipeline: not in the bundle's spvcache (%s); trying the in-process translation", e.localizedDescription.UTF8String);
    else RDNA4LOG_ERR("compute pipeline creation failed: %s", e.localizedDescription.UTF8String);
    const BOOL backendReady = !inproc || rdna4_radv_open(&inprocFailure);
    const uint64_t computeDeadline = rdna4_is_ws()
        ? (rdna4_now() + (uint64_t)RDNA4G_SYNC_WAIT_WS_MS * 1000000ull)
        : UINT64_MAX;
    if (inproc && backendReady && rdna4x_translate_fn_with_error(fn, "kernel", computeDeadline, &inprocFailure)) {   // translate in process first; skip the daemon wait
        NSError *e3 = nil;
        RDNA4ComputePipelineState *p3 = desc
            ? [[RDNA4ComputePipelineState alloc] initWithDevice:self descriptor:desc options:opts error:&e3]
            : [[RDNA4ComputePipelineState alloc] initWithDevice:self function:fn error:&e3];
        if (p3) return p3;
        inprocFailure = e3 ?: rdna4_err(100, @"pipeline initializer returned nil without an NSError after in-process translation");
        RDNA4LOG_ERR("compute pipeline creation failed after the in-process translation: %s", inprocFailure.localizedDescription.UTF8String);
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
            RDNA4LOG_ERR("compute pipeline creation failed after the wait: %s", e2.localizedDescription.UTF8String);
        }
    }
    RDNA4LinkCtx *lk = rdna4_lk_for(fn);
    const BOOL noStage2Fallback = (lk && lk->requireStage2) ? YES : NO;
    if (e.code == 41 && !noStage2Fallback && rdna4_fallback_ok()) {
        // WS/admitted apps need a valid pipeline to avoid MPS/Metal aborts.
        // Its dispatches are temporary no-ops until hot-swap installs the real shader.
        NSMutableArray *paths = [NSMutableArray array], *notes = [NSMutableArray array];
        if (!inproc) rdna4_dump_function(fn, "kernel", paths, notes);
        RDNA4LOG_WARN("COMPUTE FALLBACK %s (%s); %s: %s", ([fn respondsToSelector:@selector(name)] ? [fn name] : @"?").UTF8String, e.localizedDescription.UTF8String, inproc ? "not dumped (in-process translation did not produce it)" : "AIR dumped", [paths componentsJoinedByString:@"; "].UTF8String);
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
        RDNA4LOG_ERR("%s", e.localizedDescription.UTF8String);
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
    if (!t) RDNA4LOG_WARN("newTextureWithDescriptor NIL: pf %lu type %lu usage 0x%lx storage %lu %lux%lux%lu mips %lu array %lu samples %lu (%s)", (unsigned long)d.pixelFormat, (unsigned long)d.textureType,
                          (unsigned long)d.usage, (unsigned long)d.storageMode, (unsigned long)d.width, (unsigned long)d.height, (unsigned long)d.depth, (unsigned long)d.mipmapLevelCount,
                          (unsigned long)d.arrayLength, (unsigned long)d.sampleCount, err.localizedDescription.UTF8String);
    return t;
}

// 11h.6: IOSurface-backed texture (plane 0 of a single-plane 32 bpp surface); nil + log when the surface cannot be imported.
- (id)newTextureWithDescriptor:(MTLTextureDescriptor *)d iosurface:(IOSurfaceRef)s plane:(NSUInteger)plane {
    RDNA4LOGR("newTextureWithDescriptor:iosurface:plane: %lux%lu pf %lu usage 0x%lx plane %lu", (unsigned long)d.width, (unsigned long)d.height, (unsigned long)d.pixelFormat, (unsigned long)d.usage, (unsigned long)plane);
    NSError *err = nil;
    RDNA4Texture *t = [[RDNA4Texture alloc] initWithDevice:self descriptor:d iosurface:s plane:plane error:&err];
    if (!t) RDNA4LOG_WARN("newTextureWithDescriptor:iosurface:plane: NIL: pf %lu type %lu usage 0x%lx storage %lu %lux%lu plane %lu (%s)", (unsigned long)d.pixelFormat, (unsigned long)d.textureType,
                          (unsigned long)d.usage, (unsigned long)d.storageMode, (unsigned long)d.width, (unsigned long)d.height, (unsigned long)plane, err.localizedDescription.UTF8String);
    return t;
}

// ---- 12: client-memory buffers and textures (QuartzCore: -newTiledTextureWithBytesNoCopy:length:descriptor:offset:bytesPerRow:, 4 per second at the login screen) ----
- (id)newBufferWithBytesNoCopy:(void *)p length:(NSUInteger)len options:(MTLResourceOptions)o deallocator:(void (^)(void *, NSUInteger))d {
    RDNA4LOGR("newBufferWithBytesNoCopy:length:%lu options:0x%lx", (unsigned long)len, (unsigned long)o);
    NSError *e = nil; RDNA4Buffer *b = [[RDNA4Buffer alloc] initWithDevice:self bytesNoCopy:p length:len options:o deallocator:d error:&e];
    if (!b) RDNA4LOG_WARN("newBufferWithBytesNoCopy: NIL: %s", e.localizedDescription.UTF8String);   // a refused buffer never owned the memory: the deallocator is NOT called
    return b;
}
- (id)rdna4NoCopyTexture:(void *)p length:(NSUInteger)len deallocator:(void (^)(void *, NSUInteger))dl descriptor:(MTLTextureDescriptor *)d offset:(NSUInteger)off bytesPerRow:(NSUInteger)bpr what:(const char *)what {
    RDNA4Buffer *b = [self newBufferWithBytesNoCopy:p length:len options:MTLResourceStorageModeShared deallocator:dl];
    if (!b) { RDNA4LOG_WARN("%s: NIL (buffer over the client memory refused); pf %lu type %lu usage 0x%lx storage %lu %lux%lu", what, (unsigned long)d.pixelFormat, (unsigned long)d.textureType,
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
    if (!size || !align) return NO;
    uint64_t extent = 0, a = 0;
    if (!rdna4ha_query_extent(mr.size, mr.alignment, RDNA4R.lim.bufferImageGranularity,
                              hostImport ? 1 : 0, RDNA4R.hostAlign, &extent, &a) ||
        extent > NSUIntegerMax || a > NSUIntegerMax)
        return NO;
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
        RDNA4LOG_ERR("heapBufferSizeAndAlignWithLength: RADV open failed: %s; size 0",
                     error.localizedDescription.UTF8String);
        return NO;
    }
    unsigned sm = (unsigned)((opts >> 4) & 0xF);
    if (sm > 2) {
        RDNA4LOG_WARN("heapBufferSizeAndAlignWithLength: storage mode Memoryless is not supported; size 0");
        return NO;
    }
    rdna4dc_caps caps = rdna4_device_caps();
    if (!rdna4dc_buffer_ok(&caps, len)) {
        RDNA4LOG_WARN("heapBufferSizeAndAlignWithLength: length %lu exceeds the Vulkan buffer limit; size 0",
                      (unsigned long)len);
        return NO;
    }
    BOOL hostImport = sm < 2 && len > (64ull << 20);
    if (hostImport && (!RDNA4R.hostExt || !rdna4_vkGetMHPP)) {
        RDNA4LOG_WARN("heapBufferSizeAndAlignWithLength: large Shared/Managed buffers require coherent host import; size 0");
        return NO;
    }
    if (!RDNA4P.bmr2) {
        RDNA4LOG_WARN("heapBufferSizeAndAlignWithLength: vkGetBufferMemoryRequirements2 is unavailable; size 0");
        return NO;
    }
    VkExternalMemoryBufferCreateInfo external = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT };
    VkBufferCreateInfo create = rdna4_heap_buffer_create_info(len, hostImport ? &external : NULL);
    VkBuffer buffer = VK_NULL_HANDLE;
    VkResult result = vkCreateBuffer(RDNA4R.dev, &create, NULL, &buffer);
    if (result != VK_SUCCESS || !buffer) {
        RDNA4LOG_ERR("heapBufferSizeAndAlignWithLength: vkCreateBuffer = %d handle %p; size 0",
                     result, (void *)buffer);
        return NO;
    }
    VkMemoryRequirements mr;
    BOOL queried = rdna4_requirements2(buffer, VK_NULL_HANDLE, &mr, NULL, NULL, YES);
    vkDestroyBuffer(RDNA4R.dev, buffer, NULL);
    if (!queried) {
        RDNA4LOG_ERR("heapBufferSizeAndAlignWithLength: buffer requirements2 query failed; size 0");
        return NO;
    }
    if (!rdna4_heap_query_extent(mr, hostImport, size, align)) {
        RDNA4LOG_ERR("heapBufferSizeAndAlignWithLength: invalid buffer requirement (size %llu alignment %llu); size 0",
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
        RDNA4LOG_ERR("heapTextureSizeAndAlignWithDescriptor: RADV open failed: %s; size 0",
                     error.localizedDescription.UTF8String);
        return NO;
    }
    if (!RDNA4P.imr2) {
        RDNA4LOG_WARN("heapTextureSizeAndAlignWithDescriptor: vkGetImageMemoryRequirements2 is unavailable; size 0");
        return NO;
    }
    rdna4_heap_image_plan plan;
    if (!rdna4_heap_image_plan_make(d, YES, &plan, &error)) {
        RDNA4LOG_WARN("heapTextureSizeAndAlignWithDescriptor: %s; size 0",
                      error.localizedDescription.UTF8String);
        return NO;
    }
    VkImage image = VK_NULL_HANDLE;
    VkResult result = vkCreateImage(RDNA4R.dev, &plan.create, NULL, &image);
    if (result != VK_SUCCESS || !image) {
        RDNA4LOG_ERR("heapTextureSizeAndAlignWithDescriptor: vkCreateImage = %d handle %p; size 0",
                     result, (void *)image);
        return NO;
    }
    VkMemoryRequirements mr;
    BOOL queried = rdna4_requirements2(VK_NULL_HANDLE, image, &mr, NULL, NULL, YES);
    vkDestroyImage(RDNA4R.dev, image, NULL);
    if (!queried) {
        RDNA4LOG_ERR("heapTextureSizeAndAlignWithDescriptor: image requirements2 query failed; size 0");
        return NO;
    }
    if (!rdna4_heap_query_extent(mr, NO, size, align)) {
        RDNA4LOG_ERR("heapTextureSizeAndAlignWithDescriptor: invalid image requirement (size %llu alignment %llu); size 0",
                     (unsigned long long)mr.size, (unsigned long long)mr.alignment);
        *size = *align = 0;
        return NO;
    }
    return YES;
}

// ---- m11 H1: heaps (see RDNA4Heap / RDNA4SparseHeap) ----
- (id)newHeapWithDescriptor:(MTLHeapDescriptor *)d {
    RDNA4LOG("newHeapWithDescriptor: size %lu storage %lu cache %lu hazard %lu type %lu", (unsigned long)d.size, (unsigned long)d.storageMode, (unsigned long)d.cpuCacheMode, (unsigned long)d.hazardTrackingMode, (unsigned long)d.type);
    if (d && (NSInteger)d.type == RDNA4SP_HEAP_SPARSE) return rdna4_sparse_heap_new(self, d, NULL);
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
- (BOOL)supportsRaytracing { return rdna4_rt_runtime_supports_raytracing(); }
- (BOOL)supportsRaytracingFromRender { return rdna4_rt_runtime_supports_raytracing_from_render(); }
- (BOOL)supportsPrimitiveMotionBlur { return rdna4_rt_runtime_supports_primitive_motion_blur(); }
- (BOOL)supportsFunctionPointers { return rdna4_dynlib_vft_translator_ready(); }
- (BOOL)supportsFunctionPointersFromRender { return rdna4_dynlib_vft_translator_ready(); }
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
- (NSUInteger)sparseTileSizeInBytes { return rdna4_device_sparse_tile_size_in_bytes(); }
- (NSUInteger)sparseTileSizeInBytesForSparsePageSize:(NSInteger)sparsePageSize {
    return rdna4_device_sparse_tile_size_for_page_size(sparsePageSize);
}
- (MTLSize)sparseTileSizeWithTextureType:(MTLTextureType)textureType
                             pixelFormat:(MTLPixelFormat)pixelFormat
                             sampleCount:(NSUInteger)sampleCount {
    return rdna4_device_sparse_tile_size(textureType, pixelFormat, sampleCount);
}
- (MTLSize)sparseTileSizeWithTextureType:(MTLTextureType)textureType
                             pixelFormat:(MTLPixelFormat)pixelFormat
                             sampleCount:(NSUInteger)sampleCount
                          sparsePageSize:(NSInteger)sparsePageSize {
    return rdna4_device_sparse_tile_size_with_page_size(textureType, pixelFormat, sampleCount, sparsePageSize);
}
- (void)convertSparsePixelRegions:(const MTLRegion *)pixelRegions
                    toTileRegions:(MTLRegion *)tileRegions
                     withTileSize:(MTLSize)tileSize
                    alignmentMode:(MTLSparseTextureRegionAlignmentMode)mode
                       numRegions:(NSUInteger)numRegions {
    rdna4_device_convert_sparse_pixel_regions(pixelRegions, tileRegions, tileSize, mode, numRegions);
}
- (void)convertSparseTileRegions:(const MTLRegion *)tileRegions
                  toPixelRegions:(MTLRegion *)pixelRegions
                    withTileSize:(MTLSize)tileSize
                      numRegions:(NSUInteger)numRegions {
    rdna4_device_convert_sparse_tile_regions(tileRegions, pixelRegions, tileSize, numRegions);
}
- (MTLAccelerationStructureSizes)accelerationStructureSizesWithDescriptor:(MTLAccelerationStructureDescriptor *)d {
    return rdna4_rt_device_sizes(self, d);
}
- (id<MTLAccelerationStructure>)newAccelerationStructureWithSize:(NSUInteger)size {
    return rdna4_rt_device_new_with_size(self, size);
}
- (id<MTLAccelerationStructure>)newAccelerationStructureWithDescriptor:(MTLAccelerationStructureDescriptor *)d {
    return rdna4_rt_device_new_with_descriptor(self, d);
}
- (MTLSizeAndAlign)heapAccelerationStructureSizeAndAlignWithSize:(NSUInteger)size {
    return rdna4_rt_device_heap_as_size_align_with_size(self, size);
}
- (MTLSizeAndAlign)heapAccelerationStructureSizeAndAlignWithDescriptor:(MTLAccelerationStructureDescriptor *)d {
    return rdna4_rt_device_heap_as_size_align_with_descriptor(self, d);
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
    if (!q) RDNA4LOG_ERR("newCommandQueue: failed: %s", err.localizedDescription.UTF8String);
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
    else RDNA4LOG_ERR("pipeline creation failed: %s", e.localizedDescription.UTF8String);
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
- (id<MTLRenderPipelineState>)newRenderPipelineStateWithMeshDescriptor:(id)d options:(NSUInteger)opts reflection:(MTLRenderPipelineReflection **)refl error:(NSError **)error {
    RDNA4LOG("newRenderPipelineStateWithMeshDescriptor:options:reflection:error:");
    return (id<MTLRenderPipelineState>)rdna4_mesh_device_new_pipeline(self, d, opts, refl, error);
}
- (void)newRenderPipelineStateWithMeshDescriptor:(id)d options:(NSUInteger)opts completionHandler:(void (^)(id, id, NSError *))h {
    RDNA4LOG("newRenderPipelineStateWithMeshDescriptor:options:completionHandler:");
    rdna4_mesh_device_new_pipeline_async(self, d, opts, h);
}

// Diagnostic: name any selector Metal sends that we (and our superclasses) do not implement, before the
// standard exception. Seen: -[_MTLDevice initLimits] -> doesNotRecognizeSelector.
- (void)doesNotRecognizeSelector:(SEL)sel {
    os_log_error(rdna4_oslog(), "GarnetMetal: UNRECOGNIZED selector %{public}s", sel_getName(sel));
    if (!RDNA4_RELEASE || rdna4_verbose_log())
        NSLog(@"GarnetMetal: UNRECOGNIZED selector %s", sel_getName(sel));
    [super doesNotRecognizeSelector:sel];
}

- (instancetype)initWithAcceleratorPort:(uint32_t)port {
    if (!RDNA4_RELEASE || rdna4_verbose_log()) {
        os_log(rdna4_oslog(), "GarnetMetal: initWithAcceleratorPort 0x%x", port);
        NSLog(@"GarnetMetal: initWithAcceleratorPort 0x%x", port);
    }
    const char *why = rdna4_admit(port);   // 11b: process filter, kill file, Ready, crash counter
    self = [super initWithAcceleratorPort:port];
    if (why) {
        // Declining BEFORE the base init crashed (measured: the un-initialised object is released by ARC and
        // -[_MTLDevice dealloc] -> dispatch_release(NULL) SEGVs), so the base init runs first and the object is then
        // dropped: ARC releases the fully initialised self and Metal sees nil.
        RDNA4LOG_WARN("initWithAcceleratorPort 0x%x DECLINED in %s (pid %d): %s", port, getprogname(), getpid(), why);
        return nil;
    }
    if (self) {
        _rdna4AcceleratorPort = port;
        (void)IORegistryEntryGetRegistryEntryID((io_registry_entry_t)port, &_rdna4RegistryID);
        _rdna4Model = rdna4_model_name(port);
        _rdna4NativeMemory = rdna4_nub_memory(port);
        rdna4_argument_limit_prepare(&_rdna4NativeMemory);
    }
    if (!self) RDNA4LOG_ERR("initWithAcceleratorPort 0x%x -> nil", port);
    else if (rdna4_is_ws() || !RDNA4_RELEASE || rdna4_verbose_log()) RDNA4LOG_KEY("initWithAcceleratorPort 0x%x -> ok", port);
    if (self && rdna4_is_ws()) (void)rdna4_ca_sw_compat();
    if (self && rdna4_is_ws()) {
        uint32_t inheritedPort = ((uint32_t (*)(id, SEL))objc_msgSend)(
            self, sel_getUid("acceleratorPort"));
        rdna4_coredisplay_compat(inheritedPort);
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
    // Mac1..2, Common1..3, and Metal3 -> YES; Apple1..10 (incl. Apple6), MacCatalyst, Metal4, unknown -> NO.
    return rdna4g_supports_family((long)family, rdna4_is_ws() ? 1 : 0) ? YES : NO;
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
#endif // !defined(GARNET_SYSTEM_REPORT_ONLY)
