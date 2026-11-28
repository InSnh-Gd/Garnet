// garnet_resources_internal.h - Shared internal declarations across GarnetMetal's
// resource, texture, and heap compilation units (garnet_resources.m, garnet_textures.m,
// garnet_heaps.m).
#ifndef GARNET_RESOURCES_INTERNAL_H
#define GARNET_RESOURCES_INTERNAL_H

#include "garnet_vram_surf.h"

#ifndef GARNET_INTERNAL_EXTERN
#define GARNET_INTERNAL_EXTERN static
#endif

typedef struct { rdna4p_pool p; BOOL ready; } RDNA4HeapTimelineState;
typedef struct { BOOL on; pthread_mutex_t mu; rdna4ic_cache c; rdna4li_cache li; _Atomic uint64_t baseless; uint64_t lastSig; } RDNA4ImpCacheState;
typedef struct {
    BOOL on;
    BOOL canStatSyncWb;
    pthread_mutex_t mu;
    pthread_cond_t flushCond;
    dispatch_queue_t flushQ;
    dispatch_source_t timer;
    BOOL timerArmed;
    _Atomic int syncWb;
    _Atomic uint64_t pathbUploads;
    _Atomic uint64_t pathbSkipped;
    garnet_vs_cache_t c;
    uint64_t lastSig;
} RDNA4VramSurfState;

GARNET_INTERNAL_EXTERN RDNA4HeapTimelineState RDNA4HT;
GARNET_INTERNAL_EXTERN RDNA4ImpCacheState RDNA4IC;
GARNET_INTERNAL_EXTERN RDNA4VramSurfState RDNA4VS;

@interface RDNA4HeapBacking (RDNA4HeapHandleState)
- (void)rdna4MarkHandleOpen;
- (BOOL)rdna4ConsumeHandleOpen;
@end

GARNET_INTERNAL_EXTERN VkResult rdna4_mem_alloc(uint32_t mt, VkMemoryRequirements mr, BOOL map, BOOL zero, VkBuffer b, VkImage i, RDNA4Mem *m);
GARNET_INTERNAL_EXTERN void rdna4_mem_free(RDNA4Mem *m);
GARNET_INTERNAL_EXTERN VkResult rdna4_import_alloc(const VkMemoryAllocateInfo *ma, VkDeviceMemory *out);
GARNET_INTERNAL_EXTERN VkResult rdna4_imp_get(IOSurfaceRef s, void *base, size_t ialloc, const VkMemoryAllocateInfo *ma, VkDeviceMemory *out, BOOL *hit);
GARNET_INTERNAL_EXTERN void rdna4_imp_put(VkDeviceMemory mem);
GARNET_INTERNAL_EXTERN BOOL rdna4_linimg_get(const rdna4li_key *k, VkImage *outImg, int *outInited);
GARNET_INTERNAL_EXTERN VkImage rdna4_linimg_add(const rdna4li_key *k, VkImage freshImg, int *outInited);
GARNET_INTERNAL_EXTERN void rdna4_linimg_mark_inited(VkImage img);
GARNET_INTERNAL_EXTERN BOOL rdna4_linimg_is_inited(VkImage img);
GARNET_INTERNAL_EXTERN void rdna4_linimg_put(VkImage img);
GARNET_INTERNAL_EXTERN void rdna4_baseless_note(uint32_t id);
GARNET_INTERNAL_EXTERN void rdna4_vs_wait_flush_done(uint64_t vsId);
GARNET_INTERNAL_EXTERN void rdna4_vs_put(uint64_t vsId);
GARNET_INTERNAL_EXTERN BOOL rdna4_vs_try_bind(IOSurfaceRef s, void *base, size_t ialloc, size_t alloc, NSUInteger poff,
                                              NSUInteger w, NSUInteger h, size_t bpr, const RDNA4Fmt *fmt,
                                              NSUInteger mtlUsage, VkImageUsageFlags reqUse, VkFormatFeatureFlags offs,
                                              BOOL attOK, BOOL isDisp, BOOL hostReadOnly, const VkMemoryAllocateInfo *ma,
                                              VkImage *outImg, VkBuffer *outIbuf, uint64_t *outId);
GARNET_INTERNAL_EXTERN int rdna4nc_host_readonly(const void *p, NSUInteger len);
GARNET_INTERNAL_EXTERN void *rdna4nc_owned_allocate(uint64_t len, uint64_t hostAlign, uint64_t *span);

#endif /* GARNET_RESOURCES_INTERNAL_H */
