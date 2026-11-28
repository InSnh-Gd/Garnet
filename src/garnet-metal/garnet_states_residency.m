// garnet_states_residency.m - GarnetMetal modular compilation unit: samplers, depth-stencil, fences, and residency sets.
#if !defined(GARNET_INTERNAL) && !defined(GARNET_METAL_MODULAR_BUILD)
#define GARNET_INTERNAL static
#endif
#ifdef GARNET_METAL_MODULAR_BUILD
#include "garnet_metal_internal.h"
#endif
#include "garnet_residency_set.h"

// ---------------------------------------------------------------------------------------------------------------
// 11e binding model (metal2vulkan docs/REFLECTION.md, "Descriptor ABI (binding map)"; src/reflect/mod.rs constants).
// Every descriptor-backed Metal resource is a descriptor in ONE set; within the set (default ABI):
//   [[buffer(n)]]  -> binding n (0..31), storage buffer        [[texture(n)]] sampled  -> binding 32+n (32..159), sampled image
//   [[sampler(n)]] -> binding 160+n (n 0..15), sampler; a constexpr sampler ("StaticSampler") takes the first free binding in 160..191
//   writable [[texture(n)]] -> binding 480+n (480..607), storage image     synthetic (BufferAddressTable etc.) >= 640
//   compute dispatch grid -> 48-byte push-constant block (12 u32) + spec ids 0,1,2 = local size (KERNEL_LOCAL_SIZE_SPEC_IDS)
//   [[stage_in]] vertex attributes -> Vulkan vertex-input Location (= attribute index), buffer index = binding.
// Each stage is translated independently and both use set 0, so vertex buffer(0) and fragment buffer(0) would collide. Metal
// keeps the two argument tables apart, so the bundle keeps them apart too: the FRAGMENT module's DescriptorSet decorations are
// rewritten 0 -> 1 when the pipeline is built (the equivalent of TransformOptions::with_descriptor_layout(set = 1) that
// REFLECTION.md offers per independently translated stage); binding numbers inside a set are untouched. Vertex = set 0, fragment
// = set 1, compute = set 0. Static samplers and the compute dispatch contract come from spvcache/<sha>.meta.json (add-air.py).
// ---------------------------------------------------------------------------------------------------------------

// ---- samplers ----
static _Atomic uint64_t rdna4_sampler_allocations;

/*
 * actual = live VkSampler handles.
 * reserved = worst-case handles held by all live public argument configs.
 * reservedActual = live handles inside those reservations.
 * committed = actual + (reserved - reservedActual), so an admitted
 * configuration can subsequently materialize all K handles without another
 * capacity decision. Direct/static samplers consume the same actual budget.
 */
static pthread_mutex_t rdna4_sampler_budget_lock = PTHREAD_MUTEX_INITIALIZER;
static rdna4_argument_sampler_budget rdna4_sampler_budget_state;

static uint64_t rdna4_sampler_committed_locked(void) {
    uint64_t committed = UINT64_MAX;
    if (rdna4_argument_limit_snapshot_budget_committed(
            &rdna4_sampler_budget_state,
            atomic_load(&rdna4_sampler_allocations), &committed) !=
        RDNA4_ARGUMENT_PUBLIC_SAMPLER_OK)
        return UINT64_MAX;
    return committed;
}

static BOOL rdna4_sampler_budget_charge(RDNA4SamplerStorage *storage,
                                        NSError **err) {
    pthread_mutex_lock(&rdna4_sampler_budget_lock);
    const BOOL reserved = storage && storage->argumentReserved;
    if (storage && storage->handles >=
                       RDNA4_ARGUMENT_LIMIT_DEFAULT_OBJECTS_PER_CONFIG) {
        pthread_mutex_unlock(&rdna4_sampler_budget_lock);
        if (err) *err = rdna4_err(72, @"A sampler configuration exceeded three backend handles");
        return NO;
    }

    const uint64_t actual = atomic_load(&rdna4_sampler_allocations);
    int admitted = 0;
    if (reserved) {
        if (atomic_load_explicit(&rdna4_argument_limit_full_open_state,
                                 memory_order_acquire) != 1) {
            pthread_mutex_unlock(&rdna4_sampler_budget_lock);
            if (err) *err = rdna4_err(73, @"Argument-limit source has not passed the same-handle full-open crosscheck");
            return NO;
        }
        rdna4_argument_public_sampler_status status =
            rdna4_argument_limit_snapshot_budget_charge_reserved(
                &RDNA4ALS, &rdna4_sampler_budget_state, &admitted);
        if (status != RDNA4_ARGUMENT_PUBLIC_SAMPLER_OK || !admitted) {
            pthread_mutex_unlock(&rdna4_sampler_budget_lock);
            if (err) *err = rdna4_err(72, @"A sampler reservation was unavailable");
            return NO;
        }
    } else if (RDNA4ALS.state == RDNA4_ARGUMENT_LIMIT_STATE_READY &&
               atomic_load_explicit(&rdna4_argument_limit_full_open_state,
                                    memory_order_acquire) == 1) {
        rdna4_argument_public_sampler_status status =
            rdna4_argument_limit_snapshot_budget_can_charge_direct(
                &RDNA4ALS, &rdna4_sampler_budget_state, actual, 1,
                &admitted);
        if (status != RDNA4_ARGUMENT_PUBLIC_SAMPLER_OK || !admitted) {
            pthread_mutex_unlock(&rdna4_sampler_budget_lock);
            if (err) *err = rdna4_err(72, @"Backend sampler allocation limit reached");
            return NO;
        }
    } else if (actual >= RDNA4R.lim.maxSamplerAllocationCount) {
        /* Tier1 fallback: an unavailable or unverified export is never used. */
        pthread_mutex_unlock(&rdna4_sampler_budget_lock);
        if (err) *err = rdna4_err(72, @"Backend sampler allocation limit reached");
        return NO;
    }

    atomic_store(&rdna4_sampler_allocations, actual + 1u);
    if (storage) ++storage->handles;
    pthread_mutex_unlock(&rdna4_sampler_budget_lock);
    return YES;
}

static void rdna4_sampler_budget_release(RDNA4SamplerStorage *storage) {
    pthread_mutex_lock(&rdna4_sampler_budget_lock);
    if (storage && storage->argumentReserved) {
        int released = 0;
        rdna4_argument_public_sampler_status status =
            rdna4_argument_limit_snapshot_budget_release_reserved(
                &RDNA4ALS, &rdna4_sampler_budget_state, &released);
        if (status != RDNA4_ARGUMENT_PUBLIC_SAMPLER_OK || !released)
            RDNA4LOG_WARN("argument sampler budget: reserved release failed (%d)", status);
    }
    const uint64_t actual = atomic_load(&rdna4_sampler_allocations);
    if (actual) atomic_store(&rdna4_sampler_allocations, actual - 1u);
    if (storage && storage->handles) --storage->handles;
    pthread_mutex_unlock(&rdna4_sampler_budget_lock);
}

static BOOL rdna4_sampler_budget_reserve_configuration(
    RDNA4SamplerStorage *storage, uint32_t liveUniqueConfigurations,
    NSError **err) {
    if (!storage || storage->argumentReserved) return YES;
    if (RDNA4ALS.state != RDNA4_ARGUMENT_LIMIT_STATE_READY) return YES;
    if (atomic_load_explicit(&rdna4_argument_limit_full_open_state,
                             memory_order_acquire) != 1) {
        if (err) *err = rdna4_err(73, @"Argument-limit source has not passed the same-handle full-open crosscheck");
        return NO;
    }

    pthread_mutex_lock(&rdna4_sampler_budget_lock);
    int admitted = 0;
    rdna4_argument_public_sampler_status admission =
        rdna4_argument_limit_snapshot_budget_reserve_configuration(
            &RDNA4ALS, &rdna4_sampler_budget_state,
            liveUniqueConfigurations, storage->handles,
            atomic_load(&rdna4_sampler_allocations), &admitted);
    if (admission != RDNA4_ARGUMENT_PUBLIC_SAMPLER_OK || !admitted) {
        pthread_mutex_unlock(&rdna4_sampler_budget_lock);
        if (err) *err = rdna4_err(73, admission ==
            RDNA4_ARGUMENT_PUBLIC_SAMPLER_AT_LIMIT
                ? @"maxArgumentBufferSamplerCount reached"
                : @"Backend sampler object budget reached before argument admission");
        RDNA4LOG("argument sampler admission: REJECT live unique %u, actual %llu, reserved %llu/%llu, existing handles %u, limit %u, status %d",
                 liveUniqueConfigurations,
                 (unsigned long long)atomic_load(&rdna4_sampler_allocations),
                 (unsigned long long)rdna4_sampler_budget_state.reserved_actual_objects,
                 (unsigned long long)rdna4_sampler_budget_state.reserved_objects,
                 storage->handles, RDNA4ALS.public_sampler_limit, admission);
        return NO;
    }
    storage->argumentReserved = YES;
    RDNA4LOG("argument sampler admission: ADMIT live unique %u -> %u, actual %llu, committed %llu/%u",
             liveUniqueConfigurations, liveUniqueConfigurations + 1u,
             (unsigned long long)atomic_load(&rdna4_sampler_allocations),
             (unsigned long long)rdna4_sampler_committed_locked(),
             RDNA4ALS.public_sampler_limit);
    pthread_mutex_unlock(&rdna4_sampler_budget_lock);
    return YES;
}

static void rdna4_sampler_budget_release_configuration(
    RDNA4SamplerStorage *storage) {
    if (!storage || !storage->argumentReserved) return;
    pthread_mutex_lock(&rdna4_sampler_budget_lock);
    int released = 0;
    rdna4_argument_public_sampler_status status =
        rdna4_argument_limit_snapshot_budget_release_configuration(
            &RDNA4ALS, &rdna4_sampler_budget_state, storage->handles,
            &released);
    if (status == RDNA4_ARGUMENT_PUBLIC_SAMPLER_OK && released)
        storage->argumentReserved = NO;
    else
        RDNA4LOG_WARN("argument sampler budget: configuration release failed (%d, handles %u, reserved %llu/%llu)",
                      status, storage->handles,
                      (unsigned long long)rdna4_sampler_budget_state.reserved_actual_objects,
                      (unsigned long long)rdna4_sampler_budget_state.reserved_objects);
    pthread_mutex_unlock(&rdna4_sampler_budget_lock);
}

GARNET_INTERNAL VkSampler rdna4_make_sampler(const RDNA4SampCfg *c, BOOL comparison,
                                    BOOL normalizedCoordinates,
                                    RDNA4SamplerStorage *budgetStorage,
                                    NSError **err) {
    if (!rdna4_radv_open(err)) return VK_NULL_HANDLE;
    VkSamplerCreateInfo sc;
    const char *invalid = normalizedCoordinates
        ? rdna4_sampler_info_normalized(c, comparison, RDNA4R.samplerAnisotropy,
            RDNA4R.lim.maxSamplerAnisotropy, RDNA4R.samplerMirrorClamp, &sc)
        : rdna4_sampler_info(c, comparison, RDNA4R.samplerAnisotropy,
            RDNA4R.lim.maxSamplerAnisotropy, RDNA4R.samplerMirrorClamp, &sc);
    if (invalid) { if (err) *err = rdna4_err(71, [NSString stringWithUTF8String:invalid]); return VK_NULL_HANDLE; }
    if (!rdna4_sampler_budget_charge(budgetStorage, err))
        return VK_NULL_HANDLE;
    VkSampler s = VK_NULL_HANDLE;
    VkResult r = vkCreateSampler(RDNA4R.dev, &sc, NULL, &s);
    if (r != VK_SUCCESS) {
        rdna4_sampler_budget_release(budgetStorage);
        if (err) *err = rdna4_err(72, [NSString stringWithFormat:@"vkCreateSampler = %d", r]);
        return VK_NULL_HANDLE;
    }
    return s;
}

GARNET_INTERNAL void rdna4_destroy_sampler(VkSampler sampler,
                                  RDNA4SamplerStorage *budgetStorage) {
    if (RDNA4R.ok && sampler) {
        vkDestroySampler(RDNA4R.dev, sampler, NULL);
        rdna4_sampler_budget_release(budgetStorage);
    }
}

@implementation RDNA4SamplerStorage
- (void)dealloc {
    rdna4_destroy_sampler(normal, self);
    rdna4_destroy_sampler(comparison, self);
    rdna4_destroy_sampler(normalizedNormal, self);
}
@end

static NSMapTable<NSData *, RDNA4SamplerStorage *> *rdna4_sampler_configs;
static NSUInteger rdna4_argument_sampler_configs;

static NSData *rdna4_sampler_config_key(const RDNA4SampCfg *c) {
    uint32_t words[] = { (uint32_t)c->minF, (uint32_t)c->magF, (uint32_t)c->mipF,
        (uint32_t)c->as, (uint32_t)c->at, (uint32_t)c->ar, (uint32_t)c->border,
        (uint32_t)c->cmp, (uint32_t)c->unnorm, 0, 0, c->anisotropy };
    float low = c->lodMin == 0 ? 0 : c->lodMin, high = c->lodMax == 0 ? 0 : c->lodMax;
    memcpy(&words[9], &low, sizeof low);
    memcpy(&words[10], &high, sizeof high);
    return [NSData dataWithBytes:words length:sizeof words];
}

static void rdna4_sampler_storage_release(RDNA4SamplerStorage *storage,
                                           BOOL argumentBuffers) {
    if (!storage) return;
    @synchronized ([RDNA4SamplerStorage class]) {
        if (!storage->references) return;
        if (argumentBuffers && !--storage->argumentReferences) {
            --rdna4_argument_sampler_configs;
            rdna4_sampler_budget_release_configuration(storage);
        }
        if (!--storage->references) {
            NSData *key = rdna4_sampler_config_key(&storage->config);
            [rdna4_sampler_configs removeObjectForKey:key];
        }
    }
}

static const char *rdna4_sampler_lod_clamp_invalid(double lodMin, double lodMax) {
    if (!isfinite(lodMin) || !isfinite(lodMax) || lodMin < 0 || lodMax < lodMin)
        return "sampler LOD clamps must be finite, nonnegative and ordered";
    return NULL;
}

static VkSampler rdna4_make_sampler_bounded(const RDNA4SampCfg *config,
    BOOL comparison, BOOL normalized, RDNA4SamplerStorage *storage, NSError **err) {
    RDNA4SampCfg bounded = *config;
    // rdna4_sampler_info caps maxLod at 1000. Cap minLod identically so a
    // finite request above that backend bound still creates an ordered sampler.
    if (!bounded.unnorm && bounded.mipF != 0) {
        if (bounded.lodMin > 1000.0f) bounded.lodMin = 1000.0f;
        if (bounded.lodMax > 1000.0f) bounded.lodMax = 1000.0f;
    }
    return rdna4_make_sampler(&bounded, comparison, normalized, storage, err);
}

static const char *rdna4_sampler_descriptor_invalid(MTLSamplerDescriptor *d) {
    if (!d) return "nil sampler descriptor";
    // Check the full-width properties before any narrowing cast. A value such as
    // 0x100000000 would otherwise truncate to a legal Metal enum value.
    if ((NSUInteger)d.minFilter > 1 || (NSUInteger)d.magFilter > 1)
        return "invalid sampler min/mag filter";
    if ((NSUInteger)d.mipFilter > 2)
        return "invalid sampler mip filter";
    if ((NSUInteger)d.sAddressMode > 5 || (NSUInteger)d.tAddressMode > 5 ||
        (NSUInteger)d.rAddressMode > 5)
        return "unsupported sampler address mode";
    if ((NSUInteger)d.borderColor > 2)
        return "invalid sampler border color";
    if ((NSUInteger)d.compareFunction > 7)
        return "invalid sampler comparison function";
    if (d.maxAnisotropy < 1 || d.maxAnisotropy > 16)
        return "sampler maxAnisotropy must be in 1..16";
    return rdna4_sampler_lod_clamp_invalid(d.lodMinClamp, d.lodMaxClamp);
}

// Both public samplers and effective LOD bindings acquire one canonical config.
// Publish ownership only after admission and handle creation have succeeded.
static RDNA4SamplerStorage *rdna4_sampler_storage_acquire(
    const RDNA4SampCfg *config, BOOL argumentBuffers, NSError **err) {
    if (!rdna4_radv_open(err)) return nil;
    VkSamplerCreateInfo info;
    const char *invalid = rdna4_sampler_info(config, NO, RDNA4R.samplerAnisotropy,
        RDNA4R.lim.maxSamplerAnisotropy, RDNA4R.samplerMirrorClamp, &info);
    if (invalid) {
        if (err) *err = rdna4_err(71, [NSString stringWithUTF8String:invalid]);
        return nil;
    }
    @synchronized ([RDNA4SamplerStorage class]) {
        if (!rdna4_sampler_configs)
            rdna4_sampler_configs = [NSMapTable strongToWeakObjectsMapTable];
        NSData *key = rdna4_sampler_config_key(config);
        RDNA4SamplerStorage *storage = [rdna4_sampler_configs objectForKey:key];
        if (!storage) {
            storage = [RDNA4SamplerStorage new];
            storage->config = *config;
        }
        const BOOL firstArgumentConfiguration = argumentBuffers &&
            storage->argumentReferences == 0;
        if (firstArgumentConfiguration && RDNA4ALS.state == RDNA4_ARGUMENT_LIMIT_STATE_READY) {
            if (rdna4_argument_sampler_configs > UINT32_MAX ||
                !rdna4_sampler_budget_reserve_configuration(storage,
                    (uint32_t)rdna4_argument_sampler_configs, err)) return nil;
        }
        if (!storage->normal) {
            storage->normal = rdna4_make_sampler_bounded(config, NO, NO, storage, err);
            if (!storage->normal) {
                if (firstArgumentConfiguration)
                    rdna4_sampler_budget_release_configuration(storage);
                return nil;
            }
        }
        [rdna4_sampler_configs setObject:storage forKey:key];
        if (argumentBuffers && !storage->argumentReferences++)
            ++rdna4_argument_sampler_configs;
        ++storage->references;
        return storage;
    }
}

static VkSampler rdna4_sampler_storage_handle(RDNA4SamplerStorage *storage,
    BOOL comparison, BOOL normalized, NSError **err) {
    if (!storage) {
        if (err) *err = rdna4_err(71, @"sampler storage is unavailable");
        return VK_NULL_HANDLE;
    }
    if (!normalized) {
        if (!comparison) return storage->normal;
        @synchronized (storage) {
            // A pixel source has no legacy unnormalized comparison representation.
            if (storage->config.unnorm)
                return rdna4_make_sampler_bounded(&storage->config, YES, NO, storage, err);
            if (!storage->comparison)
                storage->comparison = rdna4_make_sampler_bounded(&storage->config, YES, NO, storage, err);
            return storage->comparison;
        }
    }
    @synchronized (storage) {
        if (!storage->config.unnorm) {
            if (!comparison) return storage->normal;
            if (!storage->comparison)
                storage->comparison = rdna4_make_sampler_bounded(&storage->config, YES, YES, storage, err);
            return storage->comparison;
        }
        if (!comparison) {
            if (!storage->normalizedNormal)
                storage->normalizedNormal = rdna4_make_sampler_bounded(&storage->config, NO, YES, storage, err);
            return storage->normalizedNormal;
        }
        if (!storage->comparison)
            storage->comparison = rdna4_make_sampler_bounded(&storage->config, YES, YES, storage, err);
        return storage->comparison;
    }
}

// A direct binding can override the sampler's LOD clamps without changing the
// public sampler/token identity. The wrapper points at either the shared base
// storage or a shared internal variant keyed by the effective sampler config.

@implementation RDNA4SamplerBinding
+ (instancetype)bindingWithBaseStorage:(RDNA4SamplerStorage *)base
                         lodMinClamp:(float)lodMin
                         lodMaxClamp:(float)lodMax
                               error:(NSError **)err {
    if (!base) {
        if (err) *err = rdna4_err(71, @"sampler binding has no base storage");
        return nil;
    }
    const char *clampInvalid = rdna4_sampler_lod_clamp_invalid(lodMin, lodMax);
    if (clampInvalid) {
        if (err) *err = rdna4_err(71, [NSString stringWithUTF8String:clampInvalid]);
        return nil;
    }
    RDNA4SampCfg config = base->config;
    config.lodMin = lodMin;
    config.lodMax = lodMax;
    BOOL override = config.lodMin != base->config.lodMin ||
        config.lodMax != base->config.lodMax;
    RDNA4SamplerStorage *storage = rdna4_sampler_storage_acquire(&config, NO, err);
    if (!storage) return nil;
    RDNA4SamplerBinding *binding = [RDNA4SamplerBinding new];
    if (!binding) {
        rdna4_sampler_storage_release(storage, NO);
        if (err) *err = rdna4_err(72, @"sampler binding allocation failed");
        return nil;
    }
    binding->_storage = storage;
    binding->_lodMinClamp = config.lodMin;
    binding->_lodMaxClamp = config.lodMax;
    binding->_lodClampOverride = override;
    return binding;
}
- (void)dealloc {
    rdna4_sampler_storage_release(_storage, NO);
}
- (RDNA4SamplerStorage *)rdna4Storage { return _storage; }
- (BOOL)rdna4HasLODClampOverride { return _lodClampOverride; }
- (float)rdna4LODMinClamp { return _lodMinClamp; }
- (float)rdna4LODMaxClamp { return _lodMaxClamp; }
- (VkSampler)vkSampler { return _storage->normal; }
- (VkSampler)rdna4SamplerComparison:(BOOL)comparison error:(NSError **)err {
    return rdna4_sampler_storage_handle(_storage, comparison, NO, err);
}
- (VkSampler)rdna4SamplerComparison:(BOOL)comparison
               normalizedCoordinates:(BOOL)normalized
                               error:(NSError **)err {
    return rdna4_sampler_storage_handle(_storage, comparison, normalized, err);
}
- (BOOL)rdna4PixelCoordinates { return _storage->config.unnorm != 0; }
@end

@implementation RDNA4SamplerState
- (instancetype)initWithDevice:(id)dev descriptor:(MTLSamplerDescriptor *)d error:(NSError **)err {
    self = [super init]; if (!self) return nil;
    _dev = dev; _lbl = [d.label copy];
    const char *descriptorInvalid = rdna4_sampler_descriptor_invalid(d);
    if (descriptorInvalid) {
        if (err) *err = rdna4_err(71, [NSString stringWithUTF8String:descriptorInvalid]);
        return nil;
    }
    RDNA4SampCfg c = { .minF = (int)d.minFilter, .magF = (int)d.magFilter, .mipF = (int)d.mipFilter, .as = (int)d.sAddressMode, .at = (int)d.tAddressMode,
        .ar = (int)d.rAddressMode, .border = (int)d.borderColor, .cmp = (int)d.compareFunction, .unnorm = !d.normalizedCoordinates,
        .lodMin = (float)d.lodMinClamp, .lodMax = (float)d.lodMaxClamp, .anisotropy = (unsigned)d.maxAnisotropy };
    _storage = rdna4_sampler_storage_acquire(&c, d.supportArgumentBuffers, err);
    if (!_storage) return nil;
    _argumentBuffers = d.supportArgumentBuffers;
    RDNA4LOGR("RDNA4SamplerState %p: vk %p (min %d mag %d mip %d addr %d/%d/%d cmp %d %s)", (__bridge void *)self, (void *)_storage->normal, c.minF, c.magF, c.mipF, c.as, c.at, c.ar, c.cmp, c.unnorm ? "unnormalized" : "normalized");
    return self;
}
- (void)dealloc {
    rdna4_sampler_storage_release(_storage, _argumentBuffers);
}
RDNA4_DNR(RDNA4SamplerState)
- (RDNA4SamplerStorage *)rdna4ArgumentIdentity { return _storage; }
- (RDNA4SamplerBinding *)rdna4Binding:(NSError **)err {
    @synchronized (self) {
        if (!_baseBinding)
            _baseBinding = [RDNA4SamplerBinding bindingWithBaseStorage:_storage
                lodMinClamp:_storage->config.lodMin lodMaxClamp:_storage->config.lodMax
                error:err];
        return _baseBinding;
    }
}
- (RDNA4SamplerBinding *)rdna4BindingWithLODMinClamp:(float)lodMin
                                          lodMaxClamp:(float)lodMax
                                                error:(NSError **)err {
    return [RDNA4SamplerBinding bindingWithBaseStorage:_storage lodMinClamp:lodMin
        lodMaxClamp:lodMax error:err];
}
- (VkSampler)vkSampler { return _storage->normal; }
- (BOOL)rdna4PixelCoordinates { return _storage->config.unnorm != 0; }
- (VkSampler)rdna4SamplerComparison:(BOOL)comparison error:(NSError **)err {
    return rdna4_sampler_storage_handle(_storage, comparison, NO, err);
}
- (VkSampler)rdna4SamplerComparison:(BOOL)comparison normalizedCoordinates:(BOOL)normalized error:(NSError **)err {
    return rdna4_sampler_storage_handle(_storage, comparison, normalized, err);
}
- (BOOL)supportArgumentBuffers { return _argumentBuffers; }
- (MTLResourceID)gpuResourceID API_AVAILABLE(macos(13.0)) {
    MTLResourceID identifier = { 0 };
    if (!_argumentBuffers) {
        RDNA4_ONCE("RDNA4SamplerState.gpuResourceID: sampler created with supportArgumentBuffers=NO; returning 0");
        return identifier;
    }
    identifier._impl = rdna4_argument_resource_token_bindless(self, _dev, NO, YES);
    return identifier;
}
- (id)device { return _dev; }
- (NSString *)label { return _lbl; }
- (void)setLabel:(NSString *)l { _lbl = [l copy]; }
@end

// ---- gap census: depth/stencil state and fences ----
// MTLDepthStencilState: SkyLight and QuartzCore create one each at start-up (-newDepthStencilStateWithDescriptor:, census rdt1), and the compositor sends
// -setDepthStencilState: on its encoders. The render passes here have colour attachments only (no depth/stencil exist), for which Metal makes the
// state a no-op, so this object records the descriptor and nothing else.
// bundle 10: the state IS used now (a pipeline with a depth/stencil attachment reads it at every draw): compare, depth write, the two stencil faces (compare, ops, masks).
GARNET_INTERNAL RDNA4DSInfo rdna4_dsinfo_default(void) { return (RDNA4DSInfo){ RDNA4DS_CMP_ALWAYS, 0, { RDNA4DS_CMP_ALWAYS, 0, 0, 0 }, { RDNA4DS_CMP_ALWAYS, 0, 0, 0 }, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu }; }
@implementation RDNA4DepthStencilState
- (instancetype)initWithDevice:(id)dev descriptor:(MTLDepthStencilDescriptor *)d {
    self = [super init]; if (!self) return nil;
    _dev = dev; _d = [d copy]; _lbl = [d.label copy];
    _info = rdna4_dsinfo_default();
    _info.cmp = (unsigned long)d.depthCompareFunction; _info.write = d.depthWriteEnabled ? 1 : 0;   // bundle 10
    MTLStencilDescriptor *fs = d.frontFaceStencil, *bs = d.backFaceStencil;   // nil = Always / Keep / masks all ones
    if (fs) { _info.f = (rdna4ds_face_in_t){ (unsigned long)fs.stencilCompareFunction, (unsigned long)fs.stencilFailureOperation, (unsigned long)fs.depthFailureOperation, (unsigned long)fs.depthStencilPassOperation }; _info.fRead = fs.readMask; _info.fWrite = fs.writeMask; }
    if (bs) { _info.b = (rdna4ds_face_in_t){ (unsigned long)bs.stencilCompareFunction, (unsigned long)bs.stencilFailureOperation, (unsigned long)bs.depthFailureOperation, (unsigned long)bs.depthStencilPassOperation }; _info.bRead = bs.readMask; _info.bWrite = bs.writeMask; }
    RDNA4LOG("RDNA4DepthStencilState %p: depth compare %lu write %d; stencil front cmp %lu ops %lu/%lu/%lu masks %x/%x, back cmp %lu ops %lu/%lu/%lu masks %x/%x (used by pipelines with a depth/stencil attachment)", (__bridge void *)self,
           _info.cmp, _info.write, _info.f.cmp, _info.f.fail, _info.f.dfail, _info.f.pass, _info.fRead, _info.fWrite, _info.b.cmp, _info.b.fail, _info.b.dfail, _info.b.pass, _info.bRead, _info.bWrite);
    return self;
}
RDNA4_DNR(RDNA4DepthStencilState)
- (MTLDepthStencilDescriptor *)rdna4Descriptor { return _d; }
- (RDNA4DSInfo)rdna4Info { return _info; }
- (id)device { return _dev; }
- (NSString *)label { return _lbl; }
- (void)setLabel:(NSString *)l { _lbl = [l copy]; }
@end

// A fence update is a checkpoint at an encoded command-buffer position. The
// producer segment's Vulkan fence completes the checkpoint; a consumer segment
// that waits on it is not submitted until then. This is deliberately stronger
// than a scoped stage dependency: the backend keeps a conservative full barrier.


@implementation RDNA4FenceCheckpoint
- (void)rdna4Ready {
    int want = RDNA4FenceCheckpointPending;
    (void)atomic_compare_exchange_strong_explicit(&state, &want, RDNA4FenceCheckpointReady,
                                                  memory_order_release, memory_order_relaxed);
}
- (void)rdna4Fail:(VkResult)result {
    atomic_store_explicit(&fault, (int)result, memory_order_relaxed);
    int current = atomic_load_explicit(&state, memory_order_relaxed);
    while (current == RDNA4FenceCheckpointPending) {
        int want = RDNA4FenceCheckpointPending;
        if (atomic_compare_exchange_weak_explicit(&state, &want, RDNA4FenceCheckpointFailed,
                                                  memory_order_release, memory_order_relaxed)) break;
        current = want;
    }
}
@end

// MTLFence identity plus generation state. A wait captures the latest update
// encoded at that point. A new fence starts at a ready generation-zero
// checkpoint, so waiting before any update is a completed no-op dependency; a
// later update creates a new pending checkpoint and never retargets that wait.
@implementation RDNA4Fence
- (instancetype)initWithDevice:(id)dev {
    self = [super init];
    if (!self) return nil;
    _dev = dev;
    pthread_mutex_init(&_mu, NULL);
    _latestCheckpoint = [RDNA4FenceCheckpoint new];
    _latestCheckpoint->generation = 0;
    [_latestCheckpoint rdna4Ready];
    return self;
}
- (void)dealloc { pthread_mutex_destroy(&_mu); }
RDNA4_DNR(RDNA4Fence)
- (RDNA4FenceCheckpoint *)rdna4UpdateCheckpointForCB:(uint64_t)cbid {
    pthread_mutex_lock(&_mu);
    if (_latestGeneration == UINT64_MAX) {
        pthread_mutex_unlock(&_mu);
        return nil;
    }
    uint64_t generation = _latestGeneration + 1;
    RDNA4FenceCheckpoint *checkpoint = [RDNA4FenceCheckpoint new];
    checkpoint->generation = generation;
    checkpoint->producerCBId = cbid;
    _latestGeneration = generation;
    _latestCheckpoint = checkpoint;
    pthread_mutex_unlock(&_mu);
    return checkpoint;
}
- (RDNA4FenceCheckpoint *)rdna4UpdateCheckpoint { return [self rdna4UpdateCheckpointForCB:0]; }
- (RDNA4FenceCheckpoint *)rdna4WaitCheckpoint {
    pthread_mutex_lock(&_mu);
    RDNA4FenceCheckpoint *checkpoint = _latestCheckpoint;
    pthread_mutex_unlock(&_mu);
    return checkpoint;
}
- (id)device { return _dev; }
- (NSString *)label { return _lbl; }
- (void)setLabel:(NSString *)l { _lbl = [l copy]; }
@end

// Indirect resources must survive command execution, even without a bound descriptor.

// ---------------------------------------------------------------------------------------------------------------
// Metal 3.2 Residency Sets (Task 4.2): RDNA4ResidencySnapshot & RDNA4ResidencySet.
//
// Wave 2B GarnetDevice.m integration:
//   1. Include order in GarnetDevice.m: place after `#include "rdna4_gl_drawable_impl.m"`
//      (after RDNA4Buffer, RDNA4Texture, RDNA4Heap, and RDNA4Residency*Internal protocols in
//      rdna4_command_queue_impl.inc are defined):
//        #include "rdna4_residency_set_impl.m"
//        #include "rdna4_io_queue_impl.m"
//   2. Device factory selector on @implementation GarnetDevice:
//        - (id)newResidencySetWithDescriptor:(id)descriptor error:(NSError **)error {
//            return [[RDNA4ResidencySet alloc] initWithDevice:self descriptor:descriptor error:error];
//        }
//   3. Optional 2-line Tier 2 texture hook in `rdna4_argument_table_commands.inc` (`rdna4PrepareArguments:`):
//        NSSet *residency = [self rdna4ResidencyDeclaredResources];
//        if (residency.count) resources = [resources setByAddingObjectsFromSet:residency];
//      (Indirect BDA buffers are already merged directly in `rdna4_command_buffer_impl.inc` `rdna4FillSet:`.)
//   4. Protocol MTLResidencySet is attached automatically in +initialize via rdna4_add_protocols()
//      and may also be listed in GarnetDevice +load census.
// ---------------------------------------------------------------------------------------------------------------

static BOOL rdna4_residency_alloc_valid(id obj, id dev, uint64_t *sizeOut) {
    if (sizeOut) *sizeOut = 0;
    if (!obj || !dev) return NO;
    if (![obj respondsToSelector:@selector(device)] || [obj device] != dev) return NO;
    BOOL knownType = [obj isKindOfClass:[RDNA4Buffer class]] ||
                     [obj isKindOfClass:[RDNA4Texture class]] ||
                     [obj isKindOfClass:[RDNA4Heap class]];
    if (!knownType) {
        Class asCls = objc_getClass("GarnetAccelerationStructure");
        Class spCls = objc_getClass("RDNA4SparseHeap");
        if ((asCls && [obj isKindOfClass:asCls]) ||
            (spCls && [obj isKindOfClass:spCls]))
            knownType = YES;
    }
    if (!knownType) {
        Protocol *pAlloc = objc_getProtocol("MTLAllocation");
        Protocol *pRes = objc_getProtocol("MTLResource");
        Protocol *pHeap = objc_getProtocol("MTLHeap");
        Protocol *pAS = objc_getProtocol("MTLAccelerationStructure");
        if ((pAlloc && [obj conformsToProtocol:pAlloc]) ||
            (pRes && [obj conformsToProtocol:pRes]) ||
            (pHeap && [obj conformsToProtocol:pHeap]) ||
            (pAS && [obj conformsToProtocol:pAS]))
            knownType = YES;
    }
    if (!knownType) return NO;
    if (sizeOut) {
        if ([obj respondsToSelector:@selector(allocatedSize)])
            *sizeOut = (uint64_t)((NSUInteger (*)(id, SEL))objc_msgSend)(obj, @selector(allocatedSize));
        else if ([obj respondsToSelector:@selector(size)])
            *sizeOut = (uint64_t)((NSUInteger (*)(id, SEL))objc_msgSend)(obj, @selector(size));
    }
    return YES;
}


@implementation RDNA4ResidencySnapshot
- (instancetype)initWithAllocations:(NSArray *)allocations
                      allocatedSize:(uint64_t)allocatedSize
                         generation:(uint64_t)generation {
    self = [super init];
    if (!self) return nil;
    _allocations = allocations ? [allocations copy] : @[];
    _allocatedSize = allocatedSize;
    _generation = generation;
    return self;
}

- (NSArray *)rdna4Allocations { return _allocations; }
- (uint64_t)rdna4AllocatedSize { return _allocatedSize; }
- (uint64_t)rdna4Generation { return _generation; }

- (void)rdna4AddBuffersToSet:(NSMutableSet *)buffers device:(id)device {
    if (!buffers || !device) return;
    for (id obj in _allocations) {
        if ([obj isKindOfClass:[RDNA4Buffer class]]) {
            RDNA4Buffer *b = (RDNA4Buffer *)obj;
            if ([b device] == device && ![b isAliasable]) [buffers addObject:b];
        } else if ([obj isKindOfClass:[RDNA4Heap class]]) {
            RDNA4Heap *h = (RDNA4Heap *)obj;
            if ([h device] != device) continue;
            for (id res in [h rdna4LiveResources]) {
                if ([res isKindOfClass:[RDNA4Buffer class]] && [(RDNA4Buffer *)res device] == device &&
                    ![res isAliasable])
                    [buffers addObject:res];
            }
        }
    }
}
@end


@implementation RDNA4ResidencySet {
    id _dev;
    NSString *_lbl;
    pthread_mutex_t _mu;
    rdna4rs_set_t _state;
    NSMutableDictionary<NSNumber *, id> *_stagedObjects;
    NSMutableDictionary<NSNumber *, id> *_committedObjects;
    RDNA4ResidencySnapshot *_snapshot;
}
+ (void)initialize {
    if (self == [RDNA4ResidencySet class]) {
        static dispatch_once_t once;
        dispatch_once(&once, ^{
            const char *n[] = { "MTLResidencySet" };
            rdna4_add_protocols(self, n, 1, "RDNA4ResidencySet");
        });
    }
}
RDNA4_DNR(RDNA4ResidencySet)

- (instancetype)initWithDevice:(id)dev descriptor:(id)desc error:(NSError **)err {
    self = [super init];
    if (!self) return nil;
    if (!dev) {
        if (err) *err = rdna4_err(130, @"residency set requires a valid device");
        return nil;
    }
    NSUInteger initialCap = 0;
    NSString *lbl = nil;
    if (desc) {
        SEL capSel = sel_registerName("initialCapacity");
        if ([desc respondsToSelector:capSel])
            initialCap = ((NSUInteger (*)(id, SEL))objc_msgSend)(desc, capSel);
        SEL lblSel = sel_registerName("label");
        if ([desc respondsToSelector:lblSel]) {
            id raw = ((id (*)(id, SEL))objc_msgSend)(desc, lblSel);
            if ([raw isKindOfClass:[NSString class]]) lbl = [(NSString *)raw copy];
        }
    }
    if (!rdna4rs_validate_capacity((uint64_t)initialCap) || !rdna4rs_init(&_state, (uint64_t)initialCap)) {
        if (err) *err = rdna4_err(130,
            [NSString stringWithFormat:@"residency set initialCapacity %lu is invalid or cannot be allocated",
             (unsigned long)initialCap]);
        return nil;
    }
    _dev = dev;
    _lbl = lbl;
    pthread_mutex_init(&_mu, NULL);
    _stagedObjects = [NSMutableDictionary dictionary];
    _committedObjects = [NSMutableDictionary dictionary];
    _snapshot = [[RDNA4ResidencySnapshot alloc] initWithAllocations:@[] allocatedSize:0 generation:0];
    return self;
}

- (void)dealloc {
    pthread_mutex_lock(&_mu);
    rdna4rs_destroy(&_state);
    [_stagedObjects removeAllObjects];
    [_committedObjects removeAllObjects];
    _snapshot = nil;
    pthread_mutex_unlock(&_mu);
    pthread_mutex_destroy(&_mu);
}

- (id)device { return _dev; }
- (NSString *)label { return _lbl; }

- (uint64_t)allocatedSize {
    pthread_mutex_lock(&_mu);
    uint64_t sz = [_snapshot rdna4AllocatedSize];
    pthread_mutex_unlock(&_mu);
    return sz;
}

- (NSUInteger)allocationCount {
    pthread_mutex_lock(&_mu);
    NSUInteger n = [_snapshot rdna4Allocations].count;
    pthread_mutex_unlock(&_mu);
    return n;
}

- (NSArray *)allAllocations {
    pthread_mutex_lock(&_mu);
    NSArray *arr = [_snapshot rdna4Allocations];
    pthread_mutex_unlock(&_mu);
    return arr;
}

- (BOOL)containsAllocation:(id)allocation {
    if (!allocation) return NO;
    uintptr_t key = (uintptr_t)(__bridge void *)allocation;
    pthread_mutex_lock(&_mu);
    BOOL present = rdna4rs_contains(&_state, key) != 0;
    pthread_mutex_unlock(&_mu);
    return present;
}

- (void)addAllocation:(id)allocation {
    uint64_t sz = 0;
    if (!rdna4_residency_alloc_valid(allocation, _dev, &sz)) return;
    uintptr_t key = (uintptr_t)(__bridge void *)allocation;
    pthread_mutex_lock(&_mu);
    if (rdna4rs_add(&_state, key, sz)) _stagedObjects[@(key)] = allocation;
    pthread_mutex_unlock(&_mu);
}

- (void)addAllocations:(const id __unsafe_unretained *)allocations count:(NSUInteger)count {
    if (!allocations || !count) return;
    for (NSUInteger i = 0; i < count; i++) [self addAllocation:allocations[i]];
}

- (void)removeAllocation:(id)allocation {
    if (!allocation) return;
    uintptr_t key = (uintptr_t)(__bridge void *)allocation;
    int removedPending = 0;
    pthread_mutex_lock(&_mu);
    if (rdna4rs_remove(&_state, key, &removedPending) && removedPending)
        [_stagedObjects removeObjectForKey:@(key)];
    pthread_mutex_unlock(&_mu);
}

- (void)removeAllocations:(const id __unsafe_unretained *)allocations count:(NSUInteger)count {
    if (!allocations || !count) return;
    for (NSUInteger i = 0; i < count; i++) [self removeAllocation:allocations[i]];
}

- (void)removeAllAllocations {
    pthread_mutex_lock(&_mu);
    rdna4rs_remove_all(&_state);
    [_stagedObjects removeAllObjects];
    pthread_mutex_unlock(&_mu);
}

- (void)commit {
    pthread_mutex_lock(&_mu);
    rdna4rs_diff_t diff = {0};
    if (!rdna4rs_commit(&_state, &diff)) {
        pthread_mutex_unlock(&_mu);
        RDNA4LOG_ERR("RDNA4ResidencySet %p: commit failed (out of memory); preserving prior snapshot", (__bridge void *)self);
        return;
    }
    for (size_t i = 0; i < diff.added_count; i++) {
        NSNumber *k = @(diff.added[i]);
        id obj = _stagedObjects[k];
        if (obj) _committedObjects[k] = obj;
    }
    for (size_t i = 0; i < diff.removed_count; i++) {
        [_committedObjects removeObjectForKey:@(diff.removed[i])];
    }
    [_stagedObjects removeAllObjects];

    NSMutableArray *arr = [NSMutableArray arrayWithCapacity:_state.committed_count];
    for (size_t i = 0; i < _state.committed_count; i++) {
        id obj = _committedObjects[@(_state.committed[i].key)];
        if (obj) [arr addObject:obj];
    }
    _snapshot = [[RDNA4ResidencySnapshot alloc] initWithAllocations:arr
                                                      allocatedSize:_state.committed_size
                                                         generation:_state.generation];
    rdna4rs_diff_destroy(&diff);
    pthread_mutex_unlock(&_mu);
}

- (void)requestResidency {
    pthread_mutex_lock(&_mu);
    rdna4rs_request_residency(&_state);
    pthread_mutex_unlock(&_mu);
}

- (void)endResidency {
    pthread_mutex_lock(&_mu);
    rdna4rs_end_residency(&_state);
    pthread_mutex_unlock(&_mu);
}

- (id<RDNA4ResidencySnapshotInternal>)rdna4CommittedSnapshot {
    pthread_mutex_lock(&_mu);
    RDNA4ResidencySnapshot *snap = _snapshot;
    pthread_mutex_unlock(&_mu);
    return snap;
}

- (BOOL)rdna4IsResidentRequested {
    pthread_mutex_lock(&_mu);
    BOOL active = _state.resident_requested != 0;
    pthread_mutex_unlock(&_mu);
    return active;
}
@end
