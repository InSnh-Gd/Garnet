// GarnetDevice.m - Single-TU compatibility bridge for host/mock tests.
// Production builds compile the 15 garnet_*.m units independently via build.sh
// with -DGARNET_METAL_MODULAR_BUILD=1 -fvisibility=hidden (see STRUCTURE.md).
#ifndef GARNET_METAL_MODULAR_BUILD
#include "garnet_metal_internal.h"
#include "garnet_backend.m"
#include "garnet_resources.m"
#include "garnet_surface.m"
#include "garnet_shaders.m"
#include "garnet_pipelines.m"
#include "garnet_commands.m"
#include "garnet_arguments.m"
#include "garnet_icb.m"
#include "garnet_sparse_io.m"
#include "garnet_rt_runtime.m"
#include "garnet_rt_accel.m"
#include "garnet_rt_plan.m"
#include "garnet_rt_encoder.m"
#include "garnet_mesh.m"
#include "garnet_device.m"
#endif
