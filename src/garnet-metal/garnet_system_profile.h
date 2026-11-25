#ifndef RDNA4_SYSTEM_PROFILE_H
#define RDNA4_SYSTEM_PROFILE_H

#include <stdint.h>
#include <stdio.h>

static inline uint64_t rdna4sp_pcie_rate(uint64_t link_capabilities) {
    static const uint32_t gt_per_second[] = {0, 2500, 5000, 8000, 16000, 32000};
    uint32_t generation = (uint32_t)(link_capabilities & 0xfu);
    uint32_t lanes = (uint32_t)((link_capabilities >> 4) & 0x3fu);
    if (!generation || generation >= sizeof gt_per_second / sizeof gt_per_second[0] ||
        !lanes || lanes > 32) return 0;
    uint64_t rate = (uint64_t)gt_per_second[generation] * 1000000ull * lanes;
    return generation <= 2 ? rate / 10 : rate * 128 / 130 / 8;
}

static inline uint32_t rdna4sp_pci_location_number(const char *pcidebug) {
    unsigned domain, bus, device;
    char tail;
    if (!pcidebug || sscanf(pcidebug, "%u:%u:%u%c", &domain, &bus, &device, &tail) < 3 ||
        device > 31) return 0;
    (void)domain;
    (void)bus;
    return device;
}

#endif
