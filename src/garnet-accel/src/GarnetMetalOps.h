/*
 *  GarnetMetalOps.h - the ONE interface between Garnet.kext (owner of the GPU, ESP-injected: rebuilds are free) and GarnetAccel.kext
 *  (the IOAccelerator object graph; ANY rebuild requires a security approval (Allow click) from the user), driver contract
 *  BYTE-IDENTICAL copies live in src/garnet/src/ and src/garnet-accel/src/ (both host tests compare them). Plain C.
 *
 *  RULE: every decision that may change during #9 / #10 lives behind this table in Garnet.kext; the aux kext is a fixed class graph, the gates
 *  and vtable-shaped trampolines that forward into the hooks. The table is VERSIONED and APPEND-ONLY: a consumer accepts abi >=
 *  GARNET_METAL_ABI_MIN and size >= GARNET_METAL_OPS_MIN; members are appended, never moved. A NULL optional hook means "not provided": the aux
 *  kext then takes its documented fail-closed default (see garnetaccel_pure.h). ABI 2 (0.0.613) appended the display-pipe members; the minimum accepted ABI stays 1
 *  (GARNET_METAL_ABI_MIN), and an ABI-1 table means "display off". `accel` / `obj` arguments are identity-only opaque pointers.
 *
 *  Reached through the nub: nub->callPlatformFunction(GARNET_METAL_FN_SYMBOL, false, &ops (a const struct GarnetMetalOps pointer), 0, 0, 0) returns
 *  kIOReturnSuccess and stores a pointer to a STATIC const table (valid while Garnet.kext is loaded).
 *
 *  ABI 3 (bring-up 0.0.656 / aux 0.0.5, G6): appends native_open (below) with its own capability bit GARNET_CAP_NATIVE_OPEN. The aux accelerator's newUserClient (slot 239) hands a
 *  type 'GRNT' open to it, so a SANDBOXED application can open the native client on the IOAccelerator service (IOAccelerator user-client routing).
 */
#ifndef GARNET_METAL_OPS_H
#define GARNET_METAL_OPS_H

#include <stdint.h>

#define GARNET_METAL_FN_SYMBOL   "garnet.metal.ops"
#define GARNET_METAL_NUB_CLASS   "GarnetMetalNub"
#define GARNET_METAL_ABI         3u                   /* 3 since bring-up 0.0.656 / aux 0.0.5 (native_open appended at 144); 2 since 0.0.613 / aux 0.0.3: the display-pipe members appended at 120 (below) */
#define GARNET_METAL_UC_GARNETN  0x47524E54u          /* the user-client type 'GRNT' (= GARNETN_UC_TYPE of GarnetNativeABI.h; Garnet.kext static_asserts the equality) */
#define GARNET_METAL_ABI_MIN     1u                   /* the oldest table a consumer accepts: an ABI-1 table (120 bytes) is valid and means "display off" */
#define GARNET_METAL_OPS_MAGIC   0x4F4E5247u          /* 'GRNO' little-endian */

#define GARNET_METAL_F_SOFTWARE_ONLY (1u << 0)        /* no submit / VRAM entry points exist yet (#9) */

/* caps (capability bitmask, set by Garnet.kext): the aux kext uses a hook only when its bit is set */
#define GARNET_CAP_VHOOK         (1ull << 0)          /* vhook (below) may be called */
#define GARNET_CAP_NATIVE_OPEN   (1ull << 1)          /* ABI 3: native_open (below) may be called */

/* factory_mask bits: which optional object factories of the accelerator may create objects (default 0 = every one returns NULL) */
#define GARNET_FACT_SYSMEMORY    (1u << 0)
#define GARNET_FACT_MEMORYMAP    (1u << 1)
#define GARNET_FACT_VIDMEMORY    (1u << 2)
#define GARNET_FACT_RESOURCE     (1u << 3)
#define GARNET_FACT_CTX2D        (1u << 4)
#define GARNET_FACT_SHAREDUC     (1u << 5)   /* newSharedUserClient makes the aux subclass (else the family's own object) */
#define GARNET_FACT_CMDQUEUE     (1u << 6)   /* newCommandQueue makes the aux subclass (else the family's own object) */
#define GARNET_FACT_ALL          0x7fu
/* vhook classes (cls argument): the aux trampoline classes */
#define GARNET_VC_EVENTMACHINE   1u
#define GARNET_VC_SHAREDUC       2u
#define GARNET_VC_CMDQUEUE       3u
#define GARNET_VC_VIDMEMORY      4u
#define GARNET_VC_RESOURCE       5u
#define GARNET_VC_CTX2D          6u
#define GARNET_VC_DISPLAYPIPE    7u   /* ABI 2: GarnetDisplayPipe slots 267 / 277 / 278 / 279, through disp_hook (never vhook) */
/* mm_hook ops */
#define GARNET_MM_COMMIT   0u
#define GARNET_MM_RELEASE  1u
#define GARNET_MM_UPDATE   2u
/* trace events (aux -> Garnet; Garnet.kext decides what to log) */
#define GARNET_TR_PROBE_OK       1u
#define GARNET_TR_DEVICE_OPEN    2u
#define GARNET_TR_DEVICE_CLOSE   3u
#define GARNET_TR_POPULATE       4u
#define GARNET_TR_STAMP          5u
#define GARNET_TR_EM_INIT        6u
#define GARNET_TR_TASK           7u
#define GARNET_TR_FACTORY        8u
#define GARNET_TR_DISPLAY        9u
#define GARNET_TR_STARTED       10u
#define GARNET_TR_MM             11u
#define GARNET_TR_STUB           12u
#define GARNET_TR_DISPPIPE      13u   /* ABI 2: newDisplayPipe; a = the pipe returned, b = 1 a GarnetDisplayPipe / 0 the family's own (display off or allocation failed) */
#define GARNET_TR_DM_START      14u   /* ABI 2: GarnetDisplayMachine::start; a = the provider the family walk starts from, b = 1 the GPU's PCI device was substituted for the nub */
#define GARNET_TR_NATIVE_OPEN   15u   /* ABI 3: GarnetAccelerator::newUserClient intercepted a 'GRNT' open; a = the IOReturn handed back (0 = a client was returned), b = the client pointer (0 on a refusal) */
/* disp_flags (ABI 2) */
#define GARNET_DISP_F_ON         (1u << 0)   /* Garnet.kext latched boot-arg garnet-metal-disp=1: the display hooks and the PCI getter may be used */

struct GarnetMetalOps {
    uint32_t magic;        /*   0  GARNET_METAL_OPS_MAGIC */
    uint32_t abi;          /*   4  GARNET_METAL_ABI */
    uint32_t size;         /*   8  sizeof(struct GarnetMetalOps) as built into Garnet.kext */
    uint32_t kext_build;   /*  12  Garnet.kext build (610 = 0.0.610) */
    uint32_t flags;        /*  16  GARNET_METAL_F_* */
    uint32_t reserved0;    /*  20  0 */
    /* REQUIRED (an ops table with any of these NULL is refused by the aux kext, which then never starts the accelerator): */
    void *(*device_open)(void *accel, void *nub);                         /*  24  per-device state (stamp page ...); NULL = refuse */
    void  (*device_close)(void *ctx);                                     /*  32  idempotent */
    int   (*populate_config)(uint8_t *cfg, uint32_t bytes);               /*  40  fill the IOAccelConfig (bytes = 0x90) on top of the family defaults + static check; 0 = ok. Non-zero at probe = the match is refused; non-zero later = the aux kext leaves the config as the family defaults with its own static non-NULL name, tears the device down (stamp VA cleared) so the event machine init fails and start aborts */
    void *(*stamp_memory)(void *ctx, uint32_t *n);                        /*  48  the IOMemoryDescriptor* of the stamp page (NOT retained), *n = 0 */
    volatile uint32_t *(*stamp_va)(void *ctx);                            /*  56  its kernel VA (for setStampBaseAddress) */
    int   (*task_window)(void *ctx, uint32_t kind, uint64_t *size, uint64_t *reserve); /* 64  kind 0 user / 1 kernel task: VA window size and the bytes reserved at 0; 0 = ok */
    /* OPTIONAL (NULL = the documented default): */
    uint32_t (*factory_mask)(void *ctx);                                  /*  72  GARNET_FACT_*; NULL = 0 */
    int   (*mm_hook)(void *ctx, uint32_t op, void *obj);                  /*  80  GARNET_MM_*: 0 = did it (true); NULL or nonzero = the aux memory map returns false */
    void  (*trace)(uint32_t event, uint64_t a, uint64_t b);               /*  88  GARNET_TR_*; NULL = silent */
    /* GENERIC VIRTUAL HOOK (the thin-shell mechanism): every trampoline slot of the aux classes (event machine 68 / 72 / 73 / 84-87, shared user client externalMethod,
     * command queue, vid memory, resource, 2D context) first asks this hook. cls = GARNET_VC_*, slot = the family vtable slot, self = the aux object (identity only),
     * args = the slot's arguments widened to 64 bits in declaration order (pointers as addresses; a reference argument is passed as its address), nargs <= 6.
     * Return 1 = handled and *ret is the slot's return value (widened); anything else = NOT handled: the aux kext then does its default (the family's own base
     * implementation for non-pure slots, 0 / false / NULL for pure ones, exactly what the aux kext did before the hook existed). */
    int   (*vhook)(void *ctx, uint32_t cls, uint32_t slot, void *self, const uint64_t *args, uint32_t nargs, uint64_t *ret);   /*  96 */
    uint64_t caps;                                                        /* 104  GARNET_CAP_*; a hook is used only if its cap bit is set */
    uint64_t reserved1;                                                   /* 112 */
    /* ---- ABI 2 (bring-up 0.0.613 / aux 0.0.3): the display pipe of #11 (IOAccelDisplayPipe layout contract; route: the aux
     * subclass GarnetDisplayPipe of M11H section 3.4). Read ONLY when abi >= 2 AND size >= GARNET_METAL_OPS_V2; an ABI-1 table (120 bytes) means "display off":
     * the aux kext then behaves exactly as aux 0.0.2 (the family's own pipe, the family's own display-machine start). */
    uint32_t disp_flags;                                                  /* 120  GARNET_DISP_F_*; 0 = display off */
    uint32_t reserved2;                                                   /* 124  0 */
    /* The display-pipe hook: same contract as vhook (cls = GARNET_VC_DISPLAYPIPE, slot = the IOAccelDisplayPipe vtable slot, self = the pipe; 1 = handled and *ret is
     * the slot's return value; anything else = the aux kext calls the FAMILY's own implementation of the slot). Used only while disp_flags has GARNET_DISP_F_ON. */
    int   (*disp_hook)(void *ctx, uint32_t cls, uint32_t slot, void *self, const uint64_t *args, uint32_t nargs, uint64_t *ret);   /* 128 */
    /* The GPU's IOPCIDevice (identity: an IOService*, NOT retained; valid while Garnet.kext is loaded) for the display machine's framebuffer walk, or NULL. */
    void *(*pci_device)(void *ctx);                                       /* 136 */
    /* ---- ABI 3 (bring-up 0.0.656 / aux 0.0.5, G6): read ONLY when abi >= 3 AND size >= GARNET_METAL_OPS_V3 AND caps has GARNET_CAP_NATIVE_OPEN AND the pointer is non-NULL.
     * The aux accelerator's newUserClient(task, securityID, type 'GRNT', handler) calls it with accel = the accelerator (identity: the pointer device_open received), task and
     * securityID as the kernel passed them, type = 'GRNT', handler = where the new IOUserClient* goes. RETURNS AN IOReturn (0 = kIOReturnSuccess with *handler set; any other value
     * = refused, *handler untouched): it is NEVER a bool, "1 = handled" does not exist here. The aux kext returns that value unchanged; it never falls through to the family for 'GRNT'. */
    int32_t (*native_open)(void *ctx, void *accel, void *task, void *security_id, uint32_t type, void **handler);   /* 144 */
};                                                                        /* 152 */

#define GARNET_METAL_OPS_MIN 120u    /* ABI 1: every consumer requires at least this much */
#define GARNET_METAL_OPS_V2  144u    /* ABI 2: the display members are present */
#define GARNET_METAL_OPS_V3  152u    /* ABI 3: native_open is present */
/* a consumer accepts abi >= GARNET_METAL_ABI_MIN (the table is append-only: no member ever moves or changes meaning) and size >= GARNET_METAL_OPS_MIN; members beyond
 * the table's own size (or its abi) are never read: they count as absent */

#ifdef __cplusplus
static_assert(sizeof(struct GarnetMetalOps) == GARNET_METAL_OPS_V3, "GarnetMetalOps ABI 3 is 152 bytes");
static_assert(__builtin_offsetof(struct GarnetMetalOps, native_open) == GARNET_METAL_OPS_V2, "the ABI-3 member starts right after the 144 ABI-2 bytes");
static_assert(__builtin_offsetof(struct GarnetMetalOps, disp_flags) == GARNET_METAL_OPS_MIN, "the ABI-2 members start right after the 120 ABI-1 bytes");
#else
_Static_assert(sizeof(struct GarnetMetalOps) == GARNET_METAL_OPS_V3, "GarnetMetalOps ABI 3 is 152 bytes");
_Static_assert(__builtin_offsetof(struct GarnetMetalOps, native_open) == GARNET_METAL_OPS_V2, "the ABI-3 member starts right after the 144 ABI-2 bytes");
_Static_assert(__builtin_offsetof(struct GarnetMetalOps, disp_flags) == GARNET_METAL_OPS_MIN, "the ABI-2 members start right after the 120 ABI-1 bytes");
#endif

#endif /* GARNET_METAL_OPS_H */
