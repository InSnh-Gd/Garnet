// Exercise production metadata validation before a Vulkan layout can be created.
#include "GarnetDevice.m"

static unsigned checks, creates;
static void require(BOOL condition, const char *message) {
    checks++;
    if (!condition) { fprintf(stderr, "BUFFER_ADDRESS_TABLE_FAIL %s\n", message); exit(1); }
}
static VkResult VKAPI_CALL createLayout(VkDevice device, const VkDescriptorSetLayoutCreateInfo *info,
                                       const VkAllocationCallbacks *allocator, VkDescriptorSetLayout *layout) {
    (void)device; (void)allocator;
    require(info->bindingCount == 1 && info->pBindings[0].binding == 640 &&
            info->pBindings[0].descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            "metadata changed the reflected Vulkan descriptor");
    creates++; *layout = (VkDescriptorSetLayout)(uintptr_t)1;
    return VK_SUCCESS;
}
static NSDictionary *metadata(id descriptor, id version, id bytes) {
    NSMutableDictionary *meta = [@{@"bindings": @[@{@"kind": @"BufferAddressTable", @"descriptor": descriptor}]} mutableCopy];
    if (version) meta[@"buffer_address_table_version"] = version;
    if (bytes) meta[@"buffer_address_table_bytes"] = bytes;
    return meta;
}
static void checkLayout(NSDictionary *meta, BOOL accepted, uint32_t version, RDNA4sRefl *reflection) {
    unsigned before = creates;
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    RDNA4PB *bindings = NULL; uint32_t count = 0; NSError *error = nil;
    BOOL result = rdna4_stage_dsl(reflection, 0, VK_SHADER_STAGE_COMPUTE_BIT, meta, &layout,
                                  &bindings, &count, [NSMutableArray array], &error);
    require(result == accepted, "unexpected layout acceptance");
    if (accepted) {
        require(!error && layout && count == 1 && bindings && bindings[0].addressTable &&
                bindings[0].addressTableVersion == version && creates == before + 1,
                "accepted address-table layout did not preserve its version");
    } else {
        require(error && !bindings && !count && creates == before,
                "bad metadata reached Vulkan or did not return an error");
    }
    free(bindings);
}
int main(int argc, const char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        puts("Usage: test-buffer-address-table\nChecks production metadata validation with a simulated Vulkan layout boundary.");
        return 0;
    }
    if (argc != 1) return 2;
    @autoreleasepool {
        RDNA4sRefl *reflection = calloc(1, sizeof *reflection);
        require(reflection != NULL, "reflection allocation");
        reflection->bind[0][640] = (RDNA4sBind){ .used = 1, .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .count = 1 };
        reflection->maxset = 0;
        RDNA4R.bufferAddress = YES;
        vkCreateDescriptorSetLayout = createLayout;
        NSDictionary *descriptor = @{@"binding": @640, @"count": @1};
        checkLayout(metadata(descriptor, nil, nil), YES, 1, reflection);
        checkLayout(metadata(descriptor, @2, @768), YES, 2, reflection);
        NSMutableDictionary *registry = [metadata(descriptor, @3, @800) mutableCopy];
        registry[@"buffer_address_table_row_stride_bytes"] = @24;
        registry[@"buffer_address_table_dynamic_extent"] = @YES;
        checkLayout(registry, YES, 3, reflection);
        for (id stride in @[@0, @16, @24.0, @YES, @"24", [NSNull null]]) {
            NSMutableDictionary *bad = [registry mutableCopy];
            bad[@"buffer_address_table_row_stride_bytes"] = stride;
            checkLayout(bad, NO, 0, reflection);
        }
        for (id dynamic in @[@NO, @1, @1.0, @"true", [NSNull null]]) {
            NSMutableDictionary *bad = [registry mutableCopy];
            bad[@"buffer_address_table_dynamic_extent"] = dynamic;
            checkLayout(bad, NO, 0, reflection);
        }
        for (NSString *key in @[@"buffer_address_table_row_stride_bytes", @"buffer_address_table_dynamic_extent"]) {
            NSMutableDictionary *bad = [registry mutableCopy];
            [bad removeObjectForKey:key];
            checkLayout(bad, NO, 0, reflection);
        }
        NSMutableDictionary *mixed = [registry mutableCopy];
        mixed[@"buffer_address_table_version"] = @2;
        mixed[@"buffer_address_table_bytes"] = @768;
        checkLayout(mixed, NO, 0, reflection);
        const struct { __unsafe_unretained id version, bytes; } invalidLayouts[] = {
            {@1, @256}, {@3, @768}, {@2, @256}, {@2, nil}, {nil, @768},
            {@YES, @768}, {@2.0, @768}, {@2, @768.0}, {@-2, @768}, {@"2", @768},
            {[NSNull null], @768}, {@2, [NSNull null]}
        };
        for (unsigned i = 0; i < sizeof invalidLayouts / sizeof *invalidLayouts; ++i)
            checkLayout(metadata(descriptor, invalidLayouts[i].version, invalidLayouts[i].bytes), NO, 0, reflection);
        NSArray *invalidDescriptors = @[
            @"640", [NSNull null], @{}, @{@"binding": @640},
            @{@"binding": @640, @"count": @YES}, @{@"binding": @640.0, @"count": @1},
            @{@"binding": @-1, @"count": @1}, @{@"binding": @672, @"count": @1},
            @{@"binding": @640, @"count": @2}
        ];
        for (id descriptorValue in invalidDescriptors)
            checkLayout(metadata(descriptorValue, @2, @768), NO, 0, reflection);
        for (id bindingsValue in @[@"invalid", [NSNull null], @[@1], @[@{}], @[@{@"kind": @1}]])
            checkLayout(@{@"bindings": bindingsValue}, NO, 0, reflection);
        NSDictionary *row = @{@"kind": @"BufferAddressTable", @"descriptor": descriptor};
        checkLayout(@{@"bindings": @[row, row]}, NO, 0, reflection);
        RDNA4R.bufferAddress = NO;
        checkLayout(metadata(descriptor, @2, @768), NO, 0, reflection);
        RDNA4R.bufferAddress = YES;
        reflection->bind[0][640].count = 2;
        checkLayout(metadata(descriptor, @2, @768), NO, 0, reflection);
        free(reflection);
        printf("BUFFER_ADDRESS_TABLE_PASS checks=%u accepted_layouts=%u simulated_vulkan=1 gpu_execution=0\n", checks, creates);
    }
    return 0;
}
