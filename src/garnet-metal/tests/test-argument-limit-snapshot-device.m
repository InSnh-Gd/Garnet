#if defined(__linux__)

#include <stddef.h>
#include <stdint.h>

extern id class_createInstance(Class cls, size_t extraBytes);

typedef unsigned long NSUInteger;
typedef signed char BOOL;
#define YES ((BOOL)1)
#define NO ((BOOL)0)
#ifndef nil
#define nil ((id)0)
#endif

__attribute__((objc_root_class))
@interface NSObject {
    Class isa;
}
+ (instancetype)alloc;
- (instancetype)init;
@end

@implementation NSObject
+ (instancetype)alloc { return class_createInstance(self, 0); }
- (instancetype)init { return self; }
@end

@interface NSAutoreleasePool : NSObject
- (void)drain;
@end

@implementation NSAutoreleasePool
- (void)drain {}
@end

#else

#import <Foundation/Foundation.h>

#endif

#define GARNET_ARGUMENT_LIMIT_SNAPSHOT_ONLY 1
#include "garnet_backend.m"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;

static void require(int condition, const char *message) {
    ++checks;
    if (!condition) {
        fprintf(stderr, "ARGUMENT_LIMIT_SNAPSHOT_DEVICE_FAIL %s\n", message);
        exit(1);
    }
}

static int provider_calls;
static unsigned loader_opens, loader_closes, loader_resolves;

static void *missing_open(void *context, const char *path) {
    (void)path;
    ++loader_opens;
    return context;
}

static void missing_close(void *context, void *handle) {
    ++loader_closes;
    require(handle == context, "mock loader closes its handle");
}

static void *missing_resolve(void *context, void *handle, const char *name) {
    (void)context;
    (void)handle;
    (void)name;
    ++loader_resolves;
    return NULL;
}

static int argument_limits_provider(
    const rdna4_radv_darwin_host_facts_v1 *facts,
    rdna4_radv_darwin_argument_limits_v1 *out) {
    (void)facts;
    ++provider_calls;
    memset(out, 0, sizeof(*out));
    out->struct_size = sizeof(*out);
    out->abi_version = 1;
    out->flags = RDNA4_RADV_DARWIN_LIMITS_READ_ONLY |
                 RDNA4_RADV_DARWIN_LIMITS_NO_VULKAN_OBJECT |
                 RDNA4_RADV_DARWIN_LIMITS_NO_PIPELINE_UUID;
    out->valid_flags = RDNA4_RADV_DARWIN_LIMITS_VALID_STABLE_CORE;
    memcpy(out->build_identity, "rdal-v1 device-mock",
           sizeof("rdal-v1 device-mock"));
    out->api_version = 0x00401000u;
    out->driver_version = 1u;
    out->driver_id = 5u;
    out->max_descriptor_set_size = 8388606u;
    out->max_sampler_allocations = 65536u;
    out->max_bound_descriptor_sets = 32u;
    out->max_per_set_descriptors = 22369621u;
    out->max_storage_buffer_range = UINT32_MAX;
    out->max_uab_descriptors_all_pools = 67108863u;
    out->max_per_stage_descriptor_update_after_bind_samplers = 8388606u;
    out->max_descriptor_set_update_after_bind_samplers = 8388606u;
    out->max_update_after_bind_descriptors_in_all_pools = 67108863u;
    return 0;
}

@interface RDNA4MockSuperDevice : NSObject
- (NSUInteger)maxArgumentBufferSamplerCount;
@end

@implementation RDNA4MockSuperDevice
- (NSUInteger)maxArgumentBufferSamplerCount { return 777u; }
@end

@interface RDNA4MockLimitDevice : RDNA4MockSuperDevice {
    rdna4_argument_limit_snapshot _limits;
}
- (instancetype)initWithProvider:(rdna4_argument_limit_export_fn)provider;
- (instancetype)initWithMissingExport;
- (NSUInteger)maxArgumentBufferSamplerCount;
- (const rdna4_argument_limit_snapshot *)limits;
@end

@implementation RDNA4MockLimitDevice
- (instancetype)initWithProvider:(rdna4_argument_limit_export_fn)provider {
    self = [super init];
    if (self) {
        rdna4_argument_limit_policy policy = rdna4_argument_limit_default_policy();
        rdna4_argument_limit_result result =
            rdna4_argument_limit_snapshot_init_from_export(
                &_limits, provider, NULL, &policy);
        require(result == RDNA4_ARGUMENT_LIMIT_RESULT_OK,
                "mock wrapper freezes one valid export");
    }
    return self;
}

- (instancetype)initWithMissingExport {
    self = [super init];
    if (self) {
        static int loader_context;
        rdna4_argument_limit_loader loader = {
            .context = &loader_context,
            .open_library = missing_open,
            .close_library = missing_close,
            .resolve_symbol = missing_resolve,
        };
        rdna4_argument_limit_policy policy = rdna4_argument_limit_default_policy();
        rdna4_argument_limit_result result =
            rdna4_argument_limit_snapshot_init_from_loader(
                &_limits, &loader, "/mock/libvulkan_radeon.dylib", NULL, &policy);
        require(result == RDNA4_ARGUMENT_LIMIT_RESULT_EXPORT_MISSING &&
                    loader_opens == 1 && loader_resolves == 1 &&
                    loader_closes == 1,
                "old bundle resolves as a stable unavailable source");
    }
    return self;
}

- (NSUInteger)maxArgumentBufferSamplerCount {
    uint32_t limit = 0;
    if (rdna4_argument_limit_snapshot_public_limit(&_limits, &limit) ==
        RDNA4_ARGUMENT_LIMIT_RESULT_OK)
        return (NSUInteger)limit;
    return [super maxArgumentBufferSamplerCount];
}
- (const rdna4_argument_limit_snapshot *)limits { return &_limits; }
@end

int main(void) {
    @autoreleasepool {
        provider_calls = 0;
        RDNA4MockLimitDevice *ready =
            [[RDNA4MockLimitDevice alloc] initWithProvider:argument_limits_provider];
        require(provider_calls == 1,
                "wrapper initialization queries the source exactly once");
        require([ready maxArgumentBufferSamplerCount] == 21844u &&
                    [ready maxArgumentBufferSamplerCount] == 21844u &&
                    provider_calls == 1,
                "getter repeats the frozen value without querying the source");

        RDNA4MockLimitDevice *missing =
            [[RDNA4MockLimitDevice alloc] initWithMissingExport];
        const rdna4_argument_limit_snapshot *missing_limits = [missing limits];
        require([missing maxArgumentBufferSamplerCount] == 777u &&
                    [missing maxArgumentBufferSamplerCount] == 777u &&
                    provider_calls == 1,
                "unavailable source preserves the superclass Tier1 result");

        uint32_t sentinel = 0xabcdef01u;
        require(rdna4_argument_limit_snapshot_public_limit(
                    missing_limits, &sentinel) ==
                    RDNA4_ARGUMENT_LIMIT_RESULT_CAPACITY_UNAVAILABLE &&
                    sentinel == 0xabcdef01u,
                "unavailable getter never invents a numeric fallback");
    }

    printf("ARGUMENT_LIMIT_SNAPSHOT_DEVICE_PASS checks=%u opens=0 "
           "vulkan_object=0 native_open=0\n",
           checks);
    return 0;
}
