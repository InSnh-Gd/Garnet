/*
 * Device-level argument scan integration fixture.
 *
 * This includes the production device implementation and builds mock Metal
 * buffers that are still RDNA4Buffer instances. Plan materialization and
 * sampler scope are production code paths; no Vulkan device, command queue, or
 * GPU submission is used.
 *
 * The fixture covers the runtime properties that the pure planner test cannot:
 *   - every draw/dispatch re-reads Shared and Managed CPU mappings,
 *   - >64 MiB typed fields use their sparse token ranges,
 *   - storage outside the reflected root plan receives no scan work,
 *   - unrelated live samplers do not become a device-wide fallback,
 *   - an aliasable resource is never read, but previously captured owner
 *     metadata remains visible to conservative ownership.
 *
 * macOS host build:
 *   clang -arch x86_64 -fobjc-arc -fblocks -O1 -Wall -Wextra -Werror \
 *     -I <vulkan include> -I . test-argument-scan-runtime.m \
 *     -framework Foundation -framework Metal -framework IOKit \
 *     -framework IOSurface -o test-argument-scan-runtime
 */
#include "GarnetDevice.m"

#include <inttypes.h>
#include <string.h>

@interface RDNA4ScanTestBuffer : RDNA4Buffer {
    uint8_t *_testBytes;
    NSUInteger _testLength;
    MTLStorageMode _testStorage;
    id<MTLDevice> _testDevice;
    BOOL _testAliasable;
}
- (BOOL)rdna4TestConfigureLength:(NSUInteger)length
                      storageMode:(MTLStorageMode)storageMode
                           device:(id<MTLDevice>)device;
- (void)rdna4TestSetAliasable:(BOOL)aliasable;
@end

@implementation RDNA4ScanTestBuffer
- (BOOL)rdna4TestConfigureLength:(NSUInteger)length
                      storageMode:(MTLStorageMode)storageMode
                           device:(id<MTLDevice>)device {
    if (!length) return NO;
    _testBytes = calloc(1, length);
    if (!_testBytes) return NO;
    _testLength = length;
    _testStorage = storageMode;
    _testDevice = device;
    return YES;
}
- (void)rdna4TestSetAliasable:(BOOL)aliasable {
    _testAliasable = aliasable;
}
- (void *)contents { return _testBytes; }
- (NSUInteger)length { return _testLength; }
- (MTLStorageMode)storageMode { return _testStorage; }
- (id<MTLDevice>)device { return _testDevice; }
- (BOOL)isAliasable { return _testAliasable; }
- (id<MTLHeap>)heap { return nil; }
- (NSUInteger)resourceOptions {
    return (NSUInteger)_testStorage << 4;
}
- (void)dealloc {
    free(_testBytes);
}
@end

@interface RDNA4ScanTestHeap : RDNA4Heap {
@public
    NSArray *children;
}
@end
@implementation RDNA4ScanTestHeap
- (NSArray *)rdna4LiveResources { return children; }
@end

@interface RDNA4ScanTestCommandBuffer : RDNA4CommandBuffer {
@public
    id testDevice;
    NSMutableSet *capturedSamplers;
}
@end
@implementation RDNA4ScanTestCommandBuffer
- (id)device { return testDevice; }
- (void)rdna4RetainArgument:(id)object { (void)object; }
- (NSError *)rdna4Error { return nil; }
- (void)rdna4ArgumentWriteRanges:(id)buffer ranges:(NSArray<NSValue *> *)ranges
                     candidates:(NSSet *)candidates {
    (void)buffer; (void)ranges;
    [capturedSamplers unionSet:candidates];
}
@end

typedef struct {
    NSUInteger writes;
    NSUInteger exactRanges;
    uint64_t touchedBytes;
    uint64_t elapsedNs;
} ScopeObservation;

static unsigned checks;
static unsigned scopes;
static unsigned sharedRounds;
static unsigned managedRounds;
static uint64_t cpuTokens;
static uint64_t exactRanges;
static uint64_t touchedBytes;
static uint64_t fallbackWorks;
static uint64_t maximumScopeNs;

static void require(BOOL value, const char *message) {
    ++checks;
    if (!value) {
        fprintf(stderr, "ARGUMENT_SCAN_RUNTIME_FAIL %s\n", message);
        exit(1);
    }
}

static void requireStatus(rdna4_as_status status, const char *operation) {
    if (status != RDNA4_AS_OK) {
        fprintf(stderr, "ARGUMENT_SCAN_RUNTIME_FAIL %s status=%d\n",
                operation, (int)status);
        exit(1);
    }
    ++checks;
}

static RDNA4ScanTestBuffer *newBuffer(id device, NSUInteger length,
                                      MTLStorageMode storageMode) {
    RDNA4ScanTestBuffer *buffer = [RDNA4ScanTestBuffer alloc];
    require(buffer != nil, "mock buffer allocation");
    require([buffer rdna4TestConfigureLength:length
                                storageMode:storageMode
                                     device:(id<MTLDevice>)device],
            "mock buffer storage allocation");
    return buffer;
}

static void writeToken(RDNA4Buffer *buffer, NSUInteger offset,
                       uint64_t token) {
    require(buffer.contents != NULL && offset <= buffer.length &&
                sizeof(token) <= buffer.length - offset,
            "raw token write range");
    memcpy((uint8_t *)buffer.contents + offset, &token, sizeof(token));
}

static uint64_t readToken(RDNA4Buffer *buffer, NSUInteger offset) {
    require(buffer.contents != NULL && offset <= buffer.length &&
                sizeof(uint64_t) <= buffer.length - offset,
            "raw token read range");
    uint64_t token = 0;
    memcpy(&token, (const uint8_t *)buffer.contents + offset, sizeof(token));
    return token;
}

static NSArray *makeLiveSamplers(id device, NSUInteger count) {
    NSMutableArray *samplers =
        [NSMutableArray arrayWithCapacity:count];
    for (NSUInteger i = 0; i < count; ++i) {
        NSObject *sampler = [NSObject new];
        require(sampler != nil, "unrelated live sampler allocation");
        require(rdna4_argument_resource_token(sampler, device, NO) != 0,
                "unrelated live sampler token");
        [samplers addObject:sampler];
    }
    return samplers;
}

static void buildPlan(rdna4_argument_scan_reflection_plan *plan,
                      const rdna4_argument_scan_reflection_field *fields,
                      size_t count) {
    requireStatus(rdna4_argument_scan_reflection_plan_build(
                      plan, fields, count, NULL, 0),
                  "reflection plan build");
}

static void prepareScan(RDNA4ArgumentScanPrepared *scan,
                        const RDNA4StageState *state, NSSet *writable,
                        id device) {
    const RDNA4StageState *stages[] = { state };
    NSString *why = nil;
    BOOL prepared = rdna4_argument_scan_prepare(scan, stages, 1, writable,
                                                device, &why);
    require(prepared, why.UTF8String ?: "argument scan preparation");
}

static ScopeObservation runScope(id device,
                                 const RDNA4ArgumentScanPrepared *scan,
                                 NSMutableSet *samplers,
                                 NSMutableArray<NSDictionary *> *writes) {
    uint64_t started = rdna4_now();
    NSString *why = nil;
    BOOL scoped = rdna4_argument_sampler_scope(device, scan, nil, nil, samplers, writes,
                                               &why);
    require(scoped, why.UTF8String ?: "argument sampler scope");
    ScopeObservation observation = {
        .writes = writes.count,
        .elapsedNs = rdna4_now() - started,
    };
    for (NSDictionary *write in writes) {
        NSArray<NSValue *> *ranges = write[@"ranges"];
        require([ranges isKindOfClass:[NSArray class]] && ranges.count,
                "scope write has no exact ranges");
        observation.exactRanges += ranges.count;
        for (NSValue *value in ranges) {
            NSRange range = value.rangeValue;
            require(range.length != 0, "scope emitted an empty exact range");
            observation.touchedBytes += range.length;
        }
    }
    ++scopes;
    exactRanges += observation.exactRanges;
    touchedBytes += observation.touchedBytes;
    maximumScopeNs = MAX(maximumScopeNs, observation.elapsedNs);
    require(observation.elapsedNs < UINT64_C(5) * 1000000000,
            "sampler scope exceeded its bounded runtime");
    return observation;
}

static void requireOnlySamplers(NSSet *actual, NSSet *expected,
                                const char *message) {
    require(actual.count == expected.count, message);
    for (id sampler in expected)
        require([actual containsObject:sampler], message);
}

static void requireCpuStats(const RDNA4ArgumentScanPrepared *scan,
                            uint64_t elements, uint64_t extent,
                            uint64_t touched) {
    require(scan->count == 1 && scan->stages[0].count == 1,
            "CPU scan has an unexpected work count");
    const rdna4_as_work_stats *stats = &scan->stages[0].stats;
    require(stats->works == 1 && stats->cpu_works == 1 &&
                stats->gpu_works == 0 && stats->opaque_works == 0,
            "CPU scan work classification");
    require(stats->cpu_elements == elements,
            "CPU scan element count");
    require(stats->cpu_extent_bytes == extent,
            "CPU scan extent count");
    require(stats->cpu_touched_bytes == touched,
            "CPU scan touched-byte count");
    require(stats->fallback_works == 0,
            "typed CPU scan used a fallback");
    fallbackWorks += stats->fallback_works;
    cpuTokens += stats->cpu_elements;
}

static void testSharedRechecksRawContents(void) {
    id device = [NSObject new];
    NSArray *unrelated = makeLiveSamplers(device, 128);
    NSObject *first = [NSObject new];
    NSObject *second = [NSObject new];
    uint64_t firstToken = rdna4_argument_resource_token(first, device, NO);
    uint64_t secondToken = rdna4_argument_resource_token(second, device, NO);
    uint64_t unrelatedToken =
        rdna4_argument_resource_token(unrelated.firstObject, device, NO);
    require(firstToken && secondToken && unrelatedToken,
            "Shared raw-write token creation");

    RDNA4ScanTestBuffer *buffer =
        newBuffer(device, 128, MTLStorageModeShared);
    writeToken(buffer, 0, unrelatedToken);
    const rdna4_argument_scan_reflection_field fields[] = {
        {
            .root = 3,
            .kind = RDNA4_AS_KIND_SAMPLER,
            .count_known = 1,
            .writable = 1,
            .field_offset = 32,
            .count = 1,
            .stride = RDNA4_AS_TOKEN_BYTES,
        },
    };
    rdna4_argument_scan_reflection_plan plan = {0};
    buildPlan(&plan, fields, sizeof(fields) / sizeof(fields[0]));
    RDNA4StageState state = {0};
    state.scanPlan = &plan;
    state.buf[3].b = buffer;
    NSSet *writable = [NSSet setWithObject:buffer];

    for (unsigned round = 0; round < 2; ++round) {
        NSObject *expected = round ? second : first;
        uint64_t expectedToken = round ? secondToken : firstToken;
        writeToken(buffer, 32, expectedToken);
        require(rdna4_argument_sampler_owners(buffer, NO) == nil,
                "raw Shared write created CPU setter metadata");

        RDNA4ArgumentScanPrepared scan = {0};
        prepareScan(&scan, &state, writable, device);
        requireCpuStats(&scan, 1, sizeof(uint64_t), sizeof(uint64_t));

        NSMutableSet *samplers = [NSMutableSet set];
        NSMutableArray<NSDictionary *> *writes = [NSMutableArray array];
        ScopeObservation observation =
            runScope(device, &scan, samplers, writes);
        require(observation.writes == 1 && observation.exactRanges == 1 &&
                    observation.touchedBytes == sizeof(uint64_t),
                "Shared scope exact-range count");
        requireOnlySamplers(samplers,
                            [NSSet setWithObject:expected],
                            "Shared scope did not use current raw bytes");
        require(![samplers containsObject:
                    (round ? first : second)],
                "Shared scope retained the previous raw token");
        require(![samplers containsObject:unrelated.firstObject],
                "Shared scope leaked a live device sampler");
        require(writes.count == 1 &&
                    [writes[0][@"candidates"] isEqualToSet:samplers],
                "Shared write did not carry the exact sampler owner");
        rdna4_argument_scan_prepared_destroy(&scan);
        ++sharedRounds;
    }

    rdna4_argument_scan_reflection_plan_destroy(&plan);
}

static void testManagedRechecksRawContents(void) {
    id device = [NSObject new];
    NSArray *unrelated = makeLiveSamplers(device, 128);
    NSObject *first = [NSObject new];
    NSObject *second = [NSObject new];
    uint64_t firstToken = rdna4_argument_resource_token(first, device, NO);
    uint64_t secondToken = rdna4_argument_resource_token(second, device, NO);
    uint64_t unrelatedToken =
        rdna4_argument_resource_token(unrelated.firstObject, device, NO);
    require(firstToken && secondToken && unrelatedToken,
            "Managed raw-write token creation");

    RDNA4ScanTestBuffer *buffer =
        newBuffer(device, 128, MTLStorageModeManaged);
    writeToken(buffer, 0, unrelatedToken);
    const rdna4_argument_scan_reflection_field fields[] = {
        {
            .root = 4,
            .kind = RDNA4_AS_KIND_SAMPLER,
            .count_known = 1,
            .writable = 1,
            .field_offset = 32,
            .count = 1,
            .stride = RDNA4_AS_TOKEN_BYTES,
        },
    };
    rdna4_argument_scan_reflection_plan plan = {0};
    buildPlan(&plan, fields, sizeof(fields) / sizeof(fields[0]));
    RDNA4StageState state = {0};
    state.scanPlan = &plan;
    state.buf[4].b = buffer;
    NSSet *writable = [NSSet setWithObject:buffer];

    for (unsigned round = 0; round < 2; ++round) {
        NSObject *expected = round ? second : first;
        uint64_t expectedToken = round ? secondToken : firstToken;
        /*
         * Deliberately bypass -didModifyRange: this backend's Managed storage
         * is coherent, and a new work item must observe the current bytes.
         */
        writeToken(buffer, 32, expectedToken);
        require(rdna4_argument_sampler_owners(buffer, NO) == nil,
                "raw Managed write created CPU setter metadata");

        RDNA4ArgumentScanPrepared scan = {0};
        prepareScan(&scan, &state, writable, device);
        requireCpuStats(&scan, 1, sizeof(uint64_t), sizeof(uint64_t));

        NSMutableSet *samplers = [NSMutableSet set];
        NSMutableArray<NSDictionary *> *writes = [NSMutableArray array];
        ScopeObservation observation =
            runScope(device, &scan, samplers, writes);
        require(observation.writes == 1 && observation.exactRanges == 1 &&
                    observation.touchedBytes == sizeof(uint64_t),
                "Managed scope exact-range count");
        requireOnlySamplers(samplers,
                            [NSSet setWithObject:expected],
                            "Managed scope did not use current raw bytes");
        require(![samplers containsObject:
                    (round ? first : second)],
                "Managed scope retained the previous raw token");
        require(![samplers containsObject:unrelated.firstObject],
                "Managed scope leaked a live device sampler");
        require(writes.count == 1 &&
                    [writes[0][@"candidates"] isEqualToSet:samplers],
                "Managed write did not carry the exact sampler owner");
        rdna4_argument_scan_prepared_destroy(&scan);
        ++managedRounds;
    }

    rdna4_argument_scan_reflection_plan_destroy(&plan);
}

static void testLargeSparseTypedAndUnreflectedRoot(void) {
    id device = [NSObject new];
    NSArray *unrelated = makeLiveSamplers(device, 128);
    NSObject *first = [NSObject new];
    NSObject *second = [NSObject new];
    NSObject *paddingDecoy = [NSObject new];
    NSObject *unreflected = [NSObject new];
    uint64_t firstToken = rdna4_argument_resource_token(first, device, NO);
    uint64_t secondToken = rdna4_argument_resource_token(second, device, NO);
    uint64_t paddingToken =
        rdna4_argument_resource_token(paddingDecoy, device, NO);
    uint64_t unreflectedToken =
        rdna4_argument_resource_token(unreflected, device, NO);
    require(firstToken && secondToken && paddingToken && unreflectedToken,
            "large sparse token creation");

    const uint64_t oldLimit = UINT64_C(64) << 20;
    const uint64_t stride = 4096;
    const uint64_t fieldOffset = oldLimit + 4096;
    const NSUInteger length =
        (NSUInteger)(oldLimit + (UINT64_C(1) << 20) + 4096);
    RDNA4ScanTestBuffer *buffer =
        newBuffer(device, length, MTLStorageModeShared);
    RDNA4ScanTestBuffer *ordinaryLarge =
        newBuffer(device, length, MTLStorageModeShared);
    writeToken(buffer, (NSUInteger)fieldOffset, firstToken);
    writeToken(buffer, (NSUInteger)(fieldOffset + stride), secondToken);
    writeToken(buffer, (NSUInteger)(fieldOffset + sizeof(uint64_t)),
               paddingToken);
    writeToken(ordinaryLarge, 128, unreflectedToken);

    const rdna4_argument_scan_reflection_field fields[] = {
        {
            .root = 5,
            .kind = RDNA4_AS_KIND_SAMPLER,
            .count_known = 1,
            .writable = 1,
            .field_offset = fieldOffset,
            .count = 2,
            .stride = stride,
        },
    };
    rdna4_argument_scan_reflection_plan plan = {0};
    buildPlan(&plan, fields, sizeof(fields) / sizeof(fields[0]));
    RDNA4StageState state = {0};
    state.scanPlan = &plan;
    state.buf[5].b = buffer;
    /* Large normal storage remains bound but is absent from reflection. */
    state.buf[6].b = ordinaryLarge;
    NSSet *writable = [NSSet setWithObject:buffer];

    RDNA4ArgumentScanPrepared scan = {0};
    prepareScan(&scan, &state, writable, device);
    requireCpuStats(&scan, 2, stride + sizeof(uint64_t),
                    2 * sizeof(uint64_t));
    require(scan.stages[0].works[0].root == 5,
            "large scan did not retain its reflected root");

    NSMutableSet *samplers = [NSMutableSet set];
    NSMutableArray<NSDictionary *> *writes = [NSMutableArray array];
    ScopeObservation observation =
        runScope(device, &scan, samplers, writes);
    require(observation.writes == 1 && observation.exactRanges == 2 &&
                observation.touchedBytes == 2 * sizeof(uint64_t),
            "large sparse exact-range count collapsed its stride");
    requireOnlySamplers(samplers,
                        [NSSet setWithObjects:first, second, nil],
                        "large sparse scan result");
    require(![samplers containsObject:unrelated.firstObject],
            "large sparse scan leaked a live device sampler");
    require(![samplers containsObject:paddingDecoy],
            "large sparse scan read padding between typed elements");
    require(![samplers containsObject:unreflected],
            "large unreflected root was scanned");
    require(readToken(buffer, (NSUInteger)(fieldOffset + sizeof(uint64_t))) ==
                paddingToken,
            "large sparse scan modified padding");
    require(writes.count == 1 &&
                [writes[0][@"ranges"] count] == 2,
            "large unreflected root emitted write ranges");

    /*
     * No work may name root 6. Checking the materialized work also catches a
     * future change that passes declared roots rather than reflected roots.
     */
    for (size_t i = 0; i < scan.stages[0].count; ++i)
        require(scan.stages[0].works[i].root != 6,
                "unreflected large root received scan work");
    rdna4_argument_scan_prepared_destroy(&scan);
    rdna4_argument_scan_reflection_plan_destroy(&plan);
}

static void testAliasableOwnerMetadataWithoutRawRead(void) {
    id device = [NSObject new];
    NSArray *unrelated = makeLiveSamplers(device, 128);
    NSObject *owner = [NSObject new];
    NSObject *illegalRaw = [NSObject new];
    uint64_t ownerToken = rdna4_argument_resource_token(owner, device, NO);
    uint64_t illegalToken =
        rdna4_argument_resource_token(illegalRaw, device, NO);
    require(ownerToken && illegalToken,
            "aliasable owner token creation");

    RDNA4ScanTestBuffer *buffer =
        newBuffer(device, 128, MTLStorageModeShared);
    /*
     * Establish the metadata while the resource is still non-aliasable.
     * Production setter paths perform this before the application can mark
     * the resource aliasable after its last legal use.
     */
    rdna4_argument_sampler_write(buffer, 32, owner, ownerToken);
    RDNA4ArgumentSamplerOwners *owners =
        rdna4_argument_sampler_owners(buffer, NO);
    require(owners != nil && owners.slots.count == 1,
            "aliasable owner metadata setup");
    [buffer rdna4TestSetAliasable:YES];

    /*
     * Apple documents that reading or writing an aliased resource instance is
     * undefined behavior. This value must never be trusted as a new owner.
     */
    writeToken(buffer, 32, illegalToken);

    const rdna4_argument_scan_reflection_field fields[] = {
        {
            .root = 7,
            .kind = RDNA4_AS_KIND_SAMPLER,
            .count_known = 1,
            .writable = 1,
            .field_offset = 32,
            .count = 1,
            .stride = RDNA4_AS_TOKEN_BYTES,
        },
    };
    rdna4_argument_scan_reflection_plan plan = {0};
    buildPlan(&plan, fields, sizeof(fields) / sizeof(fields[0]));
    RDNA4StageState state = {0};
    state.scanPlan = &plan;
    state.buf[7].b = buffer;
    NSSet *writable = [NSSet setWithObject:buffer];

    RDNA4ArgumentScanPrepared scan = {0};
    prepareScan(&scan, &state, writable, device);
    require(scan.count == 1 && scan.stages[0].count == 1,
            "aliasable scan has an unexpected work count");
    const rdna4_as_work_stats *stats = &scan.stages[0].stats;
    require(stats->works == 1 && stats->opaque_works == 1 &&
                stats->cpu_works == 0 && stats->gpu_works == 0,
            "aliasable storage was not conservative");
    require(stats->fallback_works == 0,
            "aliasable metadata case used a whole-root fallback");
    fallbackWorks += stats->fallback_works;

    NSMutableSet *samplers = [NSMutableSet set];
    NSMutableArray<NSDictionary *> *writes = [NSMutableArray array];
    ScopeObservation observation =
        runScope(device, &scan, samplers, writes);
    require(observation.writes == 1 && observation.exactRanges == 1 &&
                observation.touchedBytes == sizeof(uint64_t),
            "aliasable metadata exact-range count");
    requireOnlySamplers(samplers,
                        [NSSet setWithObject:owner],
                        "aliasable metadata owner was lost");
    require(![samplers containsObject:illegalRaw],
            "aliased resource bytes were read");
    require(![samplers containsObject:unrelated.firstObject],
            "aliasable scope leaked a live device sampler");
    require(writes.count == 1 &&
                [writes[0][@"candidates"] isEqualToSet:samplers],
            "aliasable write did not preserve the known owner");

    rdna4_argument_scan_prepared_destroy(&scan);
    rdna4_argument_scan_reflection_plan_destroy(&plan);
}

static void testDeclaredUnboundChildren(void) {
    id device = [NSObject new];
    NSArray *unrelated = makeLiveSamplers(device, 128);
    NSObject *selected = [NSObject new], *privateOwner = [NSObject new];
    NSObject *decoy = [NSObject new], *updated = [NSObject new];
    uint64_t selectedToken = rdna4_argument_resource_token(selected, device, NO);
    uint64_t privateToken = rdna4_argument_resource_token(privateOwner, device, NO);
    uint64_t decoyToken = rdna4_argument_resource_token(decoy, device, NO);
    uint64_t updatedToken = rdna4_argument_resource_token(updated, device, NO);
    RDNA4ScanTestBuffer *root = newBuffer(device, 64, MTLStorageModeShared);
    RDNA4ScanTestBuffer *shared = newBuffer(device, 128, MTLStorageModeShared);
    RDNA4ScanTestBuffer *managed = newBuffer(device, 128, MTLStorageModeManaged);
    RDNA4ScanTestBuffer *private = newBuffer(device, 128, MTLStorageModePrivate);
    RDNA4ScanTestBuffer *alias = newBuffer(device, 128, MTLStorageModeShared);
    RDNA4ScanTestBuffer *foreign = newBuffer([NSObject new], 128, MTLStorageModeShared);
    RDNA4ScanTestBuffer *undeclared = newBuffer(device, 128, MTLStorageModeShared);
    for (RDNA4Buffer *buffer in @[shared, managed]) writeToken(buffer, 40, selectedToken);
    for (RDNA4Buffer *buffer in @[private, alias, foreign, undeclared])
        writeToken(buffer, 40, decoyToken);
    rdna4_argument_sampler_write(private, 48, privateOwner, privateToken);
    [alias rdna4TestSetAliasable:YES];
    require(rdna4_argument_sampler_owners(shared, NO) == nil &&
            rdna4_argument_sampler_owners(managed, NO) == nil,
            "raw child tokens unexpectedly have setter metadata");
    NSSet *declared = [NSSet setWithArray:@[shared, managed, private, alias, foreign]];
    NSSet *writable = [NSSet setWithArray:@[shared, managed]];
    const uint32_t kinds[] = {RDNA4_AS_KIND_BUFFER, RDNA4_AS_KIND_UNKNOWN,
                              RDNA4_AS_KIND_SAMPLER, RDNA4_AS_KIND_SAMPLER};
    for (unsigned variant = 0; variant < 4; ++variant) {
        rdna4_argument_scan_reflection_field field = {
            .root = 3, .kind = kinds[variant], .count_known = variant != 2,
            .count = 1, .stride = RDNA4_AS_TOKEN_BYTES,
        };
        rdna4_argument_scan_reflection_plan plan = {0};
        buildPlan(&plan, &field, 1);
        RDNA4StageState state = {0};
        state.scanPlan = &plan;
        state.buf[3].b = root;
        RDNA4ArgumentScanPrepared scan = {0};
        prepareScan(&scan, &state, writable, device);
        for (unsigned round = 0; round < 2; ++round) {
            writeToken(shared, 40, round ? updatedToken : selectedToken);
            NSMutableSet *samplers = [NSMutableSet set];
            NSMutableArray *writes = [NSMutableArray array];
            NSString *why = nil;
            uint64_t started = rdna4_now();
            require(rdna4_argument_sampler_scope(device, &scan, declared, writable,
                        samplers, writes, &why), why.UTF8String ?: "declared child scope");
            uint64_t elapsed = rdna4_now() - started;
            require(elapsed < UINT64_C(5) * 1000000000, "declared child scope timeout");
            NSSet *expected = variant == 3 ? [NSSet set] :
                [NSSet setWithArray:round ? @[selected, updated, privateOwner] :
                                               @[selected, privateOwner]];
            requireOnlySamplers(samplers, expected,
                                "declared child scope lost an owner or admitted a decoy");
            require(writes.count == (variant == 3 ? 0 : 2),
                    "declared child writable transport ranges");
            for (NSDictionary *write in writes) {
                NSArray<NSValue *> *ranges = write[@"ranges"];
                require(ranges.count == 1 && ranges[0].rangeValue.location == 0 &&
                        ranges[0].rangeValue.length == 128,
                        "declared child write range is not the full transport");
            }
            require(![samplers containsObject:unrelated.firstObject] &&
                    ![samplers containsObject:decoy],
                    "undeclared, aliasable, foreign or Private bytes entered scope");
            printf("DECLARED_CHILD_SCOPE variant=%u round=%u mapped_bytes=%u "
                   "private_metadata_only=1 writes=%lu elapsed_ns=%" PRIu64 "\n",
                   variant, round, variant == 3 ? 0 : 256,
                   (unsigned long)writes.count, elapsed);
        }
        if (variant == 0) {
            // Exercise the production declaration/heap expansion call site too.
            RDNA4ScanTestHeap *heap = [RDNA4ScanTestHeap alloc];
            heap->children = @[shared, alias];
            RDNA4ScanTestCommandBuffer *command = [RDNA4ScanTestCommandBuffer alloc];
            command->testDevice = device;
            command->capturedSamplers = [NSMutableSet set];
            RDNA4ArgumentTables tables = {0};
            require([command rdna4PrepareArguments:&tables
                        resources:[NSSet setWithObject:heap]
                         writable:[NSSet setWithObject:shared] scan:&scan textures:nil],
                    "production heap child declaration scope");
            requireOnlySamplers(command->capturedSamplers,
                [NSSet setWithObject:updated], "heap child raw token was not forwarded");
        }
        rdna4_argument_scan_prepared_destroy(&scan);
        rdna4_argument_scan_reflection_plan_destroy(&plan);
    }
}

static void testDeclaredOpaqueRoots(void) {
    NSDictionary *rootBinding = @{
        @"kind": @"Buffer", @"metal_index": @3, @"param_index": @0,
        @"descriptor": @{ @"set": @0, @"binding": @3, @"count": @1 }
    };
    NSDictionary *ordinaryBinding = @{
        @"kind": @"Buffer", @"metal_index": @0, @"param_index": @1,
        @"descriptor": @{ @"set": @0, @"binding": @0, @"count": @1 }
    };
    NSMutableDictionary *metadata = [@{
        @"trimmed_from_reflection_version": @66,
        @"argument_buffer_roots": @[@3],
        @"bindings": @[rootBinding, ordinaryBinding]
    } mutableCopy];
    rdna4_argument_scan_reflection_plan plan = {0};
    NSError *error = nil;
    require(rdna4_argument_scan_plan_build(&plan, metadata, metadata, "opaque-root", &error) && !error,
            "declared opaque root plan");
    require(plan.plan.count == 1 && plan.plan.spans[0].root == 3 &&
            (plan.plan.spans[0].flags & RDNA4_AS_FALLBACK),
            "opaque root lost or ordinary buffer scanned");
    rdna4_argument_scan_reflection_plan_destroy(&plan);
    NSDictionary *sampler = @{
        @"kind": @"EmbeddedArgBufferSampler", @"metal_index": @0,
        @"descriptor": @{ @"set": @0, @"binding": @160, @"count": @1 },
        @"embedded_source": @{ @"buffer_index": @3, @"field_offset": @16 }
    };
    metadata[@"bindings"] = @[rootBinding, ordinaryBinding, sampler];
    require(rdna4_argument_scan_plan_build(&plan, metadata, metadata, "typed-root", &error),
            "typed declared root plan");
    require(plan.plan.count == 1 && plan.plan.spans[0].root == 3 &&
            !(plan.plan.spans[0].flags & RDNA4_AS_FALLBACK),
            "declared root widened a typed sparse scan");
    rdna4_argument_scan_reflection_plan_destroy(&plan);
    for (id invalid in @[@[@3, @3], @[@32], @[@7], @[@(-1)], @[@3.0], @[@YES], @"3"]) {
        metadata[@"argument_buffer_roots"] = invalid;
        error = nil;
        require(!rdna4_argument_scan_plan_build(&plan, metadata, nil, "invalid-root", &error) && error,
                "malformed declared root accepted");
    }
    metadata[@"argument_buffer_roots"] = @[@3];
    metadata[@"trimmed_from_reflection_version"] = @65;
    error = nil;
    require(!rdna4_argument_scan_plan_build(&plan, metadata, nil, "old-version", &error) && error,
            "new root schema accepted under old reflection");
}

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        if (argc == 2 && !strcmp(argv[1], "--help")) {
            puts("Usage: test-argument-scan-runtime\n"
                 "Runs the production Device scan/scope path with mock Metal "
                 "buffers; no VM, Vulkan device, or GPU execution.");
            return 0;
        }
        if (argc != 1) return 2;

        testDeclaredOpaqueRoots();
        testSharedRechecksRawContents();
        testManagedRechecksRawContents();
        testLargeSparseTypedAndUnreflectedRoot();
        testAliasableOwnerMetadataWithoutRawRead();
        testDeclaredUnboundChildren();

        require(sharedRounds == 2, "Shared recheck rounds");
        require(managedRounds == 2, "Managed recheck rounds");
        require(fallbackWorks == 0, "runtime fixture used fallback work");
        printf("ARGUMENT_SCAN_RUNTIME_PASS checks=%u scopes=%u "
               "shared_rounds=%u managed_rounds=%u cpu_tokens=%" PRIu64
               " exact_ranges=%" PRIu64 " touched_bytes=%" PRIu64
               " max_scope_ns=%" PRIu64 " fallback_works=%" PRIu64
               " large_sparse_tokens=2 unreflected_large_root=0 "
               "aliasable_raw_read=0 no_all_device_fallback=1 declared_child_cases=8 "
               "gpu_execution=0\n",
               checks, scopes, sharedRounds, managedRounds, cpuTokens,
               exactRanges, touchedBytes, maximumScopeNs, fallbackWorks);
    }
    return 0;
}
