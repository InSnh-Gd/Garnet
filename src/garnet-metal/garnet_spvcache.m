// garnet_spvcache.m - GarnetMetal modular compilation unit.
#if !defined(GARNET_INTERNAL) && !defined(GARNET_METAL_MODULAR_BUILD)
#define GARNET_INTERNAL static
#endif
#ifdef GARNET_METAL_MODULAR_BUILD
#include "garnet_metal_internal.h"
#endif
#include "garnet_arguments_internal.h"
#include "garnet_buffer_registry.h"
#include "garnet_hotswap.h"
#include "garnet_buffer_ranges.h"
#include "garnet_shaders_internal.h"

// ---- spvcache sidecars (<sha>.meta.json, add-air.py) ----
// Hot-swap H2: the lookup directories, in order: the bundle's Resources/spvcache (read-only in practice), then (bundle 14, applications that translate in process) the user cache directory K,
// then the side directory /private/var/tmp/garnet-spv (WindowServer-sandbox readable; the profile allows /private/var/tmp; the translator installs new <sha>.meta.json then
// <sha>.spv there by atomic rename, .spv LAST). A sandboxed application does not look in the side directory (the sandbox denies it and logs every try).
// Test hooks (root + GARNET_ALLOW=1): GARNET_TEST_SIDE_DIR replaces the side dir, GARNET_TEST_HIDE_BUNDLE_SPV=1 drops the bundle dir.
static NSString *rdna4_side_dir(void) {
    static NSString *d; static dispatch_once_t o;
    dispatch_once(&o, ^{ const char *t = garnet_getenv("GARNET_TEST_SIDE_DIR", "RDNA4M_TEST_SIDE_DIR"); d = (rdna4_allow() && t && *t) ? @(t) : @"/private/var/tmp/garnet-spv"; });
    return d;
}
GARNET_INTERNAL NSArray<NSString *> *rdna4_spv_dirs(void) {
    static NSArray *bundleOnly, *a[4]; static dispatch_once_t o; static NSString *userFor;
    dispatch_once(&o, ^{
        const char *h = garnet_getenv("GARNET_TEST_HIDE_BUNDLE_SPV", "RDNA4M_TEST_HIDE_BUNDLE_SPV");
        if (!(rdna4_allow() && h && !strcmp(h, "1"))) bundleOnly = @[ [[[NSBundle bundleForClass:[GarnetDevice class]] resourcePath] stringByAppendingPathComponent:@"spvcache"] ];
        else bundleOnly = @[];
    });
    static BOOL sideReadable; static dispatch_once_t sideOnce;
    dispatch_once(&sideOnce, ^{
        const char *sd = rdna4_side_dir().fileSystemRepresentation;
        sideReadable = rdna4_sb_can_stat(sd) && rdna4_sb_can_read(sd);
    });
    NSString *u = rdna4x_user_dir();                                   // nil unless this process translates in process
    const BOOL skipSide = !sideReadable || (u && rdna4x_skip_side_dir(rdna4_is_ws(), rdna4x_sandboxed()));
    const int idx = (u ? 1 : 0) | (skipSide ? 2 : 0);
    @synchronized ([GarnetDevice class]) {
        if (!a[idx] || (u && ![userFor isEqualToString:u])) {
            NSMutableArray *m = [bundleOnly mutableCopy];
            if (u) { [m addObject:u]; userFor = u; }
            if (!skipSide) [m addObject:rdna4_side_dir()];
            a[idx] = m;
        }
        return a[idx];
    }
}
GARNET_INTERNAL NSDictionary *rdna4_meta_for_spv(NSString *path) {
    NSString *metadataPath = [[path stringByDeletingPathExtension] stringByAppendingString:@".meta.json"];
    NSData *d = [NSData dataWithContentsOfFile:metadataPath];
    if (!d.length) return nil;
    id j = [NSJSONSerialization JSONObjectWithData:d options:0 error:NULL];
    return [j isKindOfClass:[NSDictionary class]] ? j : nil;
}
static int rdna4_idx(id s, const char *const *tbl) {
    if (![s isKindOfClass:[NSString class]]) return -1;
    for (int i = 0; tbl[i]; i++) if (!strcmp(tbl[i], [(NSString *)s UTF8String])) return i;
    return -1;
}
// Static (constexpr) sampler state from a reflection entry -> RDNA4SampCfg (Metal enum values).
static BOOL rdna4_static_cfg(NSDictionary *ss, RDNA4SampCfg *c, NSError **err) {
    static const char *const filt[] = { "Nearest", "Linear", NULL }, *const mip[] = { "None", "Nearest", "Linear", NULL };
    static const char *const addr[] = { "ClampToEdge", "MirrorClampToEdge", "Repeat", "MirroredRepeat", "ClampToZero", "ClampToBorder", NULL };   // index = MTLSamplerAddressMode
    static const char *const cmpf[] = { "None", "Less", "Equal", "LessEqual", "Greater", "NotEqual", "GreaterEqual", "Always", "Never", NULL };
    static const char *const bord[] = { "TransparentBlack", "OpaqueBlack", "OpaqueWhite", NULL };
    static const char *const coord[] = { "Normalized", "Pixel", NULL };
    int mf = rdna4_idx(ss[@"min_filter"], filt), gf = rdna4_idx(ss[@"mag_filter"], filt), mp = rdna4_idx(ss[@"mip_filter"], mip), a1 = rdna4_idx(ss[@"address_mode_s"], addr),
        a2 = rdna4_idx(ss[@"address_mode_t"], addr), a3 = rdna4_idx(ss[@"address_mode_r"], addr), cf = rdna4_idx(ss[@"compare_function"], cmpf), bc = rdna4_idx(ss[@"border_color"], bord), co = rdna4_idx(ss[@"coordinates"], coord);
    id lo = ss[@"lod_min_clamp"], hi = ss[@"lod_max_clamp"];
    uint64_t anisotropy = 1;
    BOOL validNumbers = lo && hi && CFGetTypeID((__bridge CFTypeRef)lo) == CFNumberGetTypeID() &&
        CFGetTypeID((__bridge CFTypeRef)hi) == CFNumberGetTypeID() &&
        (!ss[@"max_anisotropy"] || (rdna4_number_u64((__bridge CFTypeRef)ss[@"max_anisotropy"], &anisotropy) && anisotropy <= UINT_MAX));
    if (mf < 0 || gf < 0 || mp < 0 || a1 < 0 || a2 < 0 || a3 < 0 || cf < 0 || bc < 0 || co < 0 || !validNumbers) {
        if (err) *err = rdna4_err(73, [NSString stringWithFormat:@"static sampler state not understood: %@", ss]); return NO; }
    *c = (RDNA4SampCfg){ .minF = mf, .magF = gf, .mipF = mp, .as = a1, .at = a2, .ar = a3, .border = bc,
        .cmp = cf == 8 ? 0 : cf, .unnorm = co == 1, .lodMin = [lo floatValue], .lodMax = [hi floatValue],
        .anisotropy = (unsigned)anisotropy };
    // JSON compare index -> MTLCompareFunction: Less 1, Equal 2, LessEqual 3, Greater 4, NotEqual 5, GreaterEqual 6, Always 7 (== table index for 1..7)
    return YES;
}

typedef struct {
    uint32_t value[RDNA4_AS_ROOTS];
    size_t count;
    uint64_t dynamicMask;
} RDNA4ArgumentScanRoots;

GARNET_INTERNAL const char *rdna4_argument_scan_status_name(rdna4_as_status status) {
    switch (status) {
    case RDNA4_AS_OK: return "ok";
    case RDNA4_AS_INVALID: return "invalid";
    case RDNA4_AS_CAPACITY: return "capacity";
    case RDNA4_AS_OVERFLOW: return "overflow";
    case RDNA4_AS_NOT_FINALIZED: return "not-finalized";
    }
    return "unknown";
}

static BOOL rdna4_argument_scan_root_add(RDNA4ArgumentScanRoots *roots,
                                          uint64_t value,
                                          NSError **error) {
    if (value >= RDNA4_AS_ROOTS) {
        if (error) *error = rdna4_err(49,
            [NSString stringWithFormat:@"argument scan root %llu is outside 0..%u",
             (unsigned long long)value, RDNA4_AS_ROOTS - 1u]);
        return NO;
    }
    for (size_t i = 0; i < roots->count; ++i)
        if (roots->value[i] == (uint32_t)value) return YES;
    roots->value[roots->count++] = (uint32_t)value;
    return YES;
}

static void rdna4_argument_scan_field_add(NSMutableData *fields,
                                           uint32_t root, uint32_t kind,
                                           uint64_t offset, uint64_t count,
                                           uint64_t stride, uint32_t known) {
    rdna4_argument_scan_reflection_field field = {
        .root = root,
        .kind = kind,
        .count_known = known,
        .writable = 1,
        .field_offset = offset,
        .count = count,
        .stride = stride ? stride : RDNA4_AS_TOKEN_BYTES,
    };
    [fields appendBytes:&field length:sizeof field];
}

static BOOL rdna4_argument_scan_optional_u64(id value, uint64_t *out) {
    return value && value != [NSNull null] &&
        rdna4_number_u64((__bridge CFTypeRef)value, out);
}

static BOOL rdna4_argument_scan_optional_stride(id descriptor, id source,
                                                uint64_t *stride) {
    uint64_t candidate = 0;
    BOOL present = NO;
    if ([descriptor isKindOfClass:[NSDictionary class]]) {
        present = rdna4_argument_scan_optional_u64(
            descriptor[@"array_stride"] ?: descriptor[@"element_stride"] ?:
                descriptor[@"stride"],
            &candidate);
    }
    if (!present && [source isKindOfClass:[NSDictionary class]])
        present = rdna4_argument_scan_optional_u64(
            source[@"array_stride"] ?: source[@"element_stride"] ?:
                source[@"record_stride"] ?: source[@"stride"],
            &candidate);
    if (!present) return YES;
    if (candidate < RDNA4_AS_TOKEN_BYTES ||
        candidate % RDNA4_AS_TOKEN_BYTES)
        return NO;
    *stride = candidate;
    return YES;
}

static BOOL rdna4_argument_scan_collect_metadata(NSDictionary *metadata,
                                                 NSMutableData *fields,
                                                 RDNA4ArgumentScanRoots *roots,
                                                 BOOL includeFields,
                                                 NSError **error) {
    if (!metadata) return YES;
    if (![metadata isKindOfClass:[NSDictionary class]]) {
        if (error) *error = rdna4_err(49, @"argument scan metadata is not an object");
        return NO;
    }
    id bindings = metadata[@"bindings"];
    if (bindings && ![bindings isKindOfClass:[NSArray class]]) {
        if (error) *error = rdna4_err(49, @"argument scan bindings are not an array");
        return NO;
    }
    id declaredRoots = metadata[@"argument_buffer_roots"];
    if (declaredRoots) {
        uint64_t version = 0, previous = 0;
        if (![declaredRoots isKindOfClass:[NSArray class]] ||
            !rdna4_number_u64((__bridge CFTypeRef)metadata[@"trimmed_from_reflection_version"], &version) ||
            version != 66) {
            if (error) *error = rdna4_err(49, @"invalid declared argument-buffer roots");
            return NO;
        }
        BOOL first = YES;
        for (id value in declaredRoots) {
            uint64_t root = 0;
            BOOL matched = NO;
            if (!rdna4_number_u64((__bridge CFTypeRef)value, &root) ||
                root >= RDNA4_AS_ROOTS || (!first && root <= previous)) {
                if (error) *error = rdna4_err(49, @"invalid declared argument-buffer root index");
                return NO;
            }
            for (id row in bindings) {
                uint64_t index = 0, param = 0, set = 0, binding = 0, count = 0;
                if (![row isKindOfClass:[NSDictionary class]] ||
                    ![row[@"descriptor"] isKindOfClass:[NSDictionary class]]) continue;
                id descriptor = row[@"descriptor"];
                matched |= [row[@"kind"] isEqual:@"Buffer"] &&
                    rdna4_number_u64((__bridge CFTypeRef)row[@"metal_index"], &index) && index == root &&
                    rdna4_number_u64((__bridge CFTypeRef)row[@"param_index"], &param) &&
                    rdna4_number_u64((__bridge CFTypeRef)descriptor[@"set"], &set) && set == 0 &&
                    rdna4_number_u64((__bridge CFTypeRef)descriptor[@"binding"], &binding) && binding == root &&
                    rdna4_number_u64((__bridge CFTypeRef)descriptor[@"count"], &count) && count == 1;
            }
            if (!matched || !rdna4_argument_scan_root_add(roots, root, error)) {
                if (error) *error = rdna4_err(49, @"declared argument-buffer root has no matching buffer");
                return NO;
            }
            first = NO;
            previous = root;
        }
    }
    for (id binding in bindings) {
        if (![binding isKindOfClass:[NSDictionary class]]) {
            if (error) *error = rdna4_err(49, @"argument scan contains a non-object binding");
            return NO;
        }
        NSString *bindingKind = binding[@"kind"];
        if (![bindingKind isKindOfClass:[NSString class]]) continue;
        uint32_t kind = RDNA4_AS_KIND_UNKNOWN;
        if ([bindingKind isEqual:@"EmbeddedArgBufferTexture"])
            kind = RDNA4_AS_KIND_TEXTURE;
        else if ([bindingKind isEqual:@"EmbeddedArgBufferSampler"])
            kind = RDNA4_AS_KIND_SAMPLER;
        else if (![bindingKind isEqual:@"EmbeddedArgBufferBuffer"])
            continue;

        id source = binding[@"embedded_source"];
        uint64_t root = 0, offset = 0;
        if (![source isKindOfClass:[NSDictionary class]] ||
            !rdna4_number_u64((__bridge CFTypeRef)source[@"buffer_index"], &root) ||
            !rdna4_argument_scan_root_add(roots, root, error)) {
            if (error) *error = rdna4_err(49,
                [NSString stringWithFormat:@"%@ has no valid argument-buffer root",
                 bindingKind]);
            return NO;
        }
        BOOL validOffset = rdna4_number_u64((__bridge CFTypeRef)source[@"field_offset"],
                                            &offset) &&
            !(offset & (RDNA4_AS_TOKEN_BYTES - 1u));
        if (!includeFields) continue;

        id descriptor = binding[@"descriptor"];
        uint64_t count = 0, stride = RDNA4_AS_TOKEN_BYTES;
        BOOL countKnown = NO;
        if (kind == RDNA4_AS_KIND_BUFFER &&
            (![descriptor isKindOfClass:[NSDictionary class]] ||
             !rdna4_number_u64((__bridge CFTypeRef)descriptor[@"count"],
                               &count))) {
            /*
             * EmbeddedArgBufferBuffer is a single BDA field even though it
             * has no opaque-resource descriptor row. Treating its absent
             * descriptor as an unknown array used to force a whole-root scan.
             */
            count = 1;
            countKnown = YES;
        } else if ([descriptor isKindOfClass:[NSDictionary class]] &&
                   rdna4_number_u64((__bridge CFTypeRef)descriptor[@"count"],
                                    &count) && count) {
            countKnown = YES;
        }
        if (!rdna4_argument_scan_optional_stride(descriptor, source, &stride))
            countKnown = NO;
        if (validOffset && countKnown)
            rdna4_argument_scan_field_add(fields, (uint32_t)root, kind, offset,
                                          count, stride, 1);
        else
            rdna4_argument_scan_field_add(fields, (uint32_t)root, kind, 0, 0,
                                          stride, 0);
    }

    /*
     * argument_buffer_fields retains resource-handle coordinates even when a
     * descriptor has no GPU-table root entry. It also covers buffer-address
     * fields that are not opaque texture/sampler tokens, so those bytes remain
     * transport evidence rather than disappearing from the declared root.
     */
    id reflectedFields = metadata[@"argument_buffer_fields"];
    if (reflectedFields && ![reflectedFields isKindOfClass:[NSArray class]]) {
        if (error) *error = rdna4_err(49,
            @"argument_buffer_fields is not an array");
        return NO;
    }
    for (id row in reflectedFields) {
        uint64_t root = 0, offset = 0;
        if (![row isKindOfClass:[NSDictionary class]] ||
            !rdna4_number_u64((__bridge CFTypeRef)row[@"buffer_index"], &root) ||
            !rdna4_argument_scan_root_add(roots, root, error)) {
            if (error) *error = rdna4_err(49,
                @"argument_buffer_fields contains an invalid root");
            return NO;
        }
        if (!includeFields) continue;
        id resourceBufferIndex = row[@"resource_buffer_index"];
        uint64_t resourceBuffer = 0;
        BOOL bufferField = resourceBufferIndex && resourceBufferIndex != [NSNull null] &&
            rdna4_number_u64((__bridge CFTypeRef)resourceBufferIndex,
                             &resourceBuffer);
        (void)resourceBuffer;
        /*
         * argument_buffer_fields also carries scalar struct members. Opaque
         * texture/sampler coordinates come from bindings above; only a BDA
         * buffer-resource field is an additional token-bearing coordinate.
         */
        if (!bufferField) continue;
        id countValue = row[@"array_count"] ?: row[@"element_count"];
        id strideValue = row[@"array_stride"] ?: row[@"element_stride"] ?:
            row[@"record_stride"] ?: row[@"stride"];
        uint64_t count = 1, stride = RDNA4_AS_TOKEN_BYTES;
        BOOL countKnown = YES;
        if (countValue &&
            (!rdna4_argument_scan_optional_u64(countValue, &count) || !count))
            countKnown = NO;
        if (strideValue &&
            (!rdna4_argument_scan_optional_u64(strideValue, &stride) ||
             stride < RDNA4_AS_TOKEN_BYTES ||
             stride % RDNA4_AS_TOKEN_BYTES))
            countKnown = NO;
        if (rdna4_number_u64((__bridge CFTypeRef)row[@"field_offset"], &offset) &&
            !(offset & (RDNA4_AS_TOKEN_BYTES - 1u)) && countKnown)
            rdna4_argument_scan_field_add(fields, (uint32_t)root,
                                          RDNA4_AS_KIND_BUFFER,
                                          offset, count, stride, 1);
        else
            rdna4_argument_scan_field_add(fields, (uint32_t)root,
                                          RDNA4_AS_KIND_UNKNOWN, 0, 0,
                                          stride, 0);
    }

    id table = metadata[@"argument_resource_tables"];
    if (table && ![table isKindOfClass:[NSDictionary class]]) {
        if (error) *error = rdna4_err(49,
            @"argument_resource_tables is not an object");
        return NO;
    }
    id tableRoots = [table isKindOfClass:[NSDictionary class]] ? table[@"roots"] : nil;
    if (tableRoots && ![tableRoots isKindOfClass:[NSArray class]]) {
        if (error) *error = rdna4_err(49,
            @"argument_resource_tables.roots is not an array");
        return NO;
    }
    for (id row in tableRoots) {
        uint64_t root = 0;
        if (![row isKindOfClass:[NSDictionary class]] ||
            !rdna4_number_u64((__bridge CFTypeRef)row[@"buffer_index"], &root) ||
            !rdna4_argument_scan_root_add(roots, root, error)) {
            if (error) *error = rdna4_err(49,
                @"argument_resource_tables contains an invalid root");
            return NO;
        }
        roots->dynamicMask |= UINT64_C(1) << (uint32_t)root;
    }
    return YES;
}

GARNET_INTERNAL BOOL rdna4_argument_scan_plan_build(rdna4_argument_scan_reflection_plan *out,
                                           NSDictionary *beforeLowering,
                                           NSDictionary *afterLowering,
                                           const char *role,
                                           NSError **error) {
    NSMutableData *fields = [NSMutableData data];
    RDNA4ArgumentScanRoots roots = {0};
    if (!rdna4_argument_scan_collect_metadata(beforeLowering, fields, &roots, YES, error) ||
        !rdna4_argument_scan_collect_metadata(afterLowering, fields, &roots, NO, error))
        return NO;
    for (uint32_t root = 0; root < RDNA4_AS_ROOTS; ++root)
        if (roots.dynamicMask & (UINT64_C(1) << root))
            rdna4_argument_scan_field_add(fields, root, RDNA4_AS_KIND_UNKNOWN,
                                          0, 0, RDNA4_AS_TOKEN_BYTES, 0);
    size_t fieldCount = fields.length / sizeof(rdna4_argument_scan_reflection_field);
    if (fieldCount * sizeof(rdna4_argument_scan_reflection_field) != fields.length) {
        if (error) *error = rdna4_err(49, @"argument scan field storage is misaligned");
        return NO;
    }
    rdna4_as_status status = rdna4_argument_scan_reflection_plan_build(
        out, fields.length ? fields.bytes : NULL, fieldCount,
        roots.count ? roots.value : NULL, roots.count);
    if (status != RDNA4_AS_OK) {
        if (error) *error = rdna4_err(49,
            [NSString stringWithFormat:@"%s argument scan plan: %s", role,
             rdna4_argument_scan_status_name(status)]);
        return NO;
    }
    size_t fallbackRoots = 0;
    for (size_t i = 0; i < out->plan.count; ++i)
        if (out->plan.spans[i].flags & RDNA4_AS_FALLBACK) ++fallbackRoots;
    if (out->plan.count || out->plan.stats.fields_seen)
        RDNA4LOG("argscan plan %s: %llu field(s), %llu element(s), %zu span(s), %llu coalesced, %llu fallback-root candidate(s), overflow %llu, capacity %llu",
                 role, (unsigned long long)out->plan.stats.fields_seen,
                 (unsigned long long)out->plan.stats.elements_seen,
                 out->plan.count,
                 (unsigned long long)out->plan.stats.spans_coalesced,
                 (unsigned long long)fallbackRoots,
                 (unsigned long long)out->plan.stats.overflow_rejections,
                 (unsigned long long)out->plan.stats.capacity_rejections);
    return YES;
}

// One descriptor binding of a stage, from SPIR-V reflection (+ the meta sidecar for static samplers).


GARNET_INTERNAL BOOL rdna4_coordinate_band_index(const RDNA4PB *source, uint32_t element,
                                        uint64_t *index) {
    if (!source || source->type != VK_DESCRIPTOR_TYPE_SAMPLER ||
        element >= source->count) return NO;
    uint32_t base;
    if (source->binding >= 160 && source->binding < 192) base = 160;
    else if (source->binding >= 736 && source->binding < 768) base = 736;
    else return NO;
    uint64_t value = (uint64_t)(source->binding - base) + element;
    if (value >= 32) return NO;
    *index = value;
    return YES;
}

GARNET_INTERNAL RDNA4CoordinateFlagResult rdna4_coordinate_flag_merge(
        uint32_t flags[32], BOOL assigned[32], uint64_t index, BOOL pixel) {
    if (index >= 32) return RDNA4CoordinateFlagRange;
    uint32_t value = pixel ? 1u : 0u;
    if (assigned[index] && flags[index] != value)
        return RDNA4CoordinateFlagConflict;
    assigned[index] = YES;
    flags[index] = value;
    return RDNA4CoordinateFlagOK;
}

@class RDNA4ArgumentWriteIntent;

















// Rewrites every `OpDecorate <id> DescriptorSet 0` of a module to `set` (see the binding-model comment above).
GARNET_INTERNAL NSData *rdna4_spv_set(NSData *spv, uint32_t set) {
    NSMutableData *m = [spv mutableCopy]; uint32_t *w = m.mutableBytes; size_t nw = m.length / 4, p = 5;
    while (p < nw) {
        uint32_t op = w[p] & 0xffff, wc = w[p] >> 16; if (!wc || p + wc > nw) break;
        if (op == 71 && wc >= 4 && w[p + 2] == 34 && w[p + 3] == 0) w[p + 3] = set;
        p += wc;
    }
    return m;
}

// A fragment module starts at set 0 like every independently translated stage;
// relocate stage-local direct sampler metadata with the same 0 -> 1 rule as the SPIR-V.
static BOOL rdna4_stage_meta_sampler_row(NSDictionary *row) {
    NSString *kind = row[@"kind"];
    if (![kind isKindOfClass:[NSString class]] ||
        !([kind isEqual:@"Sampler"] || [kind isEqual:@"StaticSampler"] ||
          [kind isEqual:@"SynthesizedReadSampler"] ||
          [kind isEqual:@"EmbeddedArgBufferSampler"])) return NO;
    id descriptor = row[@"descriptor"];
    uint64_t binding = 0;
    return [descriptor isKindOfClass:[NSDictionary class]] &&
        rdna4_number_u64((__bridge CFTypeRef)descriptor[@"binding"], &binding) &&
        binding >= 160 && binding < 192;
}

static NSDictionary *rdna4_stage_meta_relocate(NSDictionary *meta, int set) {
    if (!meta || set == 0) return meta;
    id bindings = meta[@"bindings"];
    if (![bindings isKindOfClass:[NSArray class]]) return meta;
    NSMutableDictionary *out = [meta mutableCopy];
    NSMutableArray *rows = [NSMutableArray arrayWithCapacity:[bindings count]];
    for (id row in bindings) {
        if (![row isKindOfClass:[NSDictionary class]]) {
            [rows addObject:row];
            continue;
        }
        NSMutableDictionary *relocated = [row mutableCopy];
        BOOL coordinate = [relocated[@"kind"] isEqual:@"SamplerCoordinateFlags"] ||
            [relocated[@"kind"] isEqual:@"StageResourceMetadata"] ||
            [relocated[@"kind"] isEqual:@"RTPrimDataTable"];
        BOOL sampler = rdna4_stage_meta_sampler_row(relocated);
        if (!coordinate && !sampler) {
            [rows addObject:relocated];
            continue;
        }
        for (NSString *key in @[ @"descriptor", @"comparison_descriptor" ]) {
            id descriptor = relocated[key];
            if (![descriptor isKindOfClass:[NSDictionary class]]) continue;
            uint64_t descriptorSet = 0;
            if (rdna4_number_u64((__bridge CFTypeRef)descriptor[@"set"],
                                 &descriptorSet) && descriptorSet == 0) {
                NSMutableDictionary *copy = [descriptor mutableCopy];
                copy[@"set"] = @(set);
                relocated[key] = copy;
            }
        }
        [rows addObject:relocated];
    }
    out[@"bindings"] = rows;
    return out;
}

// One stage's descriptor-set layout for set `set` from its reflection; pb/npb = the stage's binding list (malloc'd). r may be NULL (no stage).
GARNET_INTERNAL BOOL rdna4_stage_dsl(const RDNA4sRefl *r, int set, VkShaderStageFlags stage, NSDictionary *meta, VkDescriptorSetLayout *dsl, RDNA4PB **pbOut, uint32_t *npb,
                          NSMutableArray *ownedSamplers, NSError **err) {
    return rdna4_stage_dsl_ex(r, set, stage, meta, dsl, pbOut, npb, ownedSamplers, NO, NULL, err);
}
GARNET_INTERNAL BOOL rdna4_stage_dsl_ex(const RDNA4sRefl *r, int set, VkShaderStageFlags stage, NSDictionary *meta, VkDescriptorSetLayout *dsl, RDNA4PB **pbOut, uint32_t *npb,
                          NSMutableArray *ownedSamplers, BOOL allowPush, BOOL *usedPush, NSError **err) {
    *pbOut = NULL; *npb = 0; if (usedPush) *usedPush = NO;
    if (r && (r->descriptor_features & RDNA4S_IMAGE_GATHER_EXTENDED) &&
        !rdna4_device_gather_caps().supported) {
        if (err) *err = rdna4_err(49,
            @"shader requires shaderImageGatherExtended unavailable on this backend");
        return NO;
    }
    uint32_t descriptorFeatures = (RDNA4R.nonUniformSampledImages ? RDNA4S_NONUNIFORM_SAMPLED : 0u) |
        (RDNA4R.nonUniformStorageImages ? RDNA4S_NONUNIFORM_STORAGE : 0u) |
        (RDNA4R.nonUniformBuffers ? RDNA4S_NONUNIFORM_BUFFERS : 0u) |
        (RDNA4R.gpuArgumentTables ? RDNA4S_RUNTIME_ARRAY : 0u) |
        (rdna4_device_gather_caps().supported ? RDNA4S_IMAGE_GATHER_EXTENDED : 0u) |
        (RDNA4R.uniformTexelBuffers ? RDNA4S_NONUNIFORM_UNIFORM_TEXEL : 0u) |
        (RDNA4R.storageTexelBuffers ? RDNA4S_NONUNIFORM_STORAGE_TEXEL : 0u);
    if (r && (r->descriptor_features & ~descriptorFeatures)) {
        if (err) *err = rdna4_err(49, @"shader requires non-uniform descriptor indexing unavailable on this backend");
        return NO;
    }
    if (r && r->bind[2][1].used && !RDNA4R.uniformTexelBuffers) {
        if (err) *err = rdna4_err(49,
            @"shader requires uniform texel-buffer arrays unavailable on this backend");
        return NO;
    }
    if (r && r->bind[3][1].used && !RDNA4R.storageTexelBuffers) {
        if (err) *err = rdna4_err(49,
            @"shader requires storage texel-buffer arrays unavailable on this backend");
        return NO;
    }
    meta = rdna4_stage_meta_relocate(meta, set);
    NSArray<NSDictionary *> *bindingMetadata = meta[@"bindings"];
    BOOL gpuArguments = meta[@"argument_resource_tables"] != nil;
    BOOL normalizedCoordinates = NO;
    if (!rdna4_argument_normalized_metadata(meta, &normalizedCoordinates, err))
        return NO;
    if (bindingMetadata && ![bindingMetadata isKindOfClass:[NSArray class]]) {
        if (err) *err = rdna4_err(49, @"shader binding metadata must be an array");
        return NO;
    }
    BOOL coordinateFlagsMetadata = NO;
    for (NSDictionary *binding in bindingMetadata) {
        if (![binding isKindOfClass:[NSDictionary class]] || ![binding[@"kind"] isKindOfClass:[NSString class]]) {
            if (err) *err = rdna4_err(49, @"shader binding metadata contains an invalid entry");
            return NO;
        }
        NSString *kind = binding[@"kind"];
        if ([kind isEqual:@"SynthesizedReadSampler"]) {
            NSDictionary *descriptor = binding[@"descriptor"];
            uint64_t descriptorSet = 0, slot = 0, count = 0;
            BOOL valid = [descriptor isKindOfClass:[NSDictionary class]] &&
                rdna4_number_u64((__bridge CFTypeRef)descriptor[@"set"], &descriptorSet) &&
                descriptorSet == (uint64_t)set &&
                rdna4_number_u64((__bridge CFTypeRef)descriptor[@"binding"], &slot) &&
                slot >= 160 && slot < 192 &&
                rdna4_number_u64((__bridge CFTypeRef)descriptor[@"count"], &count) && count == 1 &&
                [binding[@"static_sampler"] isKindOfClass:[NSDictionary class]] &&
                !binding[@"comparison_descriptor"] && !binding[@"embedded_source"] && r &&
                r->bind[set][slot].used && r->bind[set][slot].type == VK_DESCRIPTOR_TYPE_SAMPLER &&
                r->bind[set][slot].count == 1;
            if (!valid) {
                if (err) *err = rdna4_err(49, @"synthesized read sampler state or descriptor is invalid");
                return NO;
            }
        }
        if ([kind isEqual:@"SamplerCoordinateFlags"] || [kind isEqual:@"StageResourceMetadata"]) {
            BOOL stageMetadata = [kind isEqual:@"StageResourceMetadata"];
            BOOL versionPresent = NO;
            uint64_t reflectionVersion = 0;
            if (!rdna4_argument_reflection_version(meta, &versionPresent, &reflectionVersion)) return NO;
            NSUInteger expectedBytes = stageMetadata ? 384 : 128;
            NSDictionary *descriptor = binding[@"descriptor"];
            NSDictionary *extent = binding[@"extent"];
            uint64_t metalIndex = 0, descriptorSet = 0, slot = 0, count = 0,
                declared = 0, bytes = 0;
            NSSet *bindingKeys = [NSSet setWithObjects:@"kind", @"metal_index",
                @"descriptor", @"declared_size", @"extent", @"access", nil];
            NSSet *descriptorKeys = [NSSet setWithObjects:@"set", @"binding",
                @"count", nil];
            NSSet *extentKeys = [NSSet setWithObject:@"Object"];
            NSSet *objectKeys = [NSSet setWithObject:@"bytes"];
            BOOL exact = (stageMetadata || normalizedCoordinates) && r && versionPresent &&
                (stageMetadata ? (reflectionVersion == 65 || reflectionVersion == 66) && rdna4_argument_table_uses_shape_registry(meta)
                               : !gpuArguments && reflectionVersion == 64) &&
                [descriptor isKindOfClass:[NSDictionary class]] &&
                [extent isKindOfClass:[NSDictionary class]] &&
                [extent[@"Object"] isKindOfClass:[NSDictionary class]] &&
                [[NSSet setWithArray:binding.allKeys]
                    isEqualToSet:bindingKeys] &&
                [[NSSet setWithArray:descriptor.allKeys]
                    isEqualToSet:descriptorKeys] &&
                [[NSSet setWithArray:extent.allKeys]
                    isEqualToSet:extentKeys] &&
                [[NSSet setWithArray:[extent[@"Object"] allKeys]]
                    isEqualToSet:objectKeys] &&
                [binding[@"access"] isEqual:@"ReadOnly"] &&
                rdna4_number_u64((__bridge CFTypeRef)binding[@"metal_index"], &metalIndex) &&
                metalIndex == 0 &&
                rdna4_number_u64((__bridge CFTypeRef)descriptor[@"set"], &descriptorSet) &&
                descriptorSet == (uint64_t)set &&
                rdna4_number_u64((__bridge CFTypeRef)descriptor[@"binding"], &slot) &&
                slot == 674 &&
                rdna4_number_u64((__bridge CFTypeRef)descriptor[@"count"], &count) &&
                count == 1 &&
                rdna4_number_u64((__bridge CFTypeRef)binding[@"declared_size"], &declared) &&
                declared == expectedBytes &&
                rdna4_number_u64((__bridge CFTypeRef)extent[@"Object"][@"bytes"], &bytes) &&
                bytes == expectedBytes && !binding[@"comparison_descriptor"] &&
                !binding[@"embedded_source"] && !binding[@"texture_shape"];
            if (!r || !exact || coordinateFlagsMetadata) {
                if (err) *err = rdna4_err(49,
                    @"SamplerCoordinateFlags metadata does not match binding 674 reflection");
                return NO;
            }
            const RDNA4sBind *flags = &r->bind[set][674];
            if (!flags->used ||
                flags->type != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
                flags->count != 1 || flags->runtime) {
                if (err) *err = rdna4_err(49,
                    @"SamplerCoordinateFlags metadata does not match binding 674 reflection");
                return NO;
            }
            coordinateFlagsMetadata = YES;
            continue;
        }
        id paired = binding[@"comparison_descriptor"];
        if (paired) {
            // Table samplers share one runtime array; its logical pair is checked with the table ABI below.
            if (gpuArguments && [binding[@"kind"] isEqual:@"EmbeddedArgBufferSampler"]) continue;
            id normal = binding[@"descriptor"];
            uint64_t nset = 0, cset = 0, nb = 0, cb = 0, nc = 0, cc = 0;
            BOOL sampler = [binding[@"kind"] isEqual:@"Sampler"] || [binding[@"kind"] isEqual:@"StaticSampler"] ||
                [binding[@"kind"] isEqual:@"EmbeddedArgBufferSampler"];
            if (!sampler || ![normal isKindOfClass:[NSDictionary class]] || ![paired isKindOfClass:[NSDictionary class]] ||
                !rdna4_number_u64((__bridge CFTypeRef)normal[@"set"], &nset) ||
                !rdna4_number_u64((__bridge CFTypeRef)paired[@"set"], &cset) || nset != cset || nset > 1 ||
                !rdna4_number_u64((__bridge CFTypeRef)normal[@"binding"], &nb) || nb < 160 || nb >= 192 ||
                !rdna4_number_u64((__bridge CFTypeRef)paired[@"binding"], &cb) || cb != nb + 576 ||
                !rdna4_number_u64((__bridge CFTypeRef)normal[@"count"], &nc) || !nc || nc > 192 - nb ||
                !rdna4_number_u64((__bridge CFTypeRef)paired[@"count"], &cc) || cc != nc || !r ||
                !r->bind[set][nb].used || !r->bind[set][cb].used ||
                r->bind[set][nb].type != VK_DESCRIPTOR_TYPE_SAMPLER || r->bind[set][cb].type != VK_DESCRIPTOR_TYPE_SAMPLER ||
                r->bind[set][nb].count != nc || r->bind[set][cb].count != nc) {
                if (err) *err = rdna4_err(49, @"comparison sampler metadata does not match the reflected stage");
                return NO;
            }
        }
    }
    const RDNA4sBind *coordinateFlags = r ? &r->bind[set][674] : NULL;
    BOOL reflectedCoordinateFlags = coordinateFlags && coordinateFlags->used;
    if (reflectedCoordinateFlags != coordinateFlagsMetadata) {
        if (err) *err = rdna4_err(49,
            @"binding 674 and SamplerCoordinateFlags metadata must appear together");
        return NO;
    }
    if (!rdna4_argument_table_metadata(r, set, meta, err)) return NO;
    VkDescriptorSetLayoutBinding *bnd = calloc(RDNA4S_MAXBIND, sizeof *bnd); RDNA4PB *pb = calloc(RDNA4S_MAXBIND, sizeof *pb); uint32_t nb = 0;
    if (!bnd || !pb) {
        free(bnd); free(pb);
        if (err) *err = rdna4_err(49, @"cannot allocate shader binding layout");
        return NO;
    }
    if (r) {
        if (r->maxset > set && !gpuArguments) { free(bnd); free(pb); if (err) *err = rdna4_err(49, [NSString stringWithFormat:@"stage uses descriptor set %d (> %d)", r->maxset, set]); return NO; }
        for (int b = 0; b < RDNA4S_MAXBIND; b++) {
            const RDNA4sBind *a = &r->bind[set][b]; if (!a->used) continue;
            if (a->runtime) {
                free(bnd); free(pb);
                if (err) *err = rdna4_err(49, @"runtime descriptor arrays require the GPU argument table layout");
                return NO;
            }
            bnd[nb] = (VkDescriptorSetLayoutBinding){ (uint32_t)b, a->type, a->count, stage, NULL };
            pb[nb] = (RDNA4PB){ .binding = (uint32_t)b, .type = a->type, .count = a->count };
            pb[nb].imageSampleKinds = a->imageSampleKinds;
            pb[nb].texelComponentKinds = a->texelComponentKinds;
            pb[nb].comparisonSampler = a->type == VK_DESCRIPTOR_TYPE_SAMPLER && b >= 736 && b < 768;
            pb[nb].gpuArguments = gpuArguments;
            pb[nb].argumentTable = gpuArguments && (b == 672 || b == 673);
            pb[nb].argumentRoot = gpuArguments && b >= 704 && b < 735;
            pb[nb].normalizedCoordinates = normalizedCoordinates &&
                a->type == VK_DESCRIPTOR_TYPE_SAMPLER;
            pb[nb].samplerCoordinateFlags = b == 674;
            pb[nb].rtPrimDataTable = (b == (int)RDNA4RT_PRIM_DATA_BINDING &&
                a->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER && a->count == 1 && !a->runtime);
            if (pb[nb].rtPrimDataTable && !RDNA4R.bufferAddress) {
                free(bnd); free(pb);
                if (err) *err = rdna4_err(49, @"RT primitive data table requires Vulkan buffer device address support");
                return NO;
            }
            if (b == 674 && rdna4_argument_table_uses_shape_registry(meta)) {
                pb[nb].stageResourceMetadata = YES;
                pb[nb].stageTokenTable = r->bind[set][672].used;
                NSArray *shapes = meta[@"argument_resource_tables"][@"null_shapes"];
                pb[nb].nullShapeCount = (uint16_t)shapes.count;
                for (NSUInteger i = 0; i < shapes.count; ++i)
                    pb[nb].nullShapeRemap[i] = [shapes[i] unsignedIntValue];
            }
            if (pb[nb].argumentRoot) pb[nb].embeddedBuffer = (uint32_t)b - 704;
            nb++;
        }
    }
    for (NSDictionary *bd in bindingMetadata) {
        if (gpuArguments) continue;
        if (![bd isKindOfClass:[NSDictionary class]]) continue;
        BOOL embeddedTexture = [bd[@"kind"] isEqual:@"EmbeddedArgBufferTexture"];
        if (!embeddedTexture && ![bd[@"kind"] isEqual:@"EmbeddedArgBufferSampler"]) continue;
        NSDictionary *source = bd[@"embedded_source"];
        NSDictionary *descriptor = bd[@"descriptor"];
        NSNumber *binding = [descriptor isKindOfClass:[NSDictionary class]] ? descriptor[@"binding"] : nil;
        NSNumber *count = [descriptor isKindOfClass:[NSDictionary class]] ? descriptor[@"count"] : nil;
        NSDictionary *comparisonDescriptor = bd[@"comparison_descriptor"];
        uint64_t comparisonBinding = comparisonDescriptor ? [comparisonDescriptor[@"binding"] unsignedLongLongValue] : UINT64_MAX;
        NSNumber *buffer = [source isKindOfClass:[NSDictionary class]] ? source[@"buffer_index"] : nil;
        NSNumber *offset = [source isKindOfClass:[NSDictionary class]] ? source[@"field_offset"] : nil;
        BOOL matched = NO;
        uint64_t bindingValue = 0, countValue = 0, bufferValue = 0, offsetValue = 0;
        if (rdna4_number_u64((__bridge CFTypeRef)binding, &bindingValue) && bindingValue < RDNA4S_MAXBIND &&
            rdna4_number_u64((__bridge CFTypeRef)count, &countValue) && countValue > 0 && countValue <= UINT32_MAX &&
            rdna4_number_u64((__bridge CFTypeRef)buffer, &bufferValue) && bufferValue < 31 &&
            rdna4_number_u64((__bridge CFTypeRef)offset, &offsetValue) && offsetValue <= UINT32_MAX &&
            !(offsetValue & (sizeof(uint64_t) - 1))) {
            for (uint32_t i = 0; i < nb; ++i) {
                if (pb[i].binding != bindingValue && pb[i].binding != comparisonBinding) continue;
                BOOL typeMatches = embeddedTexture
                    ? (pb[i].type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || pb[i].type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ||
                       pb[i].type == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || pb[i].type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER)
                    : pb[i].type == VK_DESCRIPTOR_TYPE_SAMPLER;
                if (!typeMatches || pb[i].count != countValue || pb[i].embedded) break;
                pb[i].embedded = YES;
                pb[i].embeddedBuffer = (uint32_t)bufferValue;
                pb[i].embeddedOffset = (uint32_t)offsetValue;
                matched = YES;
            }
        }
        if (!matched) {
            free(bnd); free(pb);
            if (err) *err = rdna4_err(49, @"embedded image/sampler metadata does not match a supported descriptor");
            return NO;
        }
    }
    for (NSDictionary *bd in bindingMetadata) {
        if (![bd[@"kind"] isEqual:@"BufferAddressTable"]) continue;
        NSDictionary *descriptor = [bd[@"descriptor"] isKindOfClass:[NSDictionary class]] ? bd[@"descriptor"] : nil;
        uint64_t binding = 0, count = 0;
        BOOL found = NO;
        if (RDNA4R.bufferAddress && rdna4_number_u64((__bridge CFTypeRef)descriptor[@"binding"], &binding) &&
            rdna4_number_u64((__bridge CFTypeRef)descriptor[@"count"], &count) && count == 1 &&
            binding >= 640 && binding < 672) {
            for (uint32_t i = 0; i < nb; ++i)
                if (pb[i].binding == binding && !pb[i].addressTable && pb[i].count == 1 &&
                    pb[i].type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
                    pb[i].addressTable = YES;
                    id version = meta[@"buffer_address_table_version"], bytes = meta[@"buffer_address_table_bytes"];
                    uint64_t versionNumber = 0, tableBytes = 0;
                    id stride = meta[@"buffer_address_table_row_stride_bytes"];
                    id dynamic = meta[@"buffer_address_table_dynamic_extent"];
                    uint64_t rowStride = 0;
                    BOOL numeric = rdna4_number_u64((__bridge CFTypeRef)version, &versionNumber) &&
                        rdna4_number_u64((__bridge CFTypeRef)bytes, &tableBytes);
                    BOOL directLayout = numeric && versionNumber == RDNA4_BUFFER_TABLE_VERSION &&
                        tableBytes == sizeof(rdna4_buffer_table) && !stride && !dynamic;
                    BOOL registryLayout = numeric && versionNumber == RDNA4_BUFFER_REGISTRY_VERSION &&
                        tableBytes == RDNA4_BUFFER_REGISTRY_MIN_BYTES &&
                        rdna4_number_u64((__bridge CFTypeRef)stride, &rowStride) &&
                        rowStride == RDNA4_BUFFER_REGISTRY_ROW_BYTES && dynamic &&
                        CFGetTypeID((__bridge CFTypeRef)dynamic) == CFBooleanGetTypeID() &&
                        [dynamic boolValue];
                    if ((version || bytes || stride || dynamic) && !directLayout && !registryLayout) {
                        free(bnd); free(pb);
                        if (err) *err = rdna4_err(49, @"unsupported buffer-address table layout");
                        return NO;
                    }
                    pb[i].addressTableVersion = version ? (uint32_t)versionNumber : 1;
                    found = YES;
                }
        }
        if (!found) {
            free(bnd); free(pb);
            if (err) *err = rdna4_err(49, @"buffer-address table metadata or Vulkan device-address support is invalid");
            return NO;
        }
    }
    for (NSDictionary *bd in bindingMetadata) {
        if (![bd[@"kind"] isEqual:@"RTPrimDataTable"]) continue;
        NSDictionary *descriptor = [bd[@"descriptor"] isKindOfClass:[NSDictionary class]] ? bd[@"descriptor"] : nil;
        uint64_t descriptorSet = (uint64_t)set, binding = 0, count = 0;
        BOOL setOK = !descriptor[@"set"] ||
            (rdna4_number_u64((__bridge CFTypeRef)descriptor[@"set"], &descriptorSet) &&
             descriptorSet == (uint64_t)set);
        BOOL found = NO;
        if (RDNA4R.bufferAddress && setOK &&
            rdna4_number_u64((__bridge CFTypeRef)descriptor[@"binding"], &binding) &&
            binding == RDNA4RT_PRIM_DATA_BINDING &&
            rdna4_number_u64((__bridge CFTypeRef)descriptor[@"count"], &count) && count == 1) {
            for (uint32_t i = 0; i < nb; ++i) {
                if (pb[i].binding == RDNA4RT_PRIM_DATA_BINDING && pb[i].count == 1 &&
                    pb[i].type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
                    pb[i].rtPrimDataTable = YES;
                    found = YES;
                    break;
                }
            }
        }
        if (!found) {
            free(bnd); free(pb);
            if (err) *err = rdna4_err(49, @"RT primitive data table metadata or Vulkan device-address support is invalid");
            return NO;
        }
    }
    for (NSDictionary *bd in bindingMetadata) {
        BOOL staticSampler = [bd[@"kind"] isEqual:@"StaticSampler"] ||
            [bd[@"kind"] isEqual:@"SynthesizedReadSampler"];
        if (!staticSampler || ![bd[@"static_sampler"] isKindOfClass:[NSDictionary class]]) continue;
        uint32_t bn = (uint32_t)[bd[@"descriptor"][@"binding"] unsignedIntValue];
        NSDictionary *comparison = bd[@"comparison_descriptor"];
        uint64_t comparisonBinding = comparison ? [comparison[@"binding"] unsignedLongLongValue] : UINT64_MAX;
        for (uint32_t i = 0; i < nb; i++) if ((pb[i].binding == bn || pb[i].binding == comparisonBinding) && pb[i].type == VK_DESCRIPTOR_TYPE_SAMPLER) {
            RDNA4SampCfg c; NSError *e = nil;
            if (!rdna4_static_cfg(bd[@"static_sampler"], &c, &e)) { free(bnd); free(pb); if (err) *err = e; return NO; }
            VkSampler s = rdna4_make_sampler(&c, pb[i].comparisonSampler,
                                             normalizedCoordinates, NULL, &e);
            if (!s) { free(bnd); free(pb); if (err) *err = e; return NO; }
            pb[i].stat = s; [ownedSamplers addObject:[NSValue valueWithPointer:(void *)s]];
            pb[i].pixelCoordinates = c.unnorm != 0;
            RDNA4LOG("static sampler at binding %u: min %d mag %d mip %d addr %d/%d/%d %s", bn, c.minF, c.magF, c.mipF, c.as, c.at, c.ar, c.unnorm ? "unnormalized (coord::pixel)" : "normalized");
        }
    }
    BOOL usePush = NO;
    if (allowPush && RDNA4DO.on && RDNA4R.pushDescExt && RDNA4R.cmdPushDescSet &&
        !RDNA4P.auditSubmit && RDNA4R.realAllocDesc && vkAllocateDescriptorSets == RDNA4R.realAllocDesc &&
        nb > 0 && nb <= 16u) {
        uint32_t totalDesc = 0u;
        BOOL okPush = YES;
        for (uint32_t i = 0; i < nb; i++) {
            const RDNA4PB *p = &pb[i];
            if (p->addressTable || p->samplerCoordinateFlags || p->argumentTable || p->rtPrimDataTable ||
                (p->type != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER && p->type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER &&
                 p->type != VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE && p->type != VK_DESCRIPTOR_TYPE_SAMPLER &&
                 p->type != VK_DESCRIPTOR_TYPE_STORAGE_IMAGE && p->type != VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT)) {
                okPush = NO; break;
            }
            totalDesc += p->count;
        }
        if (okPush && totalDesc > 0u && totalDesc <= RDNA4R.maxPushDesc && totalDesc <= 24u)
            usePush = YES;
    }
    VkDescriptorSetLayoutCreateInfo dc = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .flags = usePush ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR : 0u,
        .bindingCount = nb, .pBindings = bnd };
    VkResult vr = vkCreateDescriptorSetLayout(RDNA4R.dev, &dc, NULL, dsl);
    free(bnd);
    if (vr != VK_SUCCESS) { free(pb); if (err) *err = rdna4_err(50, [NSString stringWithFormat:@"vkCreateDescriptorSetLayout = %d", vr]); return NO; }
    if (usedPush) *usedPush = usePush;
    if (nb) { *pbOut = realloc(pb, nb * sizeof *pb); *npb = nb; } else free(pb);
    return YES;
}

// MTLVertexFormat -> VkFormat (0 = unsupported).
GARNET_INTERNAL VkFormat rdna4_vkvfmt(MTLVertexFormat f) {
    switch (f) {
    case MTLVertexFormatUChar: return VK_FORMAT_R8_UINT; case MTLVertexFormatUChar2: return VK_FORMAT_R8G8_UINT; case MTLVertexFormatUChar3: return VK_FORMAT_R8G8B8_UINT; case MTLVertexFormatUChar4: return VK_FORMAT_R8G8B8A8_UINT;
    case MTLVertexFormatChar: return VK_FORMAT_R8_SINT; case MTLVertexFormatChar2: return VK_FORMAT_R8G8_SINT; case MTLVertexFormatChar3: return VK_FORMAT_R8G8B8_SINT; case MTLVertexFormatChar4: return VK_FORMAT_R8G8B8A8_SINT;
    case MTLVertexFormatUCharNormalized: return VK_FORMAT_R8_UNORM; case MTLVertexFormatUChar2Normalized: return VK_FORMAT_R8G8_UNORM; case MTLVertexFormatUChar3Normalized: return VK_FORMAT_R8G8B8_UNORM; case MTLVertexFormatUChar4Normalized: return VK_FORMAT_R8G8B8A8_UNORM;
    case MTLVertexFormatCharNormalized: return VK_FORMAT_R8_SNORM; case MTLVertexFormatChar2Normalized: return VK_FORMAT_R8G8_SNORM; case MTLVertexFormatChar3Normalized: return VK_FORMAT_R8G8B8_SNORM; case MTLVertexFormatChar4Normalized: return VK_FORMAT_R8G8B8A8_SNORM;
    case MTLVertexFormatUChar4Normalized_BGRA: return VK_FORMAT_B8G8R8A8_UNORM;
    case MTLVertexFormatUShort: return VK_FORMAT_R16_UINT; case MTLVertexFormatUShort2: return VK_FORMAT_R16G16_UINT; case MTLVertexFormatUShort3: return VK_FORMAT_R16G16B16_UINT; case MTLVertexFormatUShort4: return VK_FORMAT_R16G16B16A16_UINT;
    case MTLVertexFormatShort: return VK_FORMAT_R16_SINT; case MTLVertexFormatShort2: return VK_FORMAT_R16G16_SINT; case MTLVertexFormatShort3: return VK_FORMAT_R16G16B16_SINT; case MTLVertexFormatShort4: return VK_FORMAT_R16G16B16A16_SINT;
    case MTLVertexFormatUShortNormalized: return VK_FORMAT_R16_UNORM; case MTLVertexFormatUShort2Normalized: return VK_FORMAT_R16G16_UNORM; case MTLVertexFormatUShort3Normalized: return VK_FORMAT_R16G16B16_UNORM; case MTLVertexFormatUShort4Normalized: return VK_FORMAT_R16G16B16A16_UNORM;
    case MTLVertexFormatShortNormalized: return VK_FORMAT_R16_SNORM; case MTLVertexFormatShort2Normalized: return VK_FORMAT_R16G16_SNORM; case MTLVertexFormatShort3Normalized: return VK_FORMAT_R16G16B16_SNORM; case MTLVertexFormatShort4Normalized: return VK_FORMAT_R16G16B16A16_SNORM;
    case MTLVertexFormatHalf: return VK_FORMAT_R16_SFLOAT; case MTLVertexFormatHalf2: return VK_FORMAT_R16G16_SFLOAT; case MTLVertexFormatHalf3: return VK_FORMAT_R16G16B16_SFLOAT; case MTLVertexFormatHalf4: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case MTLVertexFormatFloat: return VK_FORMAT_R32_SFLOAT; case MTLVertexFormatFloat2: return VK_FORMAT_R32G32_SFLOAT; case MTLVertexFormatFloat3: return VK_FORMAT_R32G32B32_SFLOAT; case MTLVertexFormatFloat4: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case MTLVertexFormatInt: return VK_FORMAT_R32_SINT; case MTLVertexFormatInt2: return VK_FORMAT_R32G32_SINT; case MTLVertexFormatInt3: return VK_FORMAT_R32G32B32_SINT; case MTLVertexFormatInt4: return VK_FORMAT_R32G32B32A32_SINT;
    case MTLVertexFormatUInt: return VK_FORMAT_R32_UINT; case MTLVertexFormatUInt2: return VK_FORMAT_R32G32_UINT; case MTLVertexFormatUInt3: return VK_FORMAT_R32G32B32_UINT; case MTLVertexFormatUInt4: return VK_FORMAT_R32G32B32A32_UINT;
    case MTLVertexFormatInt1010102Normalized: return VK_FORMAT_A2B10G10R10_SNORM_PACK32; case MTLVertexFormatUInt1010102Normalized: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    default: return VK_FORMAT_UNDEFINED;
    }
}
GARNET_INTERNAL VkBlendFactor rdna4_bf(MTLBlendFactor f, BOOL *bad) {
    switch (f) {
    case MTLBlendFactorZero: return VK_BLEND_FACTOR_ZERO; case MTLBlendFactorOne: return VK_BLEND_FACTOR_ONE;
    case MTLBlendFactorSourceColor: return VK_BLEND_FACTOR_SRC_COLOR; case MTLBlendFactorOneMinusSourceColor: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case MTLBlendFactorSourceAlpha: return VK_BLEND_FACTOR_SRC_ALPHA; case MTLBlendFactorOneMinusSourceAlpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case MTLBlendFactorDestinationColor: return VK_BLEND_FACTOR_DST_COLOR; case MTLBlendFactorOneMinusDestinationColor: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case MTLBlendFactorDestinationAlpha: return VK_BLEND_FACTOR_DST_ALPHA; case MTLBlendFactorOneMinusDestinationAlpha: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case MTLBlendFactorSourceAlphaSaturated: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case MTLBlendFactorBlendColor: return VK_BLEND_FACTOR_CONSTANT_COLOR; case MTLBlendFactorOneMinusBlendColor: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case MTLBlendFactorBlendAlpha: return VK_BLEND_FACTOR_CONSTANT_ALPHA; case MTLBlendFactorOneMinusBlendAlpha: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    case MTLBlendFactorSource1Color: if (!RDNA4R.dualSrcBlend) *bad = YES; return VK_BLEND_FACTOR_SRC1_COLOR;
    case MTLBlendFactorOneMinusSource1Color: if (!RDNA4R.dualSrcBlend) *bad = YES; return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
    case MTLBlendFactorSource1Alpha: if (!RDNA4R.dualSrcBlend) *bad = YES; return VK_BLEND_FACTOR_SRC1_ALPHA;
    case MTLBlendFactorOneMinusSource1Alpha: if (!RDNA4R.dualSrcBlend) *bad = YES; return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    default: *bad = YES; return VK_BLEND_FACTOR_ZERO;
    }
}
GARNET_INTERNAL VkBlendOp rdna4_bo(MTLBlendOperation o, BOOL *bad) {
    switch (o) { case MTLBlendOperationAdd: return VK_BLEND_OP_ADD; case MTLBlendOperationSubtract: return VK_BLEND_OP_SUBTRACT;
        case MTLBlendOperationReverseSubtract: return VK_BLEND_OP_REVERSE_SUBTRACT; case MTLBlendOperationMin: return VK_BLEND_OP_MIN;
        case MTLBlendOperationMax: return VK_BLEND_OP_MAX; default: *bad = YES; return VK_BLEND_OP_ADD; }
}

// ---------------------------------------------------------------------------------------------------------------
// Pipeline HOT-SWAP (native #12): a FALLBACK pipeline object (spvcache miss in WindowServer; render/compute = no-write placeholder) registers
// its spvcache keys here. One watcher per process (a dispatch timer on a private serial queue that exists only while an entry is pending)
// stats <sha>.spv / <sha>.meta.json in the lookup directories; when the state machine (garnet_hotswap.h) says the files are complete and stable it
// calls the object's builder on the watcher queue (NEVER the render thread), which builds a NEW pipeline object through the normal init path and
// publishes it with one atomic store into the fallback object (rdna4hs_install). Encoders resolve the pointer at the start of every draw / dispatch.
// ---------------------------------------------------------------------------------------------------------------
@interface RDNA4HSEntry : NSObject { @public __weak id obj; rdna4hs_entry st; NSArray<NSString *> *shas; NSString *name; BOOL (^build)(id); } @end
@implementation RDNA4HSEntry @end
static NSMutableArray<RDNA4HSEntry *> *rdna4hs_reg; static NSLock *rdna4hs_lock; static dispatch_queue_t rdna4hs_q; static dispatch_source_t rdna4hs_timer;

GARNET_INTERNAL NSString *rdna4_fn_sha(id fn) {   // sha256(bitcodeData) of a function, nil when it has none; build 18 (P3): the linked key when the function is a linked stage of the pipeline being created
    RDNA4LinkCtx *lk_ = rdna4_lk_for(fn); if (lk_) return lk_->lsha;
    SEL sel = NSSelectorFromString(@"bitcodeData");
    NSData *bc = (fn && [fn respondsToSelector:sel]) ? ((NSData *(*)(id, SEL))objc_msgSend)(fn, sel) : nil;
    return bc.length ? rdna4_sha256hex(bc) : nil;
}
static int64_t rdna4hs_wall_ns(void) { struct timespec t; clock_gettime(CLOCK_REALTIME, &t); return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec; }
// First lookup directory holding a usable file (same order and sanity as the creation-time lookup).
static rdna4hs_obs rdna4hs_stat_path(NSString *path) {
    struct stat fileStat;
    if (rdna4_sb_stat(path.fileSystemRepresentation, &fileStat) || !S_ISREG(fileStat.st_mode) || fileStat.st_size <= 0)
        return (rdna4hs_obs){0};
    return (rdna4hs_obs){1, (uint64_t)fileStat.st_size,
        (int64_t)fileStat.st_mtimespec.tv_sec * 1000000000LL + fileStat.st_mtimespec.tv_nsec};
}
static rdna4hs_key_obs rdna4hs_stat_pair(NSString *sha, BOOL needMeta) {
    for (NSString *dir in rdna4_spv_dirs()) {
        rdna4hs_obs selected = rdna4hs_stat_path([dir stringByAppendingPathComponent:[sha stringByAppendingString:@".spv"]]);
        if (!rdna4hs_spv_ok(&selected)) continue;
        rdna4hs_key_obs pair = { .spv = selected };
        if (needMeta) pair.meta = rdna4hs_stat_path([dir stringByAppendingPathComponent:[sha stringByAppendingString:@".meta.json"]]);
        return pair;
    }
    return (rdna4hs_key_obs){0};
}
static void rdna4hs_drop(RDNA4HSEntry *e) {
    [rdna4hs_lock lock]; [rdna4hs_reg removeObjectIdenticalTo:e];
    if (!rdna4hs_reg.count && rdna4hs_timer) { dispatch_source_cancel(rdna4hs_timer); rdna4hs_timer = nil; }
    [rdna4hs_lock unlock];
}
static void rdna4hs_tick_all(void) {
    @autoreleasepool {
        [rdna4hs_lock lock]; NSArray<RDNA4HSEntry *> *snap = [rdna4hs_reg copy]; [rdna4hs_lock unlock];
        for (RDNA4HSEntry *e in snap) {
            id o = e->obj;
            if (!o || e->st.st == RDNA4HS_SWAPPED || e->st.st == RDNA4HS_GAVEUP) { rdna4hs_drop(e); continue; }
            rdna4hs_key_obs cur[RDNA4HS_MAXKEY] = {{{0},{0}}};
            for (int k = 0; k < e->st.nkeys; k++) cur[k] = rdna4hs_stat_pair(e->shas[(NSUInteger)k], YES);
            if (rdna4hs_tick(&e->st, rdna4hs_wall_ns(), cur) != RDNA4HS_BUILD) continue;
            RDNA4LOG("HOT-SWAP %s: spvcache complete and stable (attempt %d), building the real pipeline off the render thread", e->name.UTF8String, e->st.attempts);
            uint64_t t0 = rdna4_now(); BOOL ok = e->build(o); double ms = (double)(rdna4_now() - t0) / 1e6;
            if (rdna4hs_build_done(&e->st, ok, rdna4hs_wall_ns())) {
                rdna4_sync_note_async_success();
                RDNA4LOG("HOT-SWAP %s: fallback -> real (built in %.3f ms)", e->name.UTF8String, ms);
                rdna4hs_drop(e);
            }
            else if (e->st.st == RDNA4HS_GAVEUP) { RDNA4LOG_ERR("HOT-SWAP %s: GAVE UP after %d failed builds (the fallback stays)", e->name.UTF8String, e->st.attempts); rdna4hs_drop(e); }
            else RDNA4LOG_WARN("HOT-SWAP %s: build failed (attempt %d), will retry", e->name.UTF8String, e->st.attempts);
        }
    }
}
// shas: 2 (vertex, fragment) or 1 (kernel). obj is held weakly; a deallocated object drops out at the next tick.
GARNET_INTERNAL void rdna4hs_register(id obj, NSArray<NSString *> *shas, BOOL metaRequired, NSString *name, BOOL (^build)(id)) {
    if (rdna4_force_fallback() || !shas.count || shas.count > RDNA4HS_MAXKEY) return;   // forced misses (r1 test) must stay fallbacks
    static dispatch_once_t once;
    dispatch_once(&once, ^{ rdna4hs_lock = [NSLock new]; rdna4hs_reg = [NSMutableArray array]; rdna4hs_q = dispatch_queue_create("rdna4.hotswap", DISPATCH_QUEUE_SERIAL); });
    RDNA4HSEntry *e = [RDNA4HSEntry new]; e->obj = obj; e->shas = [shas copy]; e->name = [name copy]; e->build = [build copy];
    rdna4hs_init(&e->st, (int)shas.count, metaRequired ? 1 : 0);
    [rdna4hs_lock lock];
    [rdna4hs_reg addObject:e];
    if (!rdna4hs_timer) {
        rdna4hs_timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, rdna4hs_q);
        dispatch_source_set_timer(rdna4hs_timer, dispatch_time(DISPATCH_TIME_NOW, RDNA4HS_POLL_NS), RDNA4HS_POLL_NS, RDNA4HS_POLL_NS / 5);
        dispatch_source_set_event_handler(rdna4hs_timer, ^{ rdna4hs_tick_all(); });
        dispatch_resume(rdna4hs_timer);
    }
    [rdna4hs_lock unlock];
    RDNA4LOG("HOT-SWAP %s: registered (%lu key(s)%s)", name.UTF8String, (unsigned long)shas.count, metaRequired ? ", meta required" : "");
}

// Subscribe before checking files so atomic publication cannot be missed. WS gets
// only 250 ms, apps 3 s, then temporary placeholders; three timeouts skip waiting.
GARNET_INTERNAL BOOL rdna4_sync_wait(const char *what, NSArray<NSString *> *shas, BOOL needMeta) {
    if (!rdna4g_syncwait_applies(rdna4_is_ws(), atomic_load(&rdna4_app_admitted), rdna4_force_fallback(), rdna4_test_fb_as_ws()) || !shas.count) return NO;
    if (!rdna4_sync_begin(NO)) return NO;
    const uint64_t t0 = rdna4_now(), deadline = t0 + (uint64_t)rdna4g_sync_wait_ms(rdna4_is_ws()) * 1000000ull;
    const int queue = kqueue();
    if (queue < 0) { rdna4_sync_end(NO, NO); return NO; }
    NSMutableArray<NSNumber *> *fds = [NSMutableArray array];
    for (NSString *dir in rdna4_spv_dirs()) {
        if (!rdna4_sb_can_stat(dir.fileSystemRepresentation)) continue;
        int fd = open(dir.fileSystemRepresentation, O_EVTONLY | O_CLOEXEC);
        if (fd < 0) continue;
        struct kevent change;
        EV_SET(&change, fd, EVFILT_VNODE, EV_ADD | EV_CLEAR, NOTE_WRITE | NOTE_RENAME | NOTE_DELETE, 0, NULL);
        if (kevent(queue, &change, 1, NULL, 0, NULL) != 0) { close(fd); continue; }
        [fds addObject:@(fd)];
    }
    BOOL found = NO;
    for (;;) {
        BOOL all = YES;
        for (NSString *sha in shas) {
            rdna4hs_key_obs pair = rdna4hs_stat_pair(sha, needMeta);
            if (!pair.spv.present || (needMeta && !pair.meta.present)) { all = NO; break; }
        }
        found = all;
        uint64_t now = rdna4_now();
        if (found || !fds.count || now >= deadline) break;
        uint64_t remaining = deadline - now;
        struct timespec timeout = { (time_t)(remaining / NSEC_PER_SEC), (long)(remaining % NSEC_PER_SEC) };
        struct kevent event;
        int result = kevent(queue, NULL, 0, &event, 1, &timeout);
        if (result < 0 && errno == EINTR) continue;
        if (result <= 0 || (event.flags & EV_ERROR) || (event.fflags & (NOTE_RENAME | NOTE_DELETE))) break;
    }
    for (NSNumber *fd in fds) close(fd.intValue);
    close(queue);
    rdna4_sync_end(found, !found && rdna4_now() >= deadline);
    const double ms = (double)(rdna4_now() - t0) / 1e6;
    RDNA4LOG("SYNC-WAIT %s: translation %s after %.0f ms", what, found ? "ready" : "unavailable", ms);
    return found;
}

