#ifndef RDNA4_ARGUMENT_INC
#define RDNA4_ARGUMENT_INC

#include "rdna4_argument_epochs.h"

@protocol RDNA4ICBInternal;
@class RDNA4RenderPipelineState;
@class RDNA4ComputePipelineState;

@interface RDNA4ICBArgOwners : NSObject
@property NSMutableSet *icbs;
@property NSMutableSet *pipelines;
@property NSMutableSet *nestedBuffers;
@end
@implementation RDNA4ICBArgOwners
- (instancetype)init {
    if ((self = [super init])) {
        _icbs = [NSMutableSet set];
        _pipelines = [NSMutableSet set];
        _nestedBuffers = [NSMutableSet set];
    }
    return self;
}
@end

static char rdna4_icb_arg_owners_key;

static void rdna4_icb_arg_note(RDNA4Buffer *dst, id icb, id pso, RDNA4Buffer *nestedBuf) {
    if (!dst || (!icb && !pso && !nestedBuf)) return;
    @synchronized (dst) {
        RDNA4ICBArgOwners *owners = objc_getAssociatedObject(dst, &rdna4_icb_arg_owners_key);
        if (!owners) {
            owners = [RDNA4ICBArgOwners new];
            objc_setAssociatedObject(dst, &rdna4_icb_arg_owners_key, owners, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        }
        if (icb) [owners.icbs addObject:icb];
        if (pso) [owners.pipelines addObject:pso];
        if (nestedBuf && nestedBuf != dst) [owners.nestedBuffers addObject:nestedBuf];
    }
}

static void rdna4_argbuf_icb_snapshot_walk(RDNA4Buffer *buf, NSMutableSet *icbs, NSMutableSet *psos, NSMutableSet *visited) {
    if (!buf || [visited containsObject:buf]) return;
    [visited addObject:buf];
    NSSet *localIcbs = nil, *localPsos = nil, *localNested = nil;
    @synchronized (buf) {
        RDNA4ICBArgOwners *owners = objc_getAssociatedObject(buf, &rdna4_icb_arg_owners_key);
        if (owners) {
            localIcbs = [owners.icbs copy];
            localPsos = [owners.pipelines copy];
            localNested = [owners.nestedBuffers copy];
        }
    }
    if (localIcbs.count) [icbs unionSet:localIcbs];
    if (localPsos.count) [psos unionSet:localPsos];
    for (RDNA4Buffer *nb in localNested)
        rdna4_argbuf_icb_snapshot_walk(nb, icbs, psos, visited);
}

static void rdna4_argbuf_icb_snapshot(RDNA4Buffer *buf, NSMutableSet *icbs, NSMutableSet *psos) {
    if (!buf) return;
    NSMutableSet *visited = [NSMutableSet set];
    rdna4_argbuf_icb_snapshot_walk(buf, icbs, psos, visited);
}

#define RDNA4_REFLECTION_STITCHING 0x20000000u

// Metal's metadata completion ignores status, so errors need readable data too.
static dispatch_data_t rdna4_reflection_error_data(void) {
    static dispatch_data_t reply;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        uint8_t *bytes = NULL;
        size_t length = 0;
        if (!rdna4_stitch_container(NULL, 0, &bytes, &length))
            [NSException raise:NSMallocException format:@"Cannot allocate empty reflection container"];
        reply = dispatch_data_create(bytes, length, NULL, DISPATCH_DATA_DESTRUCTOR_FREE);
        if (!reply) {
            free(bytes);
            [NSException raise:NSMallocException format:@"Cannot allocate reflection dispatch data"];
        }
    });
    return reply;
}

@interface RDNA4ReflectionCompiler : NSObject
@end
@implementation RDNA4ReflectionCompiler
- (NSUInteger)compilerFlags { return 0; }
- (void)reflectionWithFunction:(id)function options:(NSUInteger)options sync:(BOOL)sync binaryArchives:(id)archives
             completionHandler:(void (^)(NSUInteger, dispatch_data_t, NSString *))completion {
    (void)sync; (void)archives;
    if (!completion) return;
    NSData *data = rdna4_function_reflection_data(function);
    BOOL visible = [function respondsToSelector:@selector(functionType)] &&
        [(id<MTLFunction>)function functionType] == MTLFunctionTypeVisible;
    if (data && ((options & RDNA4_REFLECTION_STITCHING) || visible)) {
        uint8_t *bytes = NULL;
        size_t length = 0;
        if (rdna4_stitch_reflection(data.bytes, data.length, NULL, &bytes, &length)) {
            dispatch_data_t result = dispatch_data_create(bytes, length, NULL, DISPATCH_DATA_DESTRUCTOR_FREE);
            if (!result) free(bytes);
            completion(result ? 0 : 1, result ?: rdna4_reflection_error_data(),
                       result ? nil : @"Failed to create MTLPSBIN stitching reflection data");
            return;
        }
        // The stitching callback ignores status; it still needs a valid container.
        completion(1, rdna4_reflection_error_data(), @"Failed to convert AIR reflection data to MTLPSBIN stitching reflection");
        return;
    }
    if (!data) {
        completion(1, rdna4_reflection_error_data(), @"Function has no AIR reflection data");
        return;
    }
    dispatch_data_t result = dispatch_data_create(data.bytes, data.length, NULL, ^{ (void)data; });
    completion(result ? 0 : 1, result ?: rdna4_reflection_error_data(),
               result ? nil : @"Failed to create AIR reflection data");
}
- (void)reflectionWithFunction:(id)function options:(NSUInteger)options sync:(BOOL)sync pipelineLibrary:(id)library
             completionHandler:(void (^)(NSUInteger, dispatch_data_t, NSString *))completion {
    [self reflectionWithFunction:function options:options sync:sync binaryArchives:library completionHandler:completion];
}
- (void)reflectionWithFunction:(id)function options:(NSUInteger)options sync:(BOOL)sync
             completionHandler:(void (^)(NSUInteger, dispatch_data_t, NSString *))completion {
    [self reflectionWithFunction:function options:options sync:sync binaryArchives:nil completionHandler:completion];
}
@end

@class RDNA4ArgumentLayout;
@interface RDNA4ArgumentField : NSObject
@property NSUInteger offset, size;
@property MTLDataType type;
@property RDNA4ArgumentLayout *nested;
@end
@implementation RDNA4ArgumentField
@end

static BOOL rdna4_argument_scalar(MTLDataType type, NSUInteger *size, NSUInteger *alignment) {
    const struct { MTLDataType first; NSUInteger bytes; } vectors[] = {
        { MTLDataTypeFloat, 4 }, { MTLDataTypeHalf, 2 }, { MTLDataTypeInt, 4 }, { MTLDataTypeUInt, 4 },
        { MTLDataTypeShort, 2 }, { MTLDataTypeUShort, 2 }, { MTLDataTypeChar, 1 },
        { MTLDataTypeUChar, 1 }, { MTLDataTypeBool, 1 }, { MTLDataTypeLong, 8 }, { MTLDataTypeULong, 8 }
    };
    for (unsigned i = 0; i < sizeof vectors / sizeof vectors[0]; ++i) {
        if (type < vectors[i].first || type > vectors[i].first + 3) continue;
        NSUInteger count = type - vectors[i].first + 1;
        *size = *alignment = vectors[i].bytes * (count == 3 ? 4 : count);
        return YES;
    }
    MTLDataType first = type >= MTLDataTypeHalf2x2 && type <= MTLDataTypeHalf4x4 ? MTLDataTypeHalf2x2 : MTLDataTypeFloat2x2;
    if (type >= first && type <= first + 8) {
        NSUInteger columns = (type - first) / 3 + 2, rows = (type - first) % 3 + 2;
        *alignment = (first == MTLDataTypeHalf2x2 ? 2 : 4) * (rows == 3 ? 4 : rows);
        *size = columns * *alignment;
        return YES;
    }
    if (type == MTLDataTypePointer || type == MTLDataTypeTexture || type == MTLDataTypeSampler ||
        type == MTLDataTypeRenderPipeline || type == MTLDataTypeComputePipeline ||
        type == MTLDataTypeIndirectCommandBuffer) {
        *size = *alignment = sizeof(uint64_t); return YES;
    }
    return NO;
}

@interface RDNA4ArgumentLayout : NSObject {
    NSDictionary<NSNumber *, RDNA4ArgumentField *> *_fields;
    NSUInteger _length, _alignment;
    id _device;
}
- (instancetype)initWithType:(MTLStructType *)type device:(id)device depth:(unsigned)depth;
- (instancetype)initWithArguments:(NSArray<MTLArgumentDescriptor *> *)arguments device:(id)device;
- (RDNA4ArgumentField *)field:(NSUInteger)index;
- (NSDictionary<NSNumber *, RDNA4ArgumentField *> *)fields;
- (NSUInteger)encodedLength;
- (NSUInteger)alignment;
@end

static BOOL rdna4_argument_add(NSMutableDictionary *fields, MTLDataType type, MTLArrayType *array,
                             MTLStructType *structure, MTLPointerType *pointer, NSUInteger offset, NSUInteger index,
                             id device, unsigned depth, NSUInteger *size, NSUInteger *alignment) {
    if (depth > 16 || offset > (64u << 20) || index == NSUIntegerMax || fields.count >= 4096) return NO;
    if (type == MTLDataTypeArray) {
        if (!array || !array.arrayLength || array.arrayLength > 4096 || !array.stride ||
            !array.argumentIndexStride || array.arrayLength > ((64u << 20) - offset) / array.stride) return NO;
        NSUInteger elementSize = 0, elementAlignment = 1;
        for (NSUInteger n = 0; n < array.arrayLength; ++n) {
            if (n > (NSUIntegerMax - index) / array.argumentIndexStride ||
                !rdna4_argument_add(fields, array.elementType, array.elementArrayType, array.elementStructType, array.elementPointerType,
                    offset + n * array.stride, index + n * array.argumentIndexStride, device, depth + 1,
                    &elementSize, &elementAlignment) || elementSize > array.stride) return NO;
        }
        *size = array.arrayLength * array.stride; *alignment = elementAlignment;
        return YES;
    }
    RDNA4ArgumentLayout *nested = nil;
    if (type == MTLDataTypeStruct) {
        nested = [[RDNA4ArgumentLayout alloc] initWithType:structure device:device depth:depth + 1];
        if (!nested) return NO;
        *size = nested.encodedLength; *alignment = nested.alignment;
        if (*size > (64u << 20) - offset) return NO;
        for (NSNumber *key in nested.fields) {
            if (key.unsignedIntegerValue >= NSUIntegerMax - index || fields.count >= 4096) return NO;
            NSNumber *identifier = @(index + key.unsignedIntegerValue);
            if (fields[identifier]) return NO;
            RDNA4ArgumentField *member = nested.fields[key], *field = [RDNA4ArgumentField new];
            field.offset = offset + member.offset; field.size = member.size;
            field.type = member.type; field.nested = member.nested;
            fields[identifier] = field;
        }
        return YES;
    } else if (!rdna4_argument_scalar(type, size, alignment)) return NO;
    if (type == MTLDataTypePointer && pointer.elementIsArgumentBuffer) {
        if (pointer.elementType != MTLDataTypeStruct) return NO;
        nested = [[RDNA4ArgumentLayout alloc] initWithType:pointer.elementStructType device:device depth:depth + 1];
        if (!nested || nested.encodedLength != pointer.dataSize) return NO;
    }
    if (*size > (64u << 20) - offset || fields[@(index)]) return NO;
    RDNA4ArgumentField *field = [RDNA4ArgumentField new];
    field.offset = offset; field.size = *size; field.type = type; field.nested = nested;
    fields[@(index)] = field;
    return YES;
}

@implementation RDNA4ArgumentLayout
- (instancetype)initWithArguments:(NSArray<MTLArgumentDescriptor *> *)arguments device:(id)device {
    if (!(self = [super init]) || !arguments.count || arguments.count > 4096) return nil;
    NSMutableDictionary *fields = [NSMutableDictionary dictionary];
    NSUInteger length = 0, alignment = 1;
    NSArray *ordered = [arguments sortedArrayUsingComparator:^NSComparisonResult(MTLArgumentDescriptor *a, MTLArgumentDescriptor *b) {
        return a.index < b.index ? NSOrderedAscending : a.index > b.index ? NSOrderedDescending : NSOrderedSame;
    }];
    for (MTLArgumentDescriptor *argument in ordered) {
        NSUInteger size = 0, align = 0;
        if (!rdna4_argument_scalar(argument.dataType, &size, &align) || !argument.arrayLength ||
            argument.arrayLength > 4096 || argument.arrayLength > NSUIntegerMax - argument.index) return nil;
        length = (length + align - 1) & ~(align - 1); alignment = MAX(alignment, align);
        for (NSUInteger n = 0; n < argument.arrayLength; ++n) {
            if (!rdna4_argument_add(fields, argument.dataType, nil, nil, nil, length, argument.index + n,
                                  device, 0, &size, &align)) return nil;
            length += size;
        }
    }
    _fields = [fields copy]; _alignment = alignment;
    _length = (length + alignment - 1) & ~(alignment - 1); _device = device;
    return self;
}
- (instancetype)initWithType:(MTLStructType *)type device:(id)device depth:(unsigned)depth {
    if (!(self = [super init]) || !type || depth > 16 || !type.members.count) return nil;
    NSMutableDictionary *fields = [NSMutableDictionary dictionary];
    NSUInteger length = 0, alignment = 1;
    for (MTLStructMember *member in type.members) {
        NSUInteger size = 0, align = 0;
        if (!rdna4_argument_add(fields, member.dataType, member.arrayType, member.structType, member.pointerType, member.offset,
                              member.argumentIndex, device, depth, &size, &align)) return nil;
        length = MAX(length, member.offset + size); alignment = MAX(alignment, align);
    }
    _fields = [fields copy]; _alignment = alignment;
    _length = (length + alignment - 1) & ~(alignment - 1); _device = device;
    return self;
}
- (RDNA4ArgumentField *)field:(NSUInteger)index { return _fields[@(index)]; }
- (NSDictionary<NSNumber *, RDNA4ArgumentField *> *)fields { return _fields; }
- (NSUInteger)encodedLength { return _length; }
- (NSUInteger)alignment { return _alignment; }
- (id)device { return _device; }
- (BOOL)bufferLayoutMatchesFrontEndLayout { return YES; }
// The AIR reader has already assigned the packed offsets used by the translator.
- (void)setStructType:(MTLStructType *)type withDevice:(id)device { (void)type; (void)device; }
- (NSUInteger)constantOffsetAtIndex:(NSUInteger)index {
    RDNA4ArgumentField *field = [self field:index];
    return field && field.type != MTLDataTypePointer && field.type != MTLDataTypeTexture &&
        field.type != MTLDataTypeSampler && field.type != MTLDataTypeRenderPipeline &&
        field.type != MTLDataTypeComputePipeline &&
        field.type != MTLDataTypeIndirectCommandBuffer ? field.offset : NSUIntegerMax;
}
- (id)bufferLayoutForResourceAtIndex:(NSUInteger)index { return [self field:index].nested; }
@end

@interface RDNA4ArgumentEncoder : NSObject {
    RDNA4ArgumentLayout *_layout;
    id _device;
    RDNA4Buffer *_buffer;
    NSUInteger _offset;
    NSString *_label;
}
- (instancetype)initWithLayout:(RDNA4ArgumentLayout *)layout device:(id)device;
- (RDNA4ArgumentLayout *)rdna4Layout;
- (void)rdna4ValidateRange:(NSRange)range type:(MTLDataType)type;
@end

@interface RDNA4ArgumentSamplerAlias : NSObject
@property (nonatomic, weak) id object;
@property (nonatomic, weak) id device;
@end
@implementation RDNA4ArgumentSamplerAlias
@end

@interface RDNA4ArgumentResourceToken : NSObject
@property (nonatomic, weak) id object;     // texture; sampler wrappers live in aliases
@property (nonatomic, weak) id identity;   // object or the sampler storage that owns the token
@property (nonatomic, weak) id device;
@property BOOL texture;
@property NSMutableArray<RDNA4ArgumentSamplerAlias *> *aliases;
@end
@implementation RDNA4ArgumentResourceToken
- (instancetype)init {
    if ((self = [super init])) _aliases = [NSMutableArray array];
    return self;
}
@end

static NSMutableDictionary<NSNumber *, RDNA4ArgumentResourceToken *> *rdna4_argument_tokens;
static NSLock *rdna4_argument_tokens_lock;
static _Atomic uint64_t rdna4_argument_token_id;
static char rdna4_argument_token_key;
static void rdna4_argument_tokens_init(void) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{ rdna4_argument_tokens = [NSMutableDictionary dictionary]; rdna4_argument_tokens_lock = [NSLock new]; });
}

static id rdna4_argument_token_identity(id object, BOOL texture) {
    if (!object || texture) return object;
    SEL selector = NSSelectorFromString(@"rdna4ArgumentIdentity");
    if (![object respondsToSelector:selector]) return object;
    return ((id (*)(id, SEL))objc_msgSend)(object, selector);
}

static void rdna4_argument_token_prune_aliases_locked(RDNA4ArgumentResourceToken *entry) {
    for (RDNA4ArgumentSamplerAlias *alias in [entry.aliases copy])
        if (!alias.object) [entry.aliases removeObject:alias];
}

static id rdna4_argument_token_sampler_alias_locked(RDNA4ArgumentResourceToken *entry,
                                                     id device, NSSet *scope) {
    rdna4_argument_token_prune_aliases_locked(entry);
    for (RDNA4ArgumentSamplerAlias *alias in entry.aliases) {
        id object = alias.object;
        if (!object || alias.device != device) continue;
        if (scope && ![scope containsObject:object]) continue;
        return object;
    }
    return nil;
}

static void rdna4_argument_token_prune_locked(void) {
    for (NSNumber *key in rdna4_argument_tokens.allKeys) {
        RDNA4ArgumentResourceToken *entry = rdna4_argument_tokens[key];
        if (entry.texture) {
            if (!entry.object) [rdna4_argument_tokens removeObjectForKey:key];
        } else {
            rdna4_argument_token_prune_aliases_locked(entry);
            if (!entry.aliases.count) [rdna4_argument_tokens removeObjectForKey:key];
        }
    }
}

static uint64_t rdna4_argument_resource_token(id object, id device, BOOL texture) {
    if (!object) return 0;
    id identity = rdna4_argument_token_identity(object, texture);
    if (!identity) return 0;
    rdna4_argument_tokens_init();
    [rdna4_argument_tokens_lock lock];
    NSNumber *number = objc_getAssociatedObject(identity, &rdna4_argument_token_key);
    uint64_t token = number.unsignedLongLongValue;
    if (!token) {
        uint64_t identifier = atomic_fetch_add(&rdna4_argument_token_id, 1) + 1;
        if (!identifier || identifier > 0x0000ffffffffffffull) { [rdna4_argument_tokens_lock unlock]; return 0; }
        token = 0x5244000000000000ull | identifier;
        number = @(token);
        objc_setAssociatedObject(identity, &rdna4_argument_token_key, number, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    }
    RDNA4ArgumentResourceToken *entry = rdna4_argument_tokens[@(token)];
    if (entry && entry.texture != texture) { [rdna4_argument_tokens_lock unlock]; return 0; }
    if (!entry) {
        entry = [RDNA4ArgumentResourceToken new];
        entry.texture = texture;
        rdna4_argument_tokens[@(token)] = entry;
    }
    entry.identity = identity;
    entry.device = device;
    if (texture) {
        entry.object = object;
    } else {
        entry.object = nil;
        rdna4_argument_token_prune_aliases_locked(entry);
        BOOL found = NO;
        for (RDNA4ArgumentSamplerAlias *alias in entry.aliases)
            if (alias.object == object) {
                alias.device = device;
                found = YES;
                break;
            }
        if (!found) {
            RDNA4ArgumentSamplerAlias *alias = [RDNA4ArgumentSamplerAlias new];
            alias.object = object;
            alias.device = device;
            [entry.aliases addObject:alias];
        }
    }
    if ((token & 255u) == 0) {
        rdna4_argument_token_prune_locked();
    }
    [rdna4_argument_tokens_lock unlock];
    return token;
}
static id rdna4_argument_resource_for_token(uint64_t token, BOOL texture, id device) {
    if (!token) return nil;
    rdna4_argument_tokens_init();
    [rdna4_argument_tokens_lock lock];
    RDNA4ArgumentResourceToken *entry = rdna4_argument_tokens[@(token)];
    id object = nil;
    if (entry && entry.texture == texture) {
        if (texture) {
            if (entry.device == device) object = entry.object;
            if (!entry.object) [rdna4_argument_tokens removeObjectForKey:@(token)];
        } else {
            object = rdna4_argument_token_sampler_alias_locked(entry, device, nil);
            if (!entry.aliases.count) [rdna4_argument_tokens removeObjectForKey:@(token)];
        }
    }
    [rdna4_argument_tokens_lock unlock];
    return object;
}

static NSArray *rdna4_argument_resource_snapshot(id device, NSSet *declared, NSSet *samplers) {
    rdna4_argument_tokens_init();
    NSMutableArray *rows = [NSMutableArray array];
    [rdna4_argument_tokens_lock lock];
    rdna4_argument_token_prune_locked();
    for (NSNumber *number in rdna4_argument_tokens) {
        RDNA4ArgumentResourceToken *entry = rdna4_argument_tokens[number];
        if (entry.texture) {
            id object = entry.object;
            id heap = object ? [object heap] : nil;
            if (object && entry.device == device && ![object isAliasable] &&
                ([declared containsObject:object] || (heap && [declared containsObject:heap])))
                [rows addObject:@[number, object, @YES]];
        } else {
            id object = rdna4_argument_token_sampler_alias_locked(entry, device, samplers);
            if (object) [rows addObject:@[number, object, @NO]];
        }
    }
    [rdna4_argument_tokens_lock unlock];
    [rows sortUsingComparator:^NSComparisonResult(NSArray *left, NSArray *right) {
        uint64_t a = [left[0] unsignedLongLongValue], b = [right[0] unsignedLongLongValue];
        return a < b ? NSOrderedAscending : a > b ? NSOrderedDescending : NSOrderedSame;
    }];
    return rows;
}

@interface RDNA4ArgumentWriteIntent : NSObject {
@public
    __weak id buffer;
    uint64_t serial;
    uint64_t submissionEpoch;
    uint64_t hostGeneration;
    NSRange range;
    NSArray<NSValue *> *writeRanges;
    NSMutableArray<NSValue *> *hostOverlaps;
    NSSet *candidates;
    BOOL fillOnly, active, submitted;
}
@end
@implementation RDNA4ArgumentWriteIntent
@end

static BOOL rdna4_argument_sampler_intent_matches(id token, id buffer, NSRange range,
                                                   BOOL fillOnly) {
    RDNA4ArgumentWriteIntent *intent = token;
    return intent && intent->buffer == buffer && NSEqualRanges(intent->range, range) &&
        intent->writeRanges.count == 1 &&
        NSEqualRanges(intent->writeRanges[0].rangeValue, range) &&
        intent->fillOnly == fillOnly;
}

static BOOL rdna4_argument_sampler_intent_matches_ranges(
        id token, id buffer, NSRange range, NSArray<NSValue *> *writeRanges,
        BOOL fillOnly) {
    RDNA4ArgumentWriteIntent *intent = token;
    if (!intent || intent->buffer != buffer || !NSEqualRanges(intent->range, range) ||
        intent->fillOnly != fillOnly || intent->writeRanges.count != writeRanges.count)
        return NO;
    for (NSUInteger i = 0; i < writeRanges.count; ++i)
        if (!NSEqualRanges(intent->writeRanges[i].rangeValue,
                           writeRanges[i].rangeValue))
            return NO;
    return YES;
}

@interface RDNA4ArgumentCompletedWrite : NSObject {
@public
    uint64_t serial;
    uint64_t submissionEpoch;
    uint64_t hostGeneration;
    NSRange range;
    NSSet *candidates;
    BOOL fillOnly, exclusive;
}
@end
@implementation RDNA4ArgumentCompletedWrite
@end

@interface RDNA4ArgumentSamplerOwners : NSObject
@property NSMutableDictionary<NSNumber *, NSArray *> *slots;
@property NSMutableDictionary<NSValue *, NSSet *> *possible;
@property NSMutableArray<RDNA4ArgumentWriteIntent *> *writers;
@property NSMutableArray<RDNA4ArgumentCompletedWrite *> *completed;
@property uint64_t hostGeneration;
@property BOOL hostGenerationSaturated;
@property uint64_t latestSummaryEpoch;
@property uint64_t lastCompletedEpoch;
@end
@implementation RDNA4ArgumentSamplerOwners
- (instancetype)init {
    if ((self = [super init])) {
        _slots = [NSMutableDictionary dictionary]; _possible = [NSMutableDictionary dictionary];
        _writers = [NSMutableArray array]; _completed = [NSMutableArray array];
    }
    return self;
}
@end

static char rdna4_argument_sampler_owners_key;
static RDNA4ArgumentSamplerOwners *rdna4_argument_sampler_owners(id buffer, BOOL create) {
    RDNA4ArgumentSamplerOwners *owners = objc_getAssociatedObject(buffer, &rdna4_argument_sampler_owners_key);
    if (!owners && create) {
        owners = [RDNA4ArgumentSamplerOwners new];
        objc_setAssociatedObject(buffer, &rdna4_argument_sampler_owners_key, owners, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    }
    return owners;
}

static void rdna4_argument_sampler_refine(id buffer);

static void rdna4_argument_sampler_fill_locked(RDNA4ArgumentSamplerOwners *owners, NSRange range) {
    for (NSNumber *key in owners.slots.allKeys)
        if (NSIntersectionRange(NSMakeRange(key.unsignedIntegerValue, sizeof(uint64_t)), range).length)
            [owners.slots removeObjectForKey:key];
    for (NSValue *key in owners.possible.allKeys) {
        NSRange old = key.rangeValue;
        if (!NSIntersectionRange(old, range).length) continue;
        NSSet *samplers = owners.possible[key];
        [owners.possible removeObjectForKey:key];
        if (old.location < range.location) {
            NSValue *leftKey = [NSValue valueWithRange:
                NSMakeRange(old.location, range.location - old.location)];
            NSMutableSet *left = [owners.possible[leftKey] mutableCopy] ?: [NSMutableSet set];
            [left unionSet:samplers];
            owners.possible[leftKey] = left;
        }
        if (NSMaxRange(range) < NSMaxRange(old)) {
            NSValue *rightKey = [NSValue valueWithRange:
                NSMakeRange(NSMaxRange(range), NSMaxRange(old) - NSMaxRange(range))];
            NSMutableSet *right = [owners.possible[rightKey] mutableCopy] ?: [NSMutableSet set];
            [right unionSet:samplers];
            owners.possible[rightKey] = right;
        }
    }
}

static NSDictionary *rdna4_argument_live_samplers(id device) {
    rdna4_argument_tokens_init();
    NSMutableDictionary *live = [NSMutableDictionary dictionary];
    [rdna4_argument_tokens_lock lock];
    rdna4_argument_token_prune_locked();
    for (NSNumber *number in rdna4_argument_tokens) {
        RDNA4ArgumentResourceToken *entry = rdna4_argument_tokens[number];
        if (entry.texture) continue;
        id object = rdna4_argument_token_sampler_alias_locked(entry, device, nil);
        if (object) live[number] = object;
    }
    [rdna4_argument_tokens_lock unlock];
    return live;
}

static rdna4_ae_range rdna4_argument_epoch_range(NSRange range) {
    return (rdna4_ae_range){ range.location, range.length };
}

static NSRange rdna4_argument_epoch_nsrange(rdna4_ae_range range) {
    return NSMakeRange((NSUInteger)range.location, (NSUInteger)range.length);
}

static void rdna4_argument_sampler_add_range(NSMutableArray<NSValue *> *ranges, NSRange range) {
    if (!range.length) return;
    NSMutableArray<NSValue *> *all = [ranges mutableCopy];
    [all addObject:[NSValue valueWithRange:range]];
    [all sortUsingComparator:^NSComparisonResult(NSValue *left, NSValue *right) {
        NSRange a = left.rangeValue, b = right.rangeValue;
        return a.location < b.location ? NSOrderedAscending :
            a.location > b.location ? NSOrderedDescending : NSOrderedSame;
    }];
    [ranges removeAllObjects];
    for (NSValue *value in all) {
        NSRange next = value.rangeValue;
        if (ranges.count && next.location <= NSMaxRange(ranges.lastObject.rangeValue)) {
            NSRange current = ranges.lastObject.rangeValue;
            NSUInteger end = MAX(NSMaxRange(current), NSMaxRange(next));
            ranges[ranges.count - 1] =
                [NSValue valueWithRange:NSMakeRange(current.location, end - current.location)];
        } else {
            [ranges addObject:value];
        }
    }
}

static void rdna4_argument_sampler_subtract_ranges(NSMutableArray<NSValue *> *ranges,
        NSRange cut) {
    if (!cut.length || !ranges.count) return;
    NSArray<NSValue *> *old = [ranges copy];
    [ranges removeAllObjects];
    rdna4_ae_range removed = rdna4_argument_epoch_range(cut);
    for (NSValue *value in old) {
        rdna4_ae_range pieces[2];
        size_t count = rdna4_ae_range_subtract(rdna4_argument_epoch_range(value.rangeValue),
                                               removed, pieces);
        for (size_t i = 0; i < count; ++i)
            [ranges addObject:[NSValue valueWithRange:
                rdna4_argument_epoch_nsrange(pieces[i])]];
    }
}

static void rdna4_argument_sampler_split_completed_locked(
        RDNA4ArgumentSamplerOwners *owners, NSRange cut) {
    if (!cut.length) return;
    NSArray<RDNA4ArgumentCompletedWrite *> *old = [owners.completed copy];
    for (RDNA4ArgumentCompletedWrite *write in old) {
        NSRange overlap = NSIntersectionRange(write->range, cut);
        if (!overlap.length) continue;
        [owners.completed removeObjectIdenticalTo:write];
        NSMutableArray<NSValue *> *outside =
            [NSMutableArray arrayWithObject:[NSValue valueWithRange:write->range]];
        rdna4_argument_sampler_subtract_ranges(outside, overlap);
        for (NSValue *value in outside) {
            RDNA4ArgumentCompletedWrite *piece = [RDNA4ArgumentCompletedWrite new];
            piece->serial = write->serial;
            piece->submissionEpoch = write->submissionEpoch;
            piece->hostGeneration = write->hostGeneration;
            piece->range = value.rangeValue;
            piece->candidates = write->candidates;
            piece->fillOnly = write->fillOnly;
            piece->exclusive = write->exclusive;
            [owners.completed addObject:piece];
        }
        RDNA4ArgumentCompletedWrite *hostProtected =
            [RDNA4ArgumentCompletedWrite new];
        hostProtected->serial = write->serial;
        hostProtected->submissionEpoch = write->submissionEpoch;
        hostProtected->hostGeneration = write->hostGeneration;
        hostProtected->range = overlap;
        hostProtected->candidates = write->candidates;
        hostProtected->fillOnly = write->fillOnly;
        hostProtected->exclusive = NO;
        [owners.completed addObject:hostProtected];
    }
}

// A later CPU setter owns its bytes regardless of an earlier completion callback.
static BOOL rdna4_argument_sampler_note_host_write_locked(
        RDNA4ArgumentSamplerOwners *owners, NSRange range) {
    if (!owners || !range.length) return NO;
    if (owners.hostGeneration != UINT64_MAX) owners.hostGeneration += 1;
    else owners.hostGenerationSaturated = YES;
    for (RDNA4ArgumentWriteIntent *intent in owners.writers) {
        if (!intent->active || !NSIntersectionRange(intent->range, range).length) continue;
        rdna4_argument_sampler_add_range(intent->hostOverlaps,
                                        NSIntersectionRange(intent->range, range));
    }
    rdna4_argument_sampler_split_completed_locked(owners, range);
    return owners.completed.count != 0;
}

// The mapped bytes and owner metadata become one atomic setter operation for readers.
static void rdna4_argument_sampler_store(id buffer, NSUInteger offset, uint64_t value, id sampler) {
    BOOL refine = NO;
    @synchronized (buffer) {
        uint8_t *bytes = [(id<MTLBuffer>)buffer contents];
        if (!bytes || offset > [buffer length] || sizeof value > [buffer length] - offset) return;
        NSRange range = NSMakeRange(offset, sizeof value);
        RDNA4ArgumentSamplerOwners *owners = rdna4_argument_sampler_owners(buffer, NO);
        if (owners) refine = rdna4_argument_sampler_note_host_write_locked(owners, range);
        memcpy(bytes + offset, &value, sizeof value);
        if (!owners) owners = rdna4_argument_sampler_owners(buffer, sampler != nil);
        if (!owners) return;
        rdna4_argument_sampler_fill_locked(owners, range);
        if (sampler) owners.slots[@(offset)] = @[@(value), sampler];
        refine = refine || owners.completed.count != 0;
    }
    if (refine) rdna4_argument_sampler_refine(buffer);
}

static void rdna4_argument_sampler_write(id buffer, NSUInteger offset, id sampler, uint64_t token) {
    rdna4_argument_sampler_store(buffer, offset, token, sampler);
}

static NSSet *rdna4_argument_buffer_samplers(id buffer, NSRange range, NSDictionary *live,
                                              BOOL *fullyScanned) {
    NSMutableSet *found = [NSMutableSet set];
    if (fullyScanned) *fullyScanned = NO;
    if (!range.length || range.location > [buffer length] ||
        range.length > [buffer length] - range.location) return found;
    @synchronized (buffer) {
        RDNA4ArgumentSamplerOwners *owners = rdna4_argument_sampler_owners(buffer, NO);
        BOOL pendingWriter = NO;
        // GPU candidates survive independently of mapped bytes, including work that has not executed yet.
        for (NSValue *key in owners.possible)
            if (NSIntersectionRange(key.rangeValue, range).length) [found unionSet:owners.possible[key]];
        for (RDNA4ArgumentWriteIntent *intent in owners.writers)
            if (NSIntersectionRange(intent->range, range).length) {
                [found unionSet:intent->candidates];
                if (intent->active) pendingWriter = YES;
            }
        for (RDNA4ArgumentCompletedWrite *write in owners.completed)
            if (NSIntersectionRange(write->range, range).length) [found unionSet:write->candidates];
        const uint8_t *bytes = [(id<MTLBuffer>)buffer contents];
        if (fullyScanned)
            *fullyScanned = rdna4_ae_copy_source_fully_scanned(bytes != NULL,
                [(id<MTLBuffer>)buffer storageMode] == MTLStorageModeShared, pendingWriter);
        for (NSNumber *key in owners.slots.allKeys) {
            NSUInteger offset = key.unsignedIntegerValue;
            if (!NSIntersectionRange(NSMakeRange(offset, sizeof(uint64_t)), range).length) continue;
            NSArray *slot = owners.slots[key];
            uint64_t token = 0;
            if (bytes) memcpy(&token, bytes + offset, sizeof token);
            if (!bytes || token == [slot[0] unsignedLongLongValue]) [found addObject:slot[1]];
            else [owners.slots removeObjectForKey:key];
        }
        if (bytes) {
            NSUInteger end = range.location + range.length;
            NSUInteger offset = range.location;
            NSUInteger alignment = offset & (sizeof(uint64_t) - 1);
            if (alignment) offset += sizeof(uint64_t) - alignment;
            for (; offset <= end && sizeof(uint64_t) <= end - offset; offset += sizeof(uint64_t)) {
                uint64_t token; memcpy(&token, bytes + offset, sizeof token);
                if ((token >> 48) != 0x5244) continue;
                id sampler = live[@(token)];
                if (sampler) [found addObject:sampler];
            }
        }
    }
    return found;
}

typedef struct {
    __unsafe_unretained RDNA4Buffer *buffer;
    __unsafe_unretained NSDictionary<NSNumber *, id> *live;
    RDNA4ArgumentSamplerOwners *owners;
    NSMutableSet *found;
    BOOL readBytes;
} RDNA4ArgumentScopeTokenScan;

static bool rdna4_argument_scope_visit_token(void *context,
        const rdna4_as_work *work, uint64_t offset, uint64_t width) {
    RDNA4ArgumentScopeTokenScan *scan = context;
    if (!scan || !work || width != RDNA4_AS_TOKEN_BYTES ||
        !scan->readBytes)
        return false;
    RDNA4Buffer *buffer = scan->buffer;
    if (offset > [buffer length] || width > [buffer length] - offset)
        return false;
    uint64_t token = 0;
    const uint8_t *bytes = (const uint8_t *)[(id<MTLBuffer>)buffer contents];
    if (!bytes) return false;
    memcpy(&token, bytes + offset, sizeof token);

    NSNumber *key = @(offset);
    NSArray *slot = scan->owners.slots[key];
    if (slot.count == 2) {
        uint64_t known = [slot[0] unsignedLongLongValue];
        if (token == known) [scan->found addObject:slot[1]];
        else [scan->owners.slots removeObjectForKey:key];
    }
    if ((token >> 48) == 0x5244) {
        id sampler = scan->live[@(token)];
        if (sampler) [scan->found addObject:sampler];
    }
    return true;
}

static BOOL rdna4_argument_scope_intersects(NSRange left, NSRange right) {
    return NSIntersectionRange(left, right).length != 0;
}

typedef struct {
    __unsafe_unretained RDNA4Buffer *buffer;
    NSMutableArray<NSValue *> *ranges;
    BOOL invalid;
} RDNA4ArgumentScopeRanges;

static bool rdna4_argument_scope_collect_range(void *context,
        const rdna4_as_work *work, uint64_t offset, uint64_t width) {
    (void)work;
    RDNA4ArgumentScopeRanges *scan = context;
    if (!scan || !scan->buffer || !scan->ranges || !width) {
        if (scan) scan->invalid = YES;
        return false;
    }
    NSUInteger length = [scan->buffer length];
    if (offset > length || width > length - offset ||
        offset > NSUIntegerMax || width > NSUIntegerMax) {
        scan->invalid = YES;
        return false;
    }
    NSRange range = NSMakeRange((NSUInteger)offset, (NSUInteger)width);
    if (scan->ranges.count) {
        NSRange last = scan->ranges.lastObject.rangeValue;
        if (range.location <= NSMaxRange(last)) {
            NSUInteger end = MAX(NSMaxRange(last), NSMaxRange(range));
            scan->ranges[scan->ranges.count - 1] =
                [NSValue valueWithRange:NSMakeRange(last.location,
                                                    end - last.location)];
            return true;
        }
    }
    [scan->ranges addObject:[NSValue valueWithRange:range]];
    return true;
}

static BOOL rdna4_argument_scope_ranges_intersect(
        NSArray<NSValue *> *ranges, NSRange range) {
    for (NSValue *value in ranges)
        if (rdna4_argument_scope_intersects(value.rangeValue, range))
            return YES;
    return NO;
}

static BOOL rdna4_argument_scope_contains_token(const rdna4_as_work *work,
                                                uint64_t offset) {
    if (!work || offset < work->offset || !work->count || !work->stride ||
        (offset - work->offset) % work->stride)
        return false;
    return (offset - work->offset) / work->stride < work->count;
}

static BOOL rdna4_argument_sampler_scope_work(
        id device, RDNA4Buffer *buffer, const rdna4_as_work *work,
        NSDictionary *__strong *live, NSMutableSet *samplers,
        NSMutableArray<NSDictionary *> *writes, NSString **why) {
    if (!buffer) return YES;
    uint64_t extent = 0;
    if (rdna4_as_work_extent(work, &extent) != RDNA4_AS_OK ||
        work->offset > [buffer length] ||
        extent > [buffer length] - work->offset) {
        if (why) *why = @"argument sampler scope work exceeds its buffer";
        return NO;
    }
    NSRange envelope = NSMakeRange((NSUInteger)work->offset,
                                   (NSUInteger)extent);
    NSMutableArray<NSValue *> *workRanges = [NSMutableArray array];
    RDNA4ArgumentScopeRanges rangeScan = {
        .buffer = buffer,
        .ranges = workRanges,
    };
    rdna4_as_status rangeStatus = rdna4_as_visit_byte_ranges(
        work, rdna4_argument_scope_collect_range, &rangeScan);
    if (rangeStatus != RDNA4_AS_OK || rangeScan.invalid ||
        !workRanges.count) {
        if (why) *why = [NSString stringWithFormat:
            @"argument sampler scope cannot enumerate exact write ranges: %s",
            rdna4_argument_scan_status_name(rangeStatus)];
        return NO;
    }
    NSMutableSet *found = [NSMutableSet set];
    @synchronized (buffer) {
        RDNA4ArgumentSamplerOwners *owners =
            rdna4_argument_sampler_owners(buffer, NO);
        for (NSValue *key in owners.possible)
            if (rdna4_argument_scope_ranges_intersect(
                    workRanges, key.rangeValue))
                [found unionSet:owners.possible[key]];
        for (RDNA4ArgumentWriteIntent *intent in owners.writers)
            if (intent->active &&
                rdna4_argument_scope_ranges_intersect(
                    workRanges, intent->range))
                [found unionSet:intent->candidates];
        for (RDNA4ArgumentCompletedWrite *write in owners.completed)
            if (rdna4_argument_scope_ranges_intersect(
                    workRanges, write->range))
                [found unionSet:write->candidates];

        if (work->mode == RDNA4_AS_SCAN_CPU_MAPPED) {
            if (!*live) *live = rdna4_argument_live_samplers(device);
            RDNA4ArgumentScopeTokenScan tokenScan = {
                .buffer = buffer,
                .live = *live,
                .owners = owners,
                .found = found,
                .readBytes = YES,
            };
            if (![buffer contents] ||
                rdna4_as_visit_tokens(work, rdna4_argument_scope_visit_token,
                                      &tokenScan) != RDNA4_AS_OK) {
                if (why) *why = @"argument sampler scope cannot scan mapped token work";
                return NO;
            }
        } else if (work->mode != RDNA4_AS_SCAN_GPU_SUMMARY &&
                   work->mode != RDNA4_AS_SCAN_OPAQUE_CONSERVATIVE) {
            if (why) *why = @"argument sampler scope has an unknown scan mode";
            return NO;
        } else if (owners) {
            /*
             * Private/opaque work never reads host contents. Exact
             * owner slots are metadata, so they can still contribute
             * without turning the GPU summary or transport range into
             * a CPU parameter decision.
             */
            for (NSNumber *key in owners.slots)
                if (rdna4_argument_scope_contains_token(
                        work, key.unsignedLongLongValue) &&
                    [owners.slots[key] count] == 2)
                    [found addObject:owners.slots[key][1]];
        }
    }
    [samplers unionSet:found];
    if (work->flags & RDNA4_AS_WRITABLE) {
        [writes addObject:@{
            @"buffer": buffer,
            @"range": [NSValue valueWithRange:envelope],
            @"ranges": [workRanges copy],
            @"candidates": [found copy],
            @"fillOnly": @NO,
        }];
    }
    return YES;
}

static BOOL rdna4_argument_sampler_scope(
        id device, const RDNA4ArgumentScanPrepared *scan,
        NSSet *declaredBuffers, NSSet *writable,
        NSMutableSet *samplers, NSMutableArray<NSDictionary *> *writes,
        NSString **why) {
    if (!scan || !samplers || !writes) {
        if (why) *why = @"argument sampler scope has no scan plan";
        return NO;
    }
    NSDictionary *live = nil;
    BOOL needsDeclaredTransports = NO;
    NSMutableSet *fullyScanned = [NSMutableSet set];
    for (size_t stageIndex = 0; stageIndex < scan->count; ++stageIndex) {
        const RDNA4ArgumentScanStage *stage = &scan->stages[stageIndex];
        if (!stage->state || (stage->count && !stage->works)) {
            if (why) *why = @"argument sampler scope has malformed stage work";
            return NO;
        }
        const rdna4_argument_scan_reflection_plan *reflection = stage->state->scanPlan;
        for (size_t i = 0; reflection && i < reflection->plan.count; ++i) {
            const rdna4_as_span *span = &reflection->plan.spans[i];
            if (span->kind == RDNA4_AS_KIND_BUFFER ||
                span->kind == RDNA4_AS_KIND_UNKNOWN ||
                (span->flags & RDNA4_AS_DYNAMIC))
                needsDeclaredTransports = YES;
        }
        for (size_t workIndex = 0; workIndex < stage->count; ++workIndex) {
            const rdna4_as_work *work = &stage->works[workIndex];
            if (work->root >= RDNA4_AS_ROOTS || work->width != RDNA4_AS_TOKEN_BYTES) {
                if (why) *why = @"argument sampler scope has malformed token work";
                return NO;
            }
            RDNA4Buffer *buffer = stage->state->buf[work->root].b;
            if (!rdna4_argument_sampler_scope_work(device, buffer, work, &live,
                                                    samplers, writes, why)) return NO;
            if (buffer && !work->offset && work->stride == RDNA4_AS_TOKEN_BYTES &&
                work->count == buffer.length / RDNA4_AS_TOKEN_BYTES &&
                !!(work->flags & RDNA4_AS_WRITABLE) ==
                    rdna4_argument_scan_writable(writable, buffer))
                [fullyScanned addObject:buffer];
        }
    }
    // BDA/dynamic children need not occupy a setBuffer slot. Only this command's
    // declared closure is eligible; typed-only plans keep their sparse scans.
    if (needsDeclaredTransports) for (id object in declaredBuffers) {
        if (![object isKindOfClass:[RDNA4Buffer class]]) continue;
        RDNA4Buffer *buffer = object;
        if (buffer.device != device || [buffer isAliasable] ||
            [fullyScanned containsObject:buffer] ||
            buffer.length < RDNA4_AS_TOKEN_BYTES) continue;
        rdna4_as_bound bound = {
            .present = true,
            .mapped = buffer.contents != NULL,
            .storage = rdna4_argument_scan_storage(buffer.storageMode),
        };
        rdna4_as_work work = {
            .kind = RDNA4_AS_KIND_UNKNOWN,
            .flags = RDNA4_AS_TRANSPORT |
                (rdna4_argument_scan_writable(writable, buffer) ? RDNA4_AS_WRITABLE : 0),
            .mode = (uint8_t)rdna4_as_scan_mode_for(&bound),
            .stride = RDNA4_AS_TOKEN_BYTES,
            .count = buffer.length / RDNA4_AS_TOKEN_BYTES,
            .width = RDNA4_AS_TOKEN_BYTES,
        };
        RDNA4LOGR("argscan declared transport: %llu bytes, %llu words, mode %u",
            (unsigned long long)buffer.length, (unsigned long long)work.count,
            (unsigned)work.mode);
        if (!rdna4_argument_sampler_scope_work(device, buffer, &work, &live,
                                                samplers, writes, why)) return NO;
    }
    if (writes.count) {
        NSSet *allCandidates = [samplers copy];
        for (NSUInteger i = 0; i < writes.count; ++i) {
            NSMutableDictionary *entry = [writes[i] mutableCopy];
            entry[@"candidates"] = allCandidates;
            writes[i] = [entry copy];
        }
    }
    return YES;
}

static RDNA4ArgumentWriteIntent *rdna4_argument_sampler_intent_ranges(id buffer, NSRange range,
        NSArray<NSValue *> *writeRanges, NSSet *candidates, BOOL fillOnly) {
    if (!buffer || !range.length ||
        !rdna4_ae_range_valid(rdna4_argument_epoch_range(range), [buffer length]) ||
        !writeRanges.count) return nil;
    rdna4_ae_range envelope = rdna4_argument_epoch_range(range);
    for (NSValue *value in writeRanges) {
        NSRange exact = value.rangeValue;
        rdna4_ae_range part = rdna4_argument_epoch_range(exact);
        rdna4_ae_range overlap = rdna4_ae_range_intersection(envelope, part);
        if (!part.length || !rdna4_ae_range_valid(part, [buffer length]) ||
            overlap.location != part.location || overlap.length != part.length) return nil;
    }
    static _Atomic uint64_t nextSerial;
    uint64_t previous = atomic_load(&nextSerial);
    do {
        if (previous == UINT64_MAX) return nil;
    } while (!atomic_compare_exchange_weak(&nextSerial, &previous, previous + 1));
    uint64_t serial = previous + 1;
    RDNA4ArgumentWriteIntent *intent = [RDNA4ArgumentWriteIntent new];
    intent->buffer = buffer;
    intent->serial = serial;
    intent->range = range;
    intent->writeRanges = [writeRanges copy];
    intent->hostOverlaps = [NSMutableArray array];
    intent->candidates = [candidates copy] ?: [NSSet set];
    intent->fillOnly = fillOnly;
    intent->active = YES;
    @synchronized (buffer) {
        RDNA4ArgumentSamplerOwners *owners = rdna4_argument_sampler_owners(buffer, YES);
        intent->hostGeneration = owners.hostGeneration;
        [owners.writers addObject:intent];
    }
    return intent;
}

static RDNA4ArgumentWriteIntent *rdna4_argument_sampler_intent(id buffer, NSRange range,
        NSSet *candidates, BOOL fillOnly) {
    return rdna4_argument_sampler_intent_ranges(buffer, range,
        @[[NSValue valueWithRange:range]], candidates, fillOnly);
}

static void rdna4_argument_sampler_add_candidates(id token, NSSet *candidates) {
    RDNA4ArgumentWriteIntent *intent = token;
    id buffer = intent ? intent->buffer : nil;
    if (!buffer || !candidates.count) return;
    @synchronized (buffer) {
        if (!intent->active) return;
        NSMutableSet *merged = [intent->candidates mutableCopy];
        [merged unionSet:candidates];
        intent->candidates = merged;
    }
}

static RDNA4ArgumentWriteIntent *rdna4_argument_sampler_copy(id source, NSUInteger sourceOffset,
        id destination, NSUInteger offset, NSUInteger size) {
    if (!source || !destination || !size ||
        !rdna4_ae_range_valid((rdna4_ae_range){ sourceOffset, size }, [source length]) ||
        !rdna4_ae_range_valid((rdna4_ae_range){ offset, size }, [destination length])) return nil;
    NSRange sourceRange = NSMakeRange(sourceOffset, size);
    NSDictionary *live = rdna4_argument_live_samplers([source device]);
    BOOL fullyScanned = NO;
    NSSet *found = rdna4_argument_buffer_samplers(source, sourceRange, live, &fullyScanned);
    NSUInteger tokenMask = sizeof(uint64_t) - 1;
    BOOL wholeTokenWords = !(sourceOffset & tokenMask) && !(offset & tokenMask) &&
        !(size & tokenMask);
    BOOL fillOnly = rdna4_ae_copy_fill_only(fullyScanned, wholeTokenWords, found.count != 0);
    return rdna4_argument_sampler_intent(destination, NSMakeRange(offset, size), found,
                                         fillOnly);
}

static uint64_t rdna4_argument_next_submission_epoch(void) {
    static _Atomic uint64_t nextEpoch;
    uint64_t previous = atomic_load(&nextEpoch);
    for (;;) {
        if (previous == UINT64_MAX) return UINT64_MAX;
        if (atomic_compare_exchange_weak(&nextEpoch, &previous, previous + 1))
            return previous + 1;
    }
}

static void rdna4_argument_sampler_submit(id token, uint64_t submissionEpoch) {
    RDNA4ArgumentWriteIntent *intent = token;
    id buffer = intent ? intent->buffer : nil;
    if (!buffer || !submissionEpoch) return;
    @synchronized (buffer) {
        if (!intent->active || intent->submitted) return;
        intent->submissionEpoch = submissionEpoch;
        intent->submitted = YES;
    }
}

static void rdna4_argument_sampler_abandon(id token) {
    RDNA4ArgumentWriteIntent *intent = token;
    id buffer = intent ? intent->buffer : nil;
    if (!buffer) return;
    BOOL refine = NO;
    @synchronized (buffer) {
        if (!intent->active) return;
        intent->active = NO;
        RDNA4ArgumentSamplerOwners *owners = rdna4_argument_sampler_owners(buffer, NO);
        [owners.writers removeObjectIdenticalTo:intent];
        refine = owners.completed.count != 0;
    }
    if (refine) rdna4_argument_sampler_refine(buffer);
}

static void rdna4_argument_sampler_add_possible_locked(RDNA4ArgumentSamplerOwners *owners,
        NSRange range, NSSet *candidates) {
    if (!range.length || !candidates.count) return;
    NSValue *key = [NSValue valueWithRange:range];
    NSMutableSet *possible = [owners.possible[key] mutableCopy] ?: [NSMutableSet set];
    [possible unionSet:candidates];
    owners.possible[key] = possible;
}

static void rdna4_argument_sampler_scan_locked(RDNA4ArgumentSamplerOwners *owners,
        NSRange range, const uint8_t *bytes, NSDictionary *live) {
    if (!bytes || !range.length) return;
    NSUInteger end = range.location + range.length;
    NSUInteger offset = range.location;
    NSUInteger alignment = offset & (sizeof(uint64_t) - 1);
    if (alignment) offset += sizeof(uint64_t) - alignment;
    for (; offset <= end && sizeof(uint64_t) <= end - offset; offset += sizeof(uint64_t)) {
        uint64_t token;
        memcpy(&token, bytes + offset, sizeof token);
        if ((token >> 48) != 0x5244) continue;
        id sampler = live[@(token)];
        if (sampler) owners.slots[@(offset)] = @[@(token), sampler];
    }
}

static void rdna4_argument_sampler_refine_locked(id buffer, RDNA4ArgumentSamplerOwners *owners,
        NSDictionary *live) {
    const uint8_t *bytes = [(id<MTLBuffer>)buffer contents];
    for (;;) {
        NSMutableArray<RDNA4ArgumentCompletedWrite *> *group = nil;
        NSRange span = NSMakeRange(0, 0);
        NSArray<RDNA4ArgumentCompletedWrite *> *ordered = [owners.completed
            sortedArrayUsingComparator:^NSComparisonResult(RDNA4ArgumentCompletedWrite *a,
                                                           RDNA4ArgumentCompletedWrite *b) {
                if (a->submissionEpoch != b->submissionEpoch)
                    return a->submissionEpoch < b->submissionEpoch ? NSOrderedAscending :
                        NSOrderedDescending;
                return a->serial < b->serial ? NSOrderedAscending :
                    a->serial > b->serial ? NSOrderedDescending : NSOrderedSame;
            }];
        for (RDNA4ArgumentCompletedWrite *seed in ordered) {
            group = [NSMutableArray arrayWithObject:seed];
            span = seed->range;
            BOOL expanded;
            do {
                expanded = NO;
                for (RDNA4ArgumentCompletedWrite *other in owners.completed) {
                    if ([group containsObject:other] || !NSIntersectionRange(span, other->range).length) continue;
                    [group addObject:other];
                    NSUInteger end = MAX(NSMaxRange(span), NSMaxRange(other->range));
                    span.location = MIN(span.location, other->range.location);
                    span.length = end - span.location;
                    expanded = YES;
                }
            } while (expanded);
            BOOL blocked = NO;
            for (RDNA4ArgumentWriteIntent *writer in owners.writers) {
                if (!writer->active ||
                    NSIntersectionRange(writer->range, span).length) {
                    blocked = YES;
                    break;
                }
            }
            if (!blocked) break;
            group = nil;
        }
        if (!group) return;
        NSMutableSet *candidates = [NSMutableSet set];
        BOOL allFill = YES;
        for (RDNA4ArgumentCompletedWrite *write in group) {
            [candidates unionSet:write->candidates];
            allFill = allFill && write->fillOnly;
        }
        NSMutableArray<NSValue *> *protectedRanges = [NSMutableArray array];
        for (RDNA4ArgumentCompletedWrite *write in group)
            if (!write->exclusive)
                rdna4_argument_sampler_add_range(protectedRanges, write->range);
        NSMutableArray<NSValue *> *exclusiveRanges =
            [NSMutableArray arrayWithObject:[NSValue valueWithRange:span]];
        for (NSValue *value in protectedRanges)
            rdna4_argument_sampler_subtract_ranges(exclusiveRanges, value.rangeValue);

        if (bytes) {
            for (NSValue *value in exclusiveRanges) {
                NSRange exact = value.rangeValue;
                rdna4_argument_sampler_fill_locked(owners, exact);
                rdna4_argument_sampler_scan_locked(owners, exact, bytes, live);
            }
            if (candidates.count)
                for (NSValue *value in protectedRanges)
                    rdna4_argument_sampler_add_possible_locked(owners, value.rangeValue,
                                                              candidates);
        } else if (allFill) {
            for (NSValue *value in exclusiveRanges)
                rdna4_argument_sampler_fill_locked(owners, value.rangeValue);
        } else if (candidates.count) {
            rdna4_argument_sampler_add_possible_locked(owners, span, candidates);
        }
        [owners.completed removeObjectsInArray:group];
    }
}

static void rdna4_argument_sampler_refine(id buffer) {
    if (!buffer) return;
    @synchronized (buffer) {
        RDNA4ArgumentSamplerOwners *owners = rdna4_argument_sampler_owners(buffer, NO);
        if (!owners || !owners.completed.count) return;
        rdna4_argument_sampler_refine_locked(buffer, owners,
            rdna4_argument_live_samplers([buffer device]));
    }
}

static void rdna4_argument_sampler_complete(id token) {
    RDNA4ArgumentWriteIntent *intent = token;
    id buffer = intent ? intent->buffer : nil;
    if (!buffer) return;
    @synchronized (buffer) {
        if (!intent->active) return;
        // An unaccepted intent must use abandon, never a successful completion.
        if (!intent->submitted) return;
        RDNA4ArgumentSamplerOwners *owners = rdna4_argument_sampler_owners(buffer, NO);
        if (!owners) return;
        intent->active = NO;
        [owners.writers removeObjectIdenticalTo:intent];
        for (NSValue *value in intent->writeRanges) {
            NSMutableArray<NSValue *> *exclusiveRanges =
                [NSMutableArray arrayWithObject:value];
            for (NSValue *overlap in intent->hostOverlaps)
                rdna4_argument_sampler_subtract_ranges(exclusiveRanges, overlap.rangeValue);
            for (NSValue *exclusive in exclusiveRanges) {
                RDNA4ArgumentCompletedWrite *write = [RDNA4ArgumentCompletedWrite new];
                write->serial = intent->serial;
                write->submissionEpoch = intent->submissionEpoch;
                write->hostGeneration = intent->hostGeneration;
                write->range = exclusive.rangeValue;
                write->candidates = intent->candidates;
                write->fillOnly = intent->fillOnly;
                write->exclusive = YES;
                [owners.completed addObject:write];
            }
            for (NSValue *overlap in intent->hostOverlaps) {
                NSRange protectedRange = NSIntersectionRange(value.rangeValue,
                                                             overlap.rangeValue);
                if (!protectedRange.length) continue;
                RDNA4ArgumentCompletedWrite *write = [RDNA4ArgumentCompletedWrite new];
                write->serial = intent->serial;
                write->submissionEpoch = intent->submissionEpoch;
                write->hostGeneration = intent->hostGeneration;
                write->range = protectedRange;
                write->candidates = intent->candidates;
                write->fillOnly = intent->fillOnly;
                write->exclusive = NO;
                [owners.completed addObject:write];
            }
        }
        if (intent->submissionEpoch > owners.lastCompletedEpoch)
            owners.lastCompletedEpoch = intent->submissionEpoch;
        rdna4_argument_sampler_refine_locked(buffer, owners,
            rdna4_argument_live_samplers([buffer device]));
    }
}

@implementation RDNA4ArgumentEncoder
- (instancetype)initWithLayout:(RDNA4ArgumentLayout *)layout device:(id)device {
    if (!(self = [super init]) || !layout || !layout.encodedLength) return nil;
    _layout = layout; _device = device;
    return self;
}
- (id)device { return _device; }
- (NSString *)label { return _label; }
- (void)setLabel:(NSString *)label { _label = [label copy]; }
- (NSUInteger)encodedLength { return _layout.encodedLength; }
- (NSUInteger)alignment { return _layout.alignment; }
- (RDNA4ArgumentLayout *)rdna4Layout { return _layout; }
- (void)setArgumentBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset {
    _buffer = nil; _offset = 0;
    if (!buffer) return;
    if (![buffer isKindOfClass:RDNA4Buffer.class] || buffer.device != _device || !buffer.contents ||
        offset % self.alignment || offset > buffer.length || self.encodedLength > buffer.length - offset)
        [NSException raise:NSInvalidArgumentException format:@"Argument buffer destination is unmapped, misaligned, or too small"];
    _buffer = (RDNA4Buffer *)buffer; _offset = offset;
}
- (void)setArgumentBuffer:(id<MTLBuffer>)buffer startOffset:(NSUInteger)startOffset arrayElement:(NSUInteger)element {
    if (element > (NSUIntegerMax - startOffset) / self.encodedLength)
        [NSException raise:NSInvalidArgumentException format:@"Argument buffer array offset overflows"];
    [self setArgumentBuffer:buffer offset:startOffset + element * self.encodedLength];
}
- (void *)rdna4Destination:(NSUInteger)index type:(MTLDataType)type {
    RDNA4ArgumentField *field = [_layout field:index];
    BOOL resource = field.type == MTLDataTypePointer || field.type == MTLDataTypeTexture ||
        field.type == MTLDataTypeSampler || field.type == MTLDataTypeRenderPipeline ||
        field.type == MTLDataTypeComputePipeline ||
        field.type == MTLDataTypeIndirectCommandBuffer;
    if (!_buffer || !field || (type == MTLDataTypeNone ? resource : field.type != type) ||
        field.offset > self.encodedLength || field.size > self.encodedLength - field.offset ||
        (resource && field.size != sizeof(uint64_t)))
        [NSException raise:NSInvalidArgumentException format:@"Argument ID %lu has no matching destination", (unsigned long)index];
    return (uint8_t *)[(id<MTLBuffer>)_buffer contents] + _offset + field.offset;
}
- (uint64_t)rdna4BufferAddress:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index {
    uint64_t address = 0;
    if (buffer) {
        NSUInteger needed = [_layout field:index].nested.encodedLength;
        if (![buffer isKindOfClass:RDNA4Buffer.class] || buffer.device != _device || offset > buffer.length ||
            needed > buffer.length - offset)
            [NSException raise:NSInvalidArgumentException format:@"Argument buffer resource or offset is invalid"];
        uint64_t base = [(RDNA4Buffer *)buffer gpuAddress];
        if (!base || base > UINT64_MAX - offset)
            [NSException raise:NSInvalidArgumentException format:@"Argument buffer resource or offset is invalid"];
        address = base + offset;
    }
    return address;
}
- (void)setBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index {
    (void)[self rdna4Destination:index type:MTLDataTypePointer];
    uint64_t address = [self rdna4BufferAddress:buffer offset:offset atIndex:index];
    rdna4_argument_sampler_store(_buffer, _offset + [_layout field:index].offset, address, nil);
    if (buffer && ([_layout field:index].nested != nil || objc_getAssociatedObject(buffer, &rdna4_icb_arg_owners_key) != nil))
        rdna4_icb_arg_note(_buffer, nil, nil, (RDNA4Buffer *)buffer);
}
- (void)setBuffers:(const id<MTLBuffer> __unsafe_unretained *)buffers offsets:(const NSUInteger *)offsets withRange:(NSRange)range {
    if ((!buffers || !offsets) && range.length)
        [NSException raise:NSInvalidArgumentException format:@"Argument buffer arrays are null"];
    [self rdna4ValidateRange:range type:MTLDataTypePointer];
    for (NSUInteger n = 0; n < range.length; ++n)
        (void)[self rdna4BufferAddress:buffers[n] offset:offsets[n] atIndex:range.location + n];
    for (NSUInteger n = 0; n < range.length; ++n) [self setBuffer:buffers[n] offset:offsets[n] atIndex:range.location + n];
}
- (uint64_t)rdna4TextureToken:(id<MTLTexture>)texture {
    uint64_t token = 0;
    if (texture) {
        if (![texture isKindOfClass:RDNA4Texture.class] || texture.device != _device)
            [NSException raise:NSInvalidArgumentException format:@"Argument texture has an invalid class or device"];
        token = rdna4_argument_resource_token((RDNA4Texture *)texture, _device, YES);
        if (!token) [NSException raise:NSInvalidArgumentException format:@"Argument texture token could not be allocated"];
    }
    return token;
}
- (void)setTexture:(id<MTLTexture>)texture atIndex:(NSUInteger)index {
    (void)[self rdna4Destination:index type:MTLDataTypeTexture];
    uint64_t token = [self rdna4TextureToken:texture];
    rdna4_argument_sampler_store(_buffer, _offset + [_layout field:index].offset, token, nil);
}
- (uint64_t)rdna4SamplerToken:(id<MTLSamplerState>)sampler {
    uint64_t token = 0;
    if (sampler) {
        if (![sampler isKindOfClass:RDNA4SamplerState.class] || sampler.device != _device)
            [NSException raise:NSInvalidArgumentException format:@"Argument sampler has an invalid class or device"];
        token = rdna4_argument_resource_token((RDNA4SamplerState *)sampler, _device, NO);
        if (!token) [NSException raise:NSInvalidArgumentException format:@"Argument sampler token could not be allocated"];
    }
    return token;
}
- (void)setSamplerState:(id<MTLSamplerState>)sampler atIndex:(NSUInteger)index {
    (void)[self rdna4Destination:index type:MTLDataTypeSampler];
    uint64_t token = [self rdna4SamplerToken:sampler];
    rdna4_argument_sampler_write(_buffer, _offset + [_layout field:index].offset, sampler, token);
}
- (void)rdna4ValidateRange:(NSRange)range type:(MTLDataType)type {
    if (range.length > NSUIntegerMax - range.location || range.length > 4096)
        [NSException raise:NSInvalidArgumentException format:@"Argument resource range overflows"];
    for (NSUInteger n = 0; n < range.length; ++n)
        (void)[self rdna4Destination:range.location + n type:type];
}
- (void)setTextures:(const id<MTLTexture> __unsafe_unretained *)textures withRange:(NSRange)range {
    if (!textures && range.length)
        [NSException raise:NSInvalidArgumentException format:@"Argument texture array is null"];
    [self rdna4ValidateRange:range type:MTLDataTypeTexture];
    for (NSUInteger n = 0; n < range.length; ++n) (void)[self rdna4TextureToken:textures[n]];
    for (NSUInteger n = 0; n < range.length; ++n) [self setTexture:textures[n] atIndex:range.location + n];
}
- (void)setSamplerStates:(const id<MTLSamplerState> __unsafe_unretained *)samplers withRange:(NSRange)range {
    if (!samplers && range.length)
        [NSException raise:NSInvalidArgumentException format:@"Argument sampler array is null"];
    [self rdna4ValidateRange:range type:MTLDataTypeSampler];
    for (NSUInteger n = 0; n < range.length; ++n) (void)[self rdna4SamplerToken:samplers[n]];
    for (NSUInteger n = 0; n < range.length; ++n) [self setSamplerState:samplers[n] atIndex:range.location + n];
}
- (uint64_t)rdna4IndirectCommandBufferAddress:(id<MTLIndirectCommandBuffer>)icb {
    uint64_t address = 0;
    if (icb) {
        Class icbClass = objc_getClass("RDNA4IndirectCommandBuffer");
        if (!icbClass || ![icb isKindOfClass:icbClass] || icb.device != _device)
            [NSException raise:NSInvalidArgumentException format:@"Argument indirect command buffer has an invalid class or device"];
        address = [(id<RDNA4ICBInternal>)icb rdna4EnsureGPUBackingAddress];
        if (!address)
            [NSException raise:NSInvalidArgumentException format:@"Argument indirect command buffer backing GPU address is unavailable"];
    }
    return address;
}
- (void)setIndirectCommandBuffer:(id<MTLIndirectCommandBuffer>)indirectCommandBuffer atIndex:(NSUInteger)index {
    (void)[self rdna4Destination:index type:MTLDataTypeIndirectCommandBuffer];
    uint64_t address = [self rdna4IndirectCommandBufferAddress:indirectCommandBuffer];
    rdna4_argument_sampler_store(_buffer, _offset + [_layout field:index].offset, address, nil);
    if (indirectCommandBuffer) rdna4_icb_arg_note(_buffer, indirectCommandBuffer, nil, nil);
}
- (void)setIndirectCommandBuffers:(const id<MTLIndirectCommandBuffer> __unsafe_unretained *)buffers withRange:(NSRange)range {
    if (!buffers && range.length)
        [NSException raise:NSInvalidArgumentException format:@"Argument indirect command buffer array is null"];
    [self rdna4ValidateRange:range type:MTLDataTypeIndirectCommandBuffer];
    for (NSUInteger n = 0; n < range.length; ++n) (void)[self rdna4IndirectCommandBufferAddress:buffers[n]];
    for (NSUInteger n = 0; n < range.length; ++n) [self setIndirectCommandBuffer:buffers[n] atIndex:range.location + n];
}
- (uint64_t)rdna4RenderPipelineToken:(id<MTLRenderPipelineState>)pipeline {
    uint64_t token = 0;
    if (pipeline) {
        if (![pipeline isKindOfClass:RDNA4RenderPipelineState.class] || pipeline.device != _device ||
            ![pipeline supportIndirectCommandBuffers])
            [NSException raise:NSInvalidArgumentException format:@"Argument render pipeline state is invalid or does not support indirect command buffers"];
        token = [(RDNA4RenderPipelineState *)pipeline rdna4ICBToken];
        if (!token)
            [NSException raise:NSInvalidArgumentException format:@"Argument render pipeline state has no ICB token"];
    }
    return token;
}
- (void)setRenderPipelineState:(id<MTLRenderPipelineState>)pipeline atIndex:(NSUInteger)index {
    (void)[self rdna4Destination:index type:MTLDataTypeRenderPipeline];
    uint64_t token = [self rdna4RenderPipelineToken:pipeline];
    rdna4_argument_sampler_store(_buffer, _offset + [_layout field:index].offset, token, nil);
    if (pipeline) rdna4_icb_arg_note(_buffer, nil, pipeline, nil);
}
- (void)setRenderPipelineStates:(const id<MTLRenderPipelineState> __unsafe_unretained *)pipelines withRange:(NSRange)range {
    if (!pipelines && range.length)
        [NSException raise:NSInvalidArgumentException format:@"Argument render pipeline array is null"];
    [self rdna4ValidateRange:range type:MTLDataTypeRenderPipeline];
    for (NSUInteger n = 0; n < range.length; ++n) (void)[self rdna4RenderPipelineToken:pipelines[n]];
    for (NSUInteger n = 0; n < range.length; ++n) [self setRenderPipelineState:pipelines[n] atIndex:range.location + n];
}
- (uint64_t)rdna4ComputePipelineToken:(id<MTLComputePipelineState>)pipeline {
    uint64_t token = 0;
    if (pipeline) {
        if (![pipeline isKindOfClass:RDNA4ComputePipelineState.class] || pipeline.device != _device ||
            ![pipeline supportIndirectCommandBuffers])
            [NSException raise:NSInvalidArgumentException format:@"Argument compute pipeline state is invalid or does not support indirect command buffers"];
        token = [(RDNA4ComputePipelineState *)pipeline rdna4ICBToken];
        if (!token)
            [NSException raise:NSInvalidArgumentException format:@"Argument compute pipeline state has no ICB token"];
    }
    return token;
}
- (void)setComputePipelineState:(id<MTLComputePipelineState>)pipeline atIndex:(NSUInteger)index {
    (void)[self rdna4Destination:index type:MTLDataTypeComputePipeline];
    uint64_t token = [self rdna4ComputePipelineToken:pipeline];
    rdna4_argument_sampler_store(_buffer, _offset + [_layout field:index].offset, token, nil);
    if (pipeline) rdna4_icb_arg_note(_buffer, nil, pipeline, nil);
}
- (void)setComputePipelineStates:(const id<MTLComputePipelineState> __unsafe_unretained *)pipelines withRange:(NSRange)range {
    if (!pipelines && range.length)
        [NSException raise:NSInvalidArgumentException format:@"Argument compute pipeline array is null"];
    [self rdna4ValidateRange:range type:MTLDataTypeComputePipeline];
    for (NSUInteger n = 0; n < range.length; ++n) (void)[self rdna4ComputePipelineToken:pipelines[n]];
    for (NSUInteger n = 0; n < range.length; ++n) [self setComputePipelineState:pipelines[n] atIndex:range.location + n];
}
- (void *)constantDataAtIndex:(NSUInteger)index { return [self rdna4Destination:index type:MTLDataTypeNone]; }
- (id)newArgumentEncoderForBufferAtIndex:(NSUInteger)index {
    RDNA4ArgumentLayout *nested = [_layout field:index].nested;
    return nested ? [[RDNA4ArgumentEncoder alloc] initWithLayout:nested device:_device] : nil;
}
@end

static BOOL rdna4_argument_reflection_type(MTLDataType type, MTLStructType *structure,
        MTLArrayType *array, MTLPointerType *pointer, RDNA4ArgumentLayout *layout,
        NSUInteger index, unsigned depth) {
    if (depth > 16) return NO;
    if (type == MTLDataTypeStruct) {
        for (MTLStructMember *member in structure.members) {
            if (member.argumentIndex > NSUIntegerMax - index ||
                !rdna4_argument_reflection_type(member.dataType, member.structType, member.arrayType,
                    member.pointerType, layout, index + member.argumentIndex, depth + 1)) return NO;
        }
    } else if (type == MTLDataTypeArray) {
        if (!array) return NO;
        if (array.elementType != MTLDataTypeStruct && array.elementType != MTLDataTypeArray &&
            array.elementType != MTLDataTypePointer) return YES;
        if (array.arrayLength > 4096 || (array.arrayLength && !array.argumentIndexStride)) return NO;
        for (NSUInteger n = 0; n < array.arrayLength; ++n) {
            if (n > (NSUIntegerMax - index) / array.argumentIndexStride ||
                !rdna4_argument_reflection_type(array.elementType, array.elementStructType, array.elementArrayType,
                    array.elementPointerType, layout, index + n * array.argumentIndexStride, depth + 1)) return NO;
        }
    } else if (type == MTLDataTypePointer && pointer.elementIsArgumentBuffer) {
        RDNA4ArgumentLayout *nested = [layout field:index].nested;
        SEL setter = sel_registerName("setAlignment:dataSize:");
        if (!nested || pointer.elementType != MTLDataTypeStruct || ![pointer respondsToSelector:setter]) return NO;
        // Indirect pointers use the encoded layout, not the top-level constant descriptor's minimum alignment.
        ((void (*)(id, SEL, NSUInteger, NSUInteger))objc_msgSend)(pointer, setter, nested.alignment, nested.encodedLength);
        return rdna4_argument_reflection_type(MTLDataTypeStruct, pointer.elementStructType, nil, nil,
            nested, 0, depth + 1);
    }
    return YES;
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
static BOOL rdna4_argument_reflection(id<MTLFunction> function, NSArray<MTLArgument *> *arguments) {
    for (MTLArgument *argument in arguments) {
        if (argument.type != MTLArgumentTypeBuffer || !argument.bufferPointerType.elementIsArgumentBuffer) continue;
        id encoder = [function newArgumentEncoderWithBufferIndex:argument.index];
        if (![encoder isKindOfClass:RDNA4ArgumentEncoder.class] ||
            !rdna4_argument_reflection_type(MTLDataTypeStruct, argument.bufferStructType, nil, nil,
                [encoder rdna4Layout], 0, 0)) return NO;
    }
    return YES;
}
#pragma clang diagnostic pop

#endif
