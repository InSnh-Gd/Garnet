//
//  fbname.hpp — the runtime framebuffer class rename. DEFAULT OFF.
//
//  `accel fbname <0|1>`:
//     0  disarm and report — restores the original class name unconditionally, verifies the restore
//        by reading the name back, and is safe to call from any state including "never armed".
//     1  arm — replace the framebuffer's OSMetaClass className pointer with an interned OSSymbol for
//        a name containing "AMD", so CoreDisplay's GetGPUVendorForFramebufferService resolves our
//        vendor as AMD (bit 0x4) instead of unknown (bit 0x1).
//        The object must be the identity-verified target-GPU primary AND its live metaclass must be
//        the registered local RDNA4FB leaf. Apple's shared IONDRVFramebuffer (or any other class)
//        is refused with FBN_UNPROVEN_LEAF; the 4.2 display migration never renames it.
//
//  This edits a live kernel structure. Every guard is in fbname.cpp and the load-bearing
//  search logic is host-tested in src/garnet/tests/fbname_scan_test.cpp. It is NOT to be run
//  without an adversarial review.
//
#ifndef RDNA4_FBNAME_HPP
#define RDNA4_FBNAME_HPP

#include <stdint.h>

namespace rdna4fbname {

// The name we swap in. Case-sensitive "AMD" is what CFStringFind looks for. Chosen to be
// obviously ours and NOT to collide with system AMDFramebuffer / stock driver classes.
#define RDNA4_PRIMARY_FBNAME_NEW "AMDGarnetPrimaryFramebuffer"
// The names that may legitimately be the framebuffer's leaf class before we touch anything.
#define RDNA4_PRIMARY_FBNAME_OLD "GarnetPrimaryFramebuffer"

// The rename is legal only on our local leaf metaclass. Apple's generic IONDRVFramebuffer
// (and every other Apple framebuffer class) is shared with unrelated devices; changing its
// metaclass name would mutate Apple's class globally. The 4.2 display migration leaves the
// generic class in place, so this branch must explicitly refuse rather than widen the allowlist.
inline bool fbname_leaf_is_local(const char *name) {
	return name && (!__builtin_strcmp(name, RDNA4_PRIMARY_FBNAME_OLD) ||
	                !__builtin_strcmp(name, RDNA4_PRIMARY_FBNAME_NEW));
}
inline bool fbname_leaf_is_apple_shared(const char *name) {
	return name && (!__builtin_strcmp(name, "IONDRVFramebuffer") || !__builtin_strcmp(name, "IOFramebuffer") ||
	                !__builtin_strcmp(name, "AMDFramebuffer"));
}

uint32_t control(uint64_t arg, uint64_t *out, unsigned outCount);

// Called from the kext's stop path: restores the original name if still armed. Leaving a patched
// className behind in a kext that is going away would leave a dangling OSSymbol pointer.
void restore_on_unload(void);

}  // namespace rdna4fbname

#endif /* RDNA4_FBNAME_HPP */
