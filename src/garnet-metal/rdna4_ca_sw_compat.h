// macOS 25G332: SWContext's flattened IOSurface import omits track_surface.
// Match the exact QuartzCore image and original slot before changing this process.
#ifndef RDNA4_CA_SW_COMPAT_H
#define RDNA4_CA_SW_COMPAT_H

#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach-o/loader.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#if defined(__x86_64__)
typedef void *(*rdna4_ca_sw_import_fn)(void *, int, int, int, void *);
static _Atomic(rdna4_ca_sw_import_fn) rdna4_ca_sw_original;
static void (*rdna4_ca_sw_track)(void *, void *);

static void *rdna4_ca_sw_import(void *context, int kind, int width, int height,
                              void *iosurface) {
    rdna4_ca_sw_import_fn original = atomic_load_explicit(
        &rdna4_ca_sw_original, memory_order_acquire);
    void *surface = original(context, kind, width, height, iosurface);
    if (surface) rdna4_ca_sw_track(context, surface);
    return surface;
}

static int rdna4_ca_sw_matching_image(const void *base) {
    static const uint8_t uuid[16] = {
        0x51,0x6d,0xa0,0xce,0xe1,0x22,0x3d,0xb8,
        0x8a,0x17,0x05,0xaf,0x12,0xeb,0x86,0xeb
    };
    const struct mach_header_64 *header = base;
    if (header->magic != MH_MAGIC_64) return 0;
    const struct load_command *command = (const void *)(header + 1);
    for (uint32_t i = 0; i < header->ncmds; ++i) {
        if (command->cmd == LC_UUID)
            return memcmp(((const struct uuid_command *)command)->uuid,
                          uuid, sizeof uuid) == 0;
        command = (const void *)((const char *)command + command->cmdsize);
    }
    return 0;
}

static int rdna4_ca_sw_install(void) {
    void *factory = dlsym(RTLD_DEFAULT, "CARenderOGLNew");
    Dl_info image;
    if (!factory || !dladdr(factory, &image) ||
        !rdna4_ca_sw_matching_image(image.dli_fbase)) return 0;

    uint8_t *base = image.dli_fbase;
    // Offsets belong only to UUID 516DA0CE-E122-3DB8-8A17-05AF12EB86EB.
    _Atomic(rdna4_ca_sw_import_fn) *slot =
        (void *)(base + 0x33cac858 + 0x2d8);
    rdna4_ca_sw_import_fn original = atomic_load_explicit(slot, memory_order_acquire);
    if (original != (rdna4_ca_sw_import_fn)(base + 0x1ea9ec)) {
        RDNA4LOG("CA SW compatibility refused: unexpected IOSurface import slot");
        return -1;
    }

    mach_vm_address_t address = (mach_vm_address_t)(uintptr_t)slot;
    mach_vm_address_t region = address;
    mach_vm_size_t region_size = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    kern_return_t kr = mach_vm_region(mach_task_self(), &region, &region_size,
        VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &count, &object);
    if (MACH_PORT_VALID(object)) mach_port_deallocate(mach_task_self(), object);
    if (kr != KERN_SUCCESS || address < region || region_size < sizeof(*slot) ||
        address - region > region_size - sizeof(*slot) ||
        !(info.protection & VM_PROT_READ)) {
        RDNA4LOG("CA SW compatibility refused: cannot inspect import page (%d)", kr);
        return -1;
    }
    mach_vm_size_t page_size = vm_page_size;
    mach_vm_address_t page = address & ~(page_size - 1);
    kr = mach_vm_protect(mach_task_self(), page, page_size, FALSE,
                        info.protection | VM_PROT_WRITE | VM_PROT_COPY);
    if (kr != KERN_SUCCESS) {
        RDNA4LOG("CA SW compatibility refused: cannot copy import page (%d)", kr);
        return -1;
    }
    rdna4_ca_sw_track = (void *)(base + 0x16ffa6);
    atomic_store_explicit(&rdna4_ca_sw_original, original, memory_order_release);
    atomic_store_explicit(slot, rdna4_ca_sw_import, memory_order_release);
    kr = mach_vm_protect(mach_task_self(), page, page_size, FALSE, info.protection);
    if (kr != KERN_SUCCESS) {
        atomic_store_explicit(slot, original, memory_order_release);
        (void)mach_vm_protect(mach_task_self(), page, page_size, FALSE, info.protection);
        RDNA4LOG("CA SW compatibility refused: cannot restore import page (%d)", kr);
        return -1;
    }
    RDNA4LOG("CA SW compatibility installed: track imported flattened IOSurfaces");
    return 1;
}
#endif

// Called only for an admitted WindowServer device. No framework file is changed.
static int rdna4_ca_sw_compat(void) {
#if defined(__x86_64__)
    static dispatch_once_t once;
    static int result;
    dispatch_once(&once, ^{ result = rdna4_ca_sw_install(); });
    return result;
#else
    return 0;
#endif
}

#endif
