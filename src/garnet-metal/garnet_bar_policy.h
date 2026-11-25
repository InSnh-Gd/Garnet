// garnet_bar_policy.h — Pure C dual-BAR policy (256 MiB Small-BAR vs >= 4 GiB Full-BAR).
// No macOS/ObjC/Vulkan runtime dependencies; host test: test-bar-policy.c.
#ifndef GARNET_BAR_POLICY_H
#define GARNET_BAR_POLICY_H

#include <stdint.h>

#define GARNET_BAR_FULL_MIN_MB          4096ULL
#define GARNET_VRAM_SURF_CAP_SMALL_BAR  (512ULL << 20)
#define GARNET_VRAM_SURF_CAP_FULL_BAR   (1536ULL << 20)

typedef struct {
    int full_bar;                 // 1 when BAR0,FullMode=true and BAR0 >= 4096 MiB
    int enable_nosam;             // 1 => set RADV_PERFTEST=nosam (Small-BAR default; Full-BAR off)
    uint64_t vram_surf_cap_bytes; // Phase B OPTIMAL VRAM surface cache cap
} garnet_bar_policy_t;

// Decide BAR-dependent knobs from IORegistry probe + user file overrides.
// Fail-closed: if IORegistry probe fails (0, 0), treat as Small-BAR (nosam ON, 512 MiB cap).
// Precedence for nosam: force_nosam (/private/tmp/garnet-nosam) > force_nosam_off (/private/tmp/garnet-nosam-off) > auto.
static inline garnet_bar_policy_t garnet_bar_choose_policy(int bar0_full_mode,
                                                           uint64_t bar0_mb,
                                                           int force_nosam,
                                                           int force_nosam_off) {
    const int full = (bar0_full_mode != 0 && bar0_mb >= GARNET_BAR_FULL_MIN_MB) ? 1 : 0;
    const int nosam = force_nosam ? 1 : (force_nosam_off ? 0 : (full ? 0 : 1));
    const uint64_t cap = full ? GARNET_VRAM_SURF_CAP_FULL_BAR : GARNET_VRAM_SURF_CAP_SMALL_BAR;
    return (garnet_bar_policy_t){ full, nosam, cap };
}

#endif
