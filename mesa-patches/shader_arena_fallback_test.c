#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef int VkResult;
enum { VK_SUCCESS, VK_ERROR_OUT_OF_DEVICE_MEMORY, VK_ERROR_OUT_OF_HOST_MEMORY };
enum { RADEON_DOMAIN_VRAM = 1, RADEON_DOMAIN_GTT = 2 };
enum radeon_bo_flag {
    RADEON_FLAG_NO_INTERPROCESS_SHARING = 1, RADEON_FLAG_32BIT = 2,
    RADEON_FLAG_NO_CPU_ACCESS = 4, RADEON_FLAG_READ_ONLY = 8, RADEON_FLAG_REPLAYABLE = 16,
};
enum { RADV_SHADER_ALLOC_MAX_ARENA_SIZE_SHIFT = 4,
       RADV_SHADER_ALLOC_ALIGNMENT = 256, RADV_BO_PRIORITY_SHADER = 5 };
#define RADV_SHADER_ALLOC_MIN_ARENA_SIZE 4096u
enum { RADV_SHADER_ARENA_DEFAULT, RADV_SHADER_ARENA_REPLAYABLE, RADV_SHADER_ARENA_REPLAYED };
#define MIN2(left, right) ((left) < (right) ? (left) : (right))
#define MAX2(left, right) ((left) > (right) ? (left) : (right))

struct radv_physical_device { struct { bool cpdma_prefetch_writes_memory; } info; };
struct radv_device { unsigned shader_arena_shift; void *ws; bool shader_use_invisible_vram; };
struct radv_shader_free_list { int unused; };
struct radv_shader_arena { unsigned size, type; int entries; void *bo; char *ptr; };
union radv_shader_arena_block { struct { int freelist, list; struct radv_shader_arena *arena; unsigned offset, size; }; };

static struct radv_physical_device physical;
static union radv_shader_arena_block block;
static VkResult results[2];
static int domains[2];
static unsigned attempts;
static unsigned expected_flags;
static char backing;
static const struct radv_physical_device *radv_device_physical(struct radv_device *device) { (void)device; return &physical; }
static union radv_shader_arena_block *alloc_block_obj(struct radv_device *device) { (void)device; return &block; }
static void free_block_obj(struct radv_device *device, union radv_shader_arena_block *item) { (void)device; assert(item == &block); }
static void list_inithead(int *list) { *list = 0; }
static void list_addtail(int *entry, int *head) { (void)entry; (void)head; }
static void add_hole(struct radv_shader_free_list *list, union radv_shader_arena_block *item) { (void)list; (void)item; }
static void *radv_buffer_map(void *ws, void *bo) { (void)ws; assert(bo == &backing); return bo; }
static void radv_bo_destroy(struct radv_device *device, void *allocator, void *bo) { (void)device; (void)allocator; assert(bo == &backing); }

static VkResult radv_bo_create(struct radv_device *device, void *allocator, unsigned size, unsigned alignment,
                               int domain, unsigned flags, unsigned priority, uint64_t va, bool resident, void **bo)
{
    (void)device; (void)allocator; (void)va;
    assert(attempts < 2 && size == 4096 && alignment == 256 && priority == RADV_BO_PRIORITY_SHADER && resident);
    assert(flags == expected_flags);
    domains[attempts] = domain;
    VkResult result = results[attempts++];
    if (result == VK_SUCCESS)
        *bo = &backing;
    return result;
}

#include "shader_arena_production.inc"

static void check(VkResult first, VkResult second, bool replayable, bool invisible, bool retry)
{
    struct radv_device device = {0};
    attempts = 0;
    expected_flags = RADEON_FLAG_NO_INTERPROCESS_SHARING | RADEON_FLAG_32BIT |
                     (invisible ? RADEON_FLAG_NO_CPU_ACCESS : RADEON_FLAG_READ_ONLY) |
                     (replayable ? RADEON_FLAG_REPLAYABLE : 0);
    results[0] = first;
    results[1] = second;
    device.shader_use_invisible_vram = invisible;
    struct radv_shader_arena *arena = radv_create_shader_arena(&device, NULL, 64, 0, replayable, 0);
    assert(attempts == (retry ? 2 : 1));
    assert(domains[0] == RADEON_DOMAIN_VRAM);
    if (retry) assert(domains[1] == RADEON_DOMAIN_GTT);
    assert(!!arena == ((retry ? second : first) == VK_SUCCESS));
    if (arena && !invisible) assert(arena->ptr == &backing);
    free(arena);
}

int main(void)
{
#ifdef HAVE_AMDGPU_DARWIN
    const bool darwin = true;
#else
    const bool darwin = false;
#endif
    check(VK_SUCCESS, VK_SUCCESS, false, false, false);
    check(VK_ERROR_OUT_OF_DEVICE_MEMORY, VK_SUCCESS, false, false, darwin);
    check(VK_ERROR_OUT_OF_DEVICE_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY, false, false, darwin);
    check(VK_ERROR_OUT_OF_HOST_MEMORY, VK_SUCCESS, false, false, false);
    check(VK_ERROR_OUT_OF_DEVICE_MEMORY, VK_SUCCESS, true, false, false);
    check(VK_ERROR_OUT_OF_DEVICE_MEMORY, VK_SUCCESS, false, true, false);
    printf("PASS shader arena allocation (Darwin=%d)\n", darwin);
    return 0;
}
