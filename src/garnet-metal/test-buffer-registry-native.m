// Exercise the production resource snapshot and descriptor-byte producer without a GPU.
#include "GarnetDevice.m"

static unsigned checks, updates;
static NSUInteger descriptorBytes;
static void require(BOOL value, const char *message) {
    ++checks;
    if (!value) { fprintf(stderr, "BUFFER_REGISTRY_NATIVE_FAIL %s\n", message); exit(1); }
}
@interface RegistryBuffer : RDNA4Buffer {
@public
    uint64_t base, extent;
    BOOL readonly, aliasable;
    id testDevice;
}
@end
@implementation RegistryBuffer
- (uint64_t)gpuAddress { return base; }
- (NSUInteger)length { return (NSUInteger)extent; }
- (BOOL)rdna4HostReadOnly { return readonly; }
- (BOOL)isAliasable { return aliasable; }
- (id)device { return testDevice; }
- (VkBuffer)vkBuffer { return (VkBuffer)(uintptr_t)123; }
- (VkDeviceSize)rdna4VkOffset { return 0; }
@end
@interface RegistryHeap : RDNA4Heap {
@public
    NSArray *members;
}
@end
@implementation RegistryHeap
- (NSArray *)rdna4LiveResources { return members; }
@end
@interface RegistryCommandBuffer : RDNA4CommandBuffer {
@public
    NSData *captured;
    RegistryBuffer *upload;
    id testDevice;
}
- (void)configure;
- (RDNA4CommandSegment *)nextSegment;
- (NSUInteger)activeCount;
@end
@implementation RegistryCommandBuffer
- (void)configure {
    _dslot = -1;
    _segments = [NSMutableArray array];
    upload = [RegistryBuffer alloc];
    [self nextSegment];
}
- (RDNA4CommandSegment *)nextSegment {
    RDNA4CommandSegment *old = _curSegment;
    _curSegment = [RDNA4CommandSegment new];
    [_segments addObject:_curSegment];
    return old;
}
- (NSUInteger)activeCount { return _bufferAddressResources.count; }
- (id)device { return testDevice; }
- (BOOL)rdna4Bytes:(const void *)bytes length:(NSUInteger)length
        buffer:(RDNA4Buffer *__strong *)buffer offset:(NSUInteger *)offset {
    captured = [NSData dataWithBytes:bytes length:length];
    *buffer = upload; *offset = 0;
    return YES;
}
@end
static RegistryBuffer *makeBuffer(id device, uint64_t address, uint64_t length, BOOL readonly) {
    RegistryBuffer *buffer = [RegistryBuffer alloc];
    buffer->testDevice = device; buffer->base = address;
    buffer->extent = length; buffer->readonly = readonly;
    return buffer;
}
static void VKAPI_CALL update(VkDevice device, uint32_t count, const VkWriteDescriptorSet *writes,
        uint32_t copies, const VkCopyDescriptorSet *copy) {
    (void)device; (void)copies; (void)copy;
    require(count == 1 && writes[0].dstBinding == 651 && writes[0].descriptorCount == 1 &&
        writes[0].descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        "producer changed the existing descriptor binding");
    descriptorBytes = writes[0].pBufferInfo[0].range;
    ++updates;
}
static void prepare(RegistryCommandBuffer *command, NSSet *resources) {
    RDNA4ArgumentTables inactive = {0};
    RDNA4ArgumentScanPrepared scan = {0};
    require([command rdna4PrepareArguments:&inactive resources:resources writable:[NSSet set]
        scan:&scan textures:^(RDNA4Texture *texture, VkImageLayout layout, BOOL writable) {
            (void)texture; (void)layout; (void)writable;
        }], "inactive image tables lost buffer declarations");
}
static BOOL fill(RegistryCommandBuffer *command, const RDNA4StageState *state, const char *stage) {
    RDNA4PB binding = {.binding = 651, .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .count = 1, .addressTable = YES, .addressTableVersion = 3};
    return [command rdna4FillSet:(VkDescriptorSet)(uintptr_t)1 bindings:&binding count:1
                          state:state stage:stage];
}
static rdna4_buffer_registry_header header(NSData *data) {
    rdna4_buffer_registry_header value;
    require(data.length >= 800, "missing v3 header");
    memcpy(&value, (const uint8_t *)data.bytes + 768, sizeof value);
    require(value.row_stride_bytes == 24 && value.reserved == 0 &&
        value.table_bytes == data.length && value.row_count == (data.length - 800) / 24,
        "header does not describe exact descriptor extent");
    return value;
}
static BOOL contains(NSData *data, uint64_t base, uint64_t length, uint64_t access) {
    rdna4_buffer_registry_header h = header(data);
    for (uint64_t i = 0; i < h.row_count; ++i) {
        rdna4_buffer_registry_row row;
        memcpy(&row, (const uint8_t *)data.bytes + 800 + i * 24, sizeof row);
        if (row.base == base && row.length == length && row.access == access) return YES;
    }
    return NO;
}
int main(void) {
    @autoreleasepool {
        vkUpdateDescriptorSets = update;
        RDNA4R.lim.maxStorageBufferRange = UINT32_MAX;
        id device = [NSObject new];
        RegistryCommandBuffer *command = [RegistryCommandBuffer alloc];
        command->testDevice = device; [command configure];
        RegistryBuffer *direct = makeBuffer(device, UINT64_C(0x1800000000), UINT64_C(0x200000000), YES);
        RegistryBuffer *overlap = makeBuffer(device, direct->base + 32, 128, NO);
        RegistryBuffer *alias = makeBuffer(device, direct->base + 1024, 128, NO);
        alias->aliasable = YES;
        RegistryBuffer *undeclared = makeBuffer(device, UINT64_C(0x2200000000), 256, NO);
        RegistryHeap *heap = [RegistryHeap alloc]; heap->members = @[overlap, alias];
        NSMutableSet *resources = [NSMutableSet setWithObjects:direct, heap, nil];
        for (unsigned i = 0; i < 257; ++i)
            [resources addObject:makeBuffer(device, UINT64_C(0x3000000000) + i * 256, 64, i & 1)];
        prepare(command, resources);
        require([command activeCount] == 259, "actual buffer count was truncated or heap children lost");
        RDNA4StageState state = {0}; state.buf[7] = (RDNA4BufSlot){direct, 256};
        const char *stages[] = {"vertex", "fragment", "compute"};
        for (unsigned i = 0; i < 3; ++i) {
            require(fill(command, &state, stages[i]), "stage registry production failed");
            require(header(command->captured).row_count == 259 && descriptorBytes == 800 + 259 * 24,
                "descriptor extent or row count differs from actual resources");
            rdna4_buffer_table prefix;
            memcpy(&prefix, command->captured.bytes, sizeof prefix);
            require(prefix.address[7] == direct->base + 256 && prefix.length[7] == direct->extent - 256 &&
                prefix.access[7] == RDNA4_BUFFER_READ, "direct prefix lost view offset or readonly");
            require(contains(command->captured, direct->base, direct->extent, RDNA4_BUFFER_READ),
                "registry lost full 64-bit declared extent or readonly");
            require(contains(command->captured, overlap->base, overlap->extent, 3),
                "overlapping range permissions were merged");
            require(!contains(command->captured, alias->base, alias->extent, 3) &&
                !contains(command->captured, undeclared->base, undeclared->extent, 3),
                "aliasable or undeclared buffer entered the registry");
        }
        RDNA4CommandSegment *first = [command nextSegment];
        prepare(command, [NSSet set]);
        require([command activeCount] == 0, "previous draw declarations leaked into current registry");
        require([first->argumentRefs containsObject:overlap], "consuming segment did not retain heap child");
        RDNA4StageState empty = {0};
        require(fill(command, &empty, "compute") && header(command->captured).row_count == 0,
            "empty registry retains old command history");
        prepare(command, [NSSet setWithObject:overlap]);
        RDNA4R.lim.maxStorageBufferRange = 823;
        unsigned before = updates;
        require(!fill(command, &empty, "compute") && [command rdna4Error] && updates == before,
            "backend admission overflow reached descriptor update");
        RegistryCommandBuffer *bad = [RegistryCommandBuffer alloc];
        bad->testDevice = device; [bad configure];
        RegistryBuffer *overflow = makeBuffer(device, UINT64_MAX - 7, 8, NO);
        prepare(bad, [NSSet setWithObject:overflow]);
        RDNA4R.lim.maxStorageBufferRange = UINT32_MAX;
        require(!fill(bad, &empty, "compute") && [bad rdna4Error] && updates == before,
            "overflowing BDA range reached descriptor update");
        printf("BUFFER_REGISTRY_NATIVE_PASS checks=%u stages=3 rows=259 exact_bytes=1 readonly=1 overlap=1 unknown=1 heap=1 segment_snapshot=1 admission_failure=1 gpu_execution=0\n", checks);
    }
    return 0;
}
