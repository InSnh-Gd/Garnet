// garnet_mesh_internal.h - Shared internal declarations for Metal 3 mesh/object pipelines.
#ifndef GARNET_MESH_INTERNAL_H
#define GARNET_MESH_INTERNAL_H

#include "garnet_pipelines_internal.h"

@interface RDNA4MeshRenderPipelineState : RDNA4RenderPipelineState
@end

GARNET_INTERNAL_EXTERN id rdna4_mesh_device_new_pipeline(
    id dev, id desc, NSUInteger options, MTLRenderPipelineReflection **refl, NSError **error);
GARNET_INTERNAL_EXTERN void rdna4_mesh_device_new_pipeline_async(
    id dev, id desc, NSUInteger options, void (^handler)(id, id, NSError *));

#endif /* GARNET_MESH_INTERNAL_H */
