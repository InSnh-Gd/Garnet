// Temporary [DEBUG-render] observations for the 25G332 desktop corruption.
// Read cached policy and the main display's accelerator; never select a compositor.
#ifndef RDNA4_COMPOSITOR_OBSERVATION_H
#define RDNA4_COMPOSITOR_OBSERVATION_H

#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach-o/loader.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#if defined(__x86_64__)
static const uint8_t rdna4_observation_skylight_uuid[16] = {
    0x9a,0x58,0xd4,0x6f,0x26,0xdb,0x32,0x7e,
    0xbd,0xfe,0xa4,0x65,0xf0,0x01,0x45,0x2f
};
static const uint8_t rdna4_observation_coredisplay_uuid[16] = {
    0xa9,0x59,0xf4,0x16,0xcf,0x8a,0x3c,0x15,
    0x92,0x50,0x72,0x5c,0x60,0xfb,0xd0,0x9c
};

static const uint8_t *rdna4_observation_image(const void *symbol,
                                            const uint8_t uuid[16]) {
    Dl_info image;
    if (!symbol || !dladdr(symbol, &image)) return NULL;
    const struct mach_header_64 *header = image.dli_fbase;
    if (header->magic != MH_MAGIC_64) return NULL;
    const uint8_t *cursor = (const void *)(header + 1);
    const uint8_t *end = cursor + header->sizeofcmds;
    for (uint32_t i = 0; i < header->ncmds; ++i) {
        if ((size_t)(end - cursor) < sizeof(struct load_command)) return NULL;
        const struct load_command *command = (const void *)cursor;
        if (command->cmdsize < sizeof(*command) ||
            command->cmdsize > (size_t)(end - cursor)) return NULL;
        if (command->cmd == LC_UUID) {
            if (command->cmdsize < sizeof(struct uuid_command)) return NULL;
            return memcmp(((const struct uuid_command *)command)->uuid, uuid, 16)
                ? NULL : (const void *)header;
        }
        cursor += command->cmdsize;
    }
    return NULL;
}

static int rdna4_observation_read(const uint8_t *base, uintptr_t offset,
                                  void *value, size_t size) {
    mach_vm_size_t copied = 0;
    return mach_vm_read_overwrite(mach_task_self(),
        (mach_vm_address_t)(uintptr_t)base + offset, size,
        (mach_vm_address_t)(uintptr_t)value, &copied) == KERN_SUCCESS && copied == size;
}

static int rdna4_compositor_snapshot(int server) {
    const uint8_t *base = rdna4_observation_image(
        dlsym(RTLD_DEFAULT, "SLSMainConnectionID"), rdna4_observation_skylight_uuid);
    int32_t active = -1;
    uint8_t ignore = 0, metal = 0;
    int64_t once = 0;
    // File offsets in SkyLight UUID 9A58D46F-26DB-327E-BDFE-A465F001452F.
    if (!base ||
        !rdna4_observation_read(base, 0x3aa0fbcc, &active, sizeof active) ||
        !rdna4_observation_read(base, 0x3aa0fbc8, &ignore, sizeof ignore) ||
        !rdna4_observation_read(base, 0x3aa10268, &metal, sizeof metal) ||
        !rdna4_observation_read(base, 0x3aa10270, &once, sizeof once)) {
        RDNA4LOG("[DEBUG-render] compositor observation unavailable: unsupported or unreadable SkyLight");
        return 0;
    }
    RDNA4LOG("[DEBUG-render] policy active=%d ignoreAcceleration=%u metalCapable=%u once=%lld",
             active, ignore, metal, (long long)once);
    if (!server || once != -1) return 1;

    void *(*main_display)(void) = dlsym(RTLD_DEFAULT, "CGXMainDisplayDevice");
    int (*accelerator)(const void *, int32_t, uint32_t *, uint32_t *) =
        dlsym(RTLD_DEFAULT, "CGXAcceleratorForDisplayDevice");
    const uint8_t *display_image = rdna4_observation_image(
        (const void *)main_display, rdna4_observation_coredisplay_uuid);
    if (!display_image || rdna4_observation_image((const void *)accelerator,
            rdna4_observation_coredisplay_uuid) != display_image) {
        RDNA4LOG("[DEBUG-render] main display observation unavailable: unsupported CoreDisplay");
        return 0;
    }
    const void *display = main_display();
    uint32_t port = 0, identifier = 0;
    int result = accelerator(display, -1, &port, &identifier);
    RDNA4LOG("[DEBUG-render] main display=%p acceleratorResult=%d port=0x%x identifier=0x%x",
             display, result, port, identifier);
    return 1;
}
#endif

static void rdna4_compositor_observe(void) {
#if defined(__x86_64__)
    // CoreDisplay server callbacks exist only in the real WindowServer.
    if (geteuid() != 88 || strcmp(getprogname(), "WindowServer")) return;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC),
                       dispatch_get_main_queue(), ^{ (void)rdna4_compositor_snapshot(1); });
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 15 * NSEC_PER_SEC),
                       dispatch_get_main_queue(), ^{ (void)rdna4_compositor_snapshot(1); });
    });
#endif
}

#endif
