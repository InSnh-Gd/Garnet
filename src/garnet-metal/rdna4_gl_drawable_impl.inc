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

@interface GarnetGLDrawable : NSObject {
    uint32_t _sid;
}
@end

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
