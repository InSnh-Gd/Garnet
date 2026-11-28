// garnet_sparse_internal.h - Shared internal declarations for sparse/PRT heaps, textures, and resource-state encoding.
#ifndef GARNET_SPARSE_INTERNAL_H
#define GARNET_SPARSE_INTERNAL_H

#include "garnet_sparse.h"

@interface RDNA4SparseHeap : _MTLHeap
@end

@interface RDNA4SparseTexture : RDNA4Texture
@end

@interface RDNA4ResourceStateEncoder : _MTLCommandEncoder
@end

GARNET_INTERNAL_EXTERN id rdna4_sparse_heap_new(id dev, MTLHeapDescriptor *d, NSError **err);
GARNET_INTERNAL_EXTERN id rdna4_cb_new_resource_state_encoder(RDNA4CommandBuffer *cb, id desc);
GARNET_INTERNAL_EXTERN NSUInteger rdna4_device_sparse_tile_size_in_bytes(void);
GARNET_INTERNAL_EXTERN NSUInteger rdna4_device_sparse_tile_size_for_page_size(NSInteger sparsePageSize);
GARNET_INTERNAL_EXTERN MTLSize rdna4_device_sparse_tile_size(MTLTextureType textureType,
                                                                     MTLPixelFormat pixelFormat,
                                                                     NSUInteger sampleCount);
GARNET_INTERNAL_EXTERN MTLSize rdna4_device_sparse_tile_size_with_page_size(MTLTextureType textureType,
                                                                                    MTLPixelFormat pixelFormat,
                                                                                    NSUInteger sampleCount,
                                                                                    NSInteger sparsePageSize);
GARNET_INTERNAL_EXTERN void rdna4_device_convert_sparse_pixel_regions(const MTLRegion *pixelRegions,
                                                                              MTLRegion *tileRegions,
                                                                              MTLSize tileSize,
                                                                              MTLSparseTextureRegionAlignmentMode mode,
                                                                              NSUInteger numRegions);
GARNET_INTERNAL_EXTERN void rdna4_device_convert_sparse_tile_regions(const MTLRegion *tileRegions,
                                                                             MTLRegion *pixelRegions,
                                                                             MTLSize tileSize,
                                                                             NSUInteger numRegions);

#endif /* GARNET_SPARSE_INTERNAL_H */
