#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#define MIN2(a, b) ((a) < (b) ? (a) : (b))
#define MAX2(a, b) ((a) > (b) ? (a) : (b))
#define ALWAYS_INLINE static inline
#define VKAPI_ATTR
#define VKAPI_CALL
#define VK_FROM_HANDLE(type, name, handle) struct type *name = (handle)
#define RADV_MAX_MEMORY_ALLOCATION_SIZE UINT64_C(0xfffffffc)
#define RADV_DRM_DEVICE_AMDGPU_DARWIN 3
#define RADV_CMD_DIRTY_INDEX_BUFFER 4
#define RDNA4N_BUFFER_MAX_BYTES (UINT64_C(1) << 20)
struct acd_kext { int unused; };
typedef struct ac_drm_device { int native; } ac_drm_device;
struct radv_physical_device {
    int drm_device_type, drm_device_mtx;
    ac_drm_device *drm_device;
    struct { uint32_t vram_size_kb; } info;
};
static unsigned locks, unlocks, opens;
static void simple_mtx_lock(int *p) { (void)p; locks++; }
static void simple_mtx_unlock(int *p) { (void)p; unlocks++; }
static void radv_create_drm_device_locked(struct radv_physical_device *p) { (void)p; opens++; }
static struct acd_kext *ac_drm_darwin_kext(ac_drm_device *d, uint32_t *minor) {
    (void)minor; static struct acd_kext native;
    return d->native ? &native : NULL;
}
struct radv_vertex_buffer_state { struct { uint64_t addr, size; } bindings[32]; };
struct radv_dynamic_state {
    struct { uint32_t bindings[32], offsets[32], format_sizes[32], non_trivial_format[32]; } vertex_input;
    struct { uint32_t vi_binding_strides[32]; } vk;
};
struct radv_vbo_info { uint64_t va; uint32_t binding, stride, size, attrib_offset, attrib_format_size, non_trivial_format; };
struct radv_index_buffer_state { uint64_t va; uint32_t index_type, max_index_count; };
struct radv_cmd_buffer {
    struct {
        struct radv_vertex_buffer_state vertex_buffer;
        struct radv_dynamic_state dynamic;
        struct radv_index_buffer_state index_buffer;
        uint32_t primitive_restart_index, dirty;
    } state;
};
typedef struct radv_cmd_buffer *VkCommandBuffer;
typedef struct { struct { uint64_t address, size; } addressRange; uint32_t indexType; } VkBindIndexBuffer3InfoKHR;
static uint32_t vk_to_index_type(uint32_t type) { return type; }
static uint32_t radv_get_primitive_restart_index(uint32_t type) { return type == 16 ? UINT16_MAX : UINT32_MAX; }
static int radv_get_vgt_index_size(uint32_t type) { return type == 16 ? 2 : 4; }
struct radeon_winsys_bo { bool is_virtual; uint64_t size; };
struct radv_amdgpu_winsys_bo { struct radeon_winsys_bo base; uint32_t bo_handle; void *children; };
static struct radv_amdgpu_winsys_bo *radv_amdgpu_winsys_bo(struct radeon_winsys_bo *bo) { return (void *)bo; }
struct radv_device { int vk; };
struct radv_device_memory { struct { void *device; } base; struct radeon_winsys_bo *bo; void *user_ptr; unsigned import_handle_type; uint64_t alloc_size; };
typedef struct radv_device *VkDevice;
typedef struct radv_device_memory *VkDeviceMemory;
#include "large_api_actual.inc"

int main(void) {
    const uint64_t gib = UINT64_C(1) << 30;
    ac_drm_device backend = {1};
    struct radv_physical_device pdev = { .drm_device_type = RADV_DRM_DEVICE_AMDGPU_DARWIN,
        .drm_device = &backend, .info = {16 * 1024 * 1024} };
    assert(radv_get_max_allocation_size(&pdev) == 8 * gib);
    assert(locks == 1 && unlocks == 1 && opens == 1);
    pdev.info.vram_size_kb = 6 * 1024 * 1024;
    assert(radv_get_max_allocation_size(&pdev) == 6 * gib);
    backend.native = 0;
    assert(radv_get_max_allocation_size(&pdev) == RADV_MAX_MEMORY_ALLOCATION_SIZE);
    pdev.drm_device = NULL;
    assert(radv_get_max_allocation_size(&pdev) == RADV_MAX_MEMORY_ALLOCATION_SIZE);
    pdev.drm_device_type = 0; unsigned before = opens;
    assert(radv_get_max_allocation_size(&pdev) == RADV_MAX_MEMORY_ALLOCATION_SIZE && opens == before);

    struct radv_cmd_buffer cmd = {0};
    cmd.state.dynamic.vertex_input.bindings[3] = 7;
    cmd.state.dynamic.vk.vi_binding_strides[7] = 16;
    cmd.state.vertex_buffer.bindings[7].addr = 40 * gib;
    cmd.state.vertex_buffer.bindings[7].size = 8 * gib;
    struct radv_vbo_info info = {0};
    radv_get_vbo_info(&cmd, 3, &info);
    assert(info.va == 40 * gib && info.size == UINT32_MAX && info.stride == 16 && info.binding == 7);
    cmd.state.vertex_buffer.bindings[7].size = 128;
    radv_get_vbo_info(&cmd, 3, &info); assert(info.size == 128);
    VkBindIndexBuffer3InfoKHR bind = {{40 * gib, 8 * gib}, 16};
    radv_CmdBindIndexBuffer3KHR(&cmd, &bind);
    assert(cmd.state.index_buffer.va == 40 * gib && cmd.state.index_buffer.max_index_count == UINT32_MAX);
    bind.indexType = 32;
    radv_CmdBindIndexBuffer3KHR(&cmd, &bind);
    assert(cmd.state.index_buffer.max_index_count == 2 * gib);
    bind.addressRange.size = 128;
    radv_CmdBindIndexBuffer3KHR(&cmd, &bind); assert(cmd.state.index_buffer.max_index_count == 32);
    bind.addressRange.size = 0;
    radv_CmdBindIndexBuffer3KHR(&cmd, &bind); assert(!cmd.state.index_buffer.va && !cmd.state.index_buffer.max_index_count);

    struct radv_device dev = {0}, other = {0};
    struct radv_amdgpu_winsys_bo bo = { .base = {.size = 8 * gib}, .bo_handle = 7 };
    struct radv_device_memory mem = {.base = {&dev.vk}, .bo = &bo.base, .alloc_size = 8 * gib};
    uint32_t handle = 0;
    assert(buffer_handle(&dev, &mem, 6 * gib, 4, &handle) == 0 && handle == 7);
    bo.children = &bo;
    assert(buffer_handle(&dev, &mem, 0, 4, &handle) == -ENOSYS);
    assert(buffer_handle(&other, &mem, 0, 4, &handle) == -EINVAL);
    bo.children = NULL;
    assert(buffer_handle(&dev, &mem, 8 * gib, 4, &handle) == -EINVAL);
    assert(buffer_handle(&dev, &mem, UINT64_MAX, 4, &handle) == -EINVAL);
    puts("large-buffer-api: production limits, 64-bit bases, bounded windows and bridge routing PASS");
    return 0;
}
