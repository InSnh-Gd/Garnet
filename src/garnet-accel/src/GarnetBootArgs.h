// GarnetBootArgs.h - boot-arg migration fallback for the rdna4-* -> garnet-* rename.
//
// Every kext boot-arg read stays written as PE_parse_boot_argn("garnet-...") (the host suites assert
// that exact text). In a KERNEL C++ build this header is force-included (-include in the kext
// Makefiles), so each read first asks for the new name and, only when the new name is absent, for
// the legacy name (garnet -> rdna4bringup, garnet-X -> rdna4-X). A legacy hit is logged so old
// config.plist tokens show up in the kernel log. When both are present the new one wins.
// The header is shared byte for byte by src/garnet/src and src/garnet-accel/src
// (tests/bootargs_alias_test.cpp checks both copies).
#ifndef GARNET_BOOT_ARGS_H
#define GARNET_BOOT_ARGS_H

#ifdef __cplusplus
// Pure: write the legacy spelling of a current boot-arg name into out (NUL-terminated).
// Returns false (out = "") when the name has no legacy spelling or out is too small.
static inline bool garnet_legacy_boot_arg_name(const char *name, char *out, unsigned long cap) {
    static const char kNew[] = "garnet", kOld[] = "rdna4";
    if (!name || !out || cap == 0) return false;
    out[0] = '\0';
    for (unsigned i = 0; i < sizeof(kNew) - 1; i++) if (name[i] != kNew[i]) return false;
    const char *rest = name + sizeof(kNew) - 1;
    // only the two spellings the rename produced: "garnet" and "garnet-<x>"
    bool primary = (rest[0] == '\0');
    if (!primary && !(rest[0] == '-' && rest[1] != '\0')) return false;
    static const char kLegacyRoot[] = "rdna4bringup";
    const char *src = primary ? kLegacyRoot : kOld;
    unsigned long n = 0;
    for (const char *p = src; *p; p++) { if (n + 1 >= cap) { out[0] = '\0'; return false; } out[n++] = *p; }
    if (!primary) {
        for (const char *p = rest; *p; p++) { if (n + 1 >= cap) { out[0] = '\0'; return false; } out[n++] = *p; }
    }
    out[n] = '\0';
    return true;
}

#if defined(KERNEL)
#include <pexpert/pexpert.h>
#include <IOKit/IOLib.h>
static inline bool garnet_parse_boot_argn(const char *name, void *val, int size) {
    if (PE_parse_boot_argn(name, val, size)) {
        char old[64];
        unsigned char scratch[16];
        if (size > 0 && size <= (int)sizeof(scratch) && garnet_legacy_boot_arg_name(name, old, sizeof(old)) &&
            PE_parse_boot_argn(old, scratch, size))
            IOLog("Garnet: boot-arg %s overrides deprecated %s\n", name, old);
        return true;
    }
    char old[64];
    if (!garnet_legacy_boot_arg_name(name, old, sizeof(old)) || !PE_parse_boot_argn(old, val, size)) return false;
    IOLog("Garnet: boot-arg %s is deprecated; use %s\n", old, name);
    return true;
}
// After the declaration above: every later call in this translation unit goes through the fallback.
#define PE_parse_boot_argn(n, v, s) garnet_parse_boot_argn((n), (v), (s))
#endif // KERNEL
#endif // __cplusplus
#endif // GARNET_BOOT_ARGS_H
