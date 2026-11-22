#include "GarnetDevice.m"

static unsigned checks, layoutCalls;

static void require(BOOL ok, const char *message) {
    ++checks;
    if (!ok) {
        fprintf(stderr, "GATHER_CAPABILITY_FAIL %s\n", message);
        exit(1);
    }
}

static VkResult VKAPI_CALL createLayout(VkDevice device,
    const VkDescriptorSetLayoutCreateInfo *info,
    const VkAllocationCallbacks *allocator, VkDescriptorSetLayout *layout) {
    (void)device;
    (void)allocator;
    require(info && !info->bindingCount, "expected an empty layout");
    ++layoutCalls;
    *layout = (VkDescriptorSetLayout)(uintptr_t)1;
    return VK_SUCCESS;
}

static void checkStage(RDNA4sRefl *reflection, BOOL expected) {
    NSError *error = nil;
    RDNA4PB *bindings = NULL;
    uint32_t count = 0;
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    unsigned before = layoutCalls;
    BOOL ok = rdna4_stage_dsl(reflection, 0, VK_SHADER_STAGE_COMPUTE_BIT,
        @{}, &layout, &bindings, &count, [NSMutableArray array], &error);
    require(ok == expected, "stage capability decision differed");
    require(count == 0, "empty stage acquired bindings");
    if (expected) {
        require(!error && layout && layoutCalls == before + 1,
            "supported stage did not create its layout");
    } else {
        require(error && !layout && !bindings && layoutCalls == before,
            "unsupported gather allocated a layout");
        require([error.localizedDescription containsString:
            @"shaderImageGatherExtended"], "missing feature was not named");
    }
    free(bindings);
}

int main(void) {
    @autoreleasepool {
        const uint32_t code[] = {
            0x07230203, 0x00010000, 0, 1, 0,
            (2u << 16) | 17u, 25u,
        };
        RDNA4sMod module;
        char why[128] = {0};
        require(!rdna4s_parse(code, sizeof code / sizeof code[0],
            &module, why, sizeof why), "capability parse failed");
        require(module.descriptor_features == RDNA4S_IMAGE_GATHER_EXTENDED,
            "ImageGatherExtended capability was lost");
        rdna4s_free(&module);

        RDNA4sRefl *reflection = calloc(1, sizeof *reflection);
        require(reflection != NULL, "reflection allocation failed");
        reflection->descriptor_features = RDNA4S_IMAGE_GATHER_EXTENDED;
        vkCreateDescriptorSetLayout = createLayout;

        RDNA4GatherCaps = rdna4_gather_caps_from_vulkan(1, -8, 7);
        atomic_store(&rdna4_gather_caps_ready, 0);
        checkStage(reflection, NO);
        RDNA4GatherCaps = rdna4_gather_caps_from_vulkan(0, -8, 7);
        atomic_store(&rdna4_gather_caps_ready, 1);
        checkStage(reflection, NO);
        reflection->descriptor_features = 0;
        checkStage(reflection, YES);
        reflection->descriptor_features = RDNA4S_IMAGE_GATHER_EXTENDED;
        RDNA4GatherCaps = rdna4_gather_caps_from_vulkan(1, -8, 7);
        checkStage(reflection, YES);
        free(reflection);
        printf("GATHER_CAPABILITY_PASS checks=%u mock_vulkan=1 "
               "gpu_execution=0\n", checks);
    }
    return 0;
}
