// garnet_arguments_internal.h - Shared internal declarations across GarnetMetal's
// argument-buffer and argument-summary compilation units (garnet_arguments.m,
// garnet_argument_encoder.m, garnet_argument_summary.m, garnet_argument_summary_owners.m).
#ifndef GARNET_ARGUMENTS_INTERNAL_H
#define GARNET_ARGUMENTS_INTERNAL_H

#if !defined(RDNA4_ARGUMENT_SUMMARY_RUNTIME_TEST) && !defined(RDNA4_ARGUMENT_SUMMARY_OWNERS_TEST)

#ifndef GARNET_INTERNAL_EXTERN
#define GARNET_INTERNAL_EXTERN static
#endif

@interface RDNA4ArgumentPool : NSObject {
@public
    VkDescriptorPool handle;
@private
    uint64_t _descriptorCount;
}
- (instancetype)initWithInfo:(const VkDescriptorPoolCreateInfo *)info error:(NSError **)error;
@end

@interface RDNA4ICBArgOwners : NSObject
@property NSMutableSet *icbs;
@property NSMutableSet *pipelines;
@property NSMutableSet *nestedBuffers;
@end

@interface RDNA4ReflectionCompiler : NSObject
@end

@interface RDNA4ArgumentField : NSObject
@property NSUInteger offset, size;
@property MTLDataType type;
@property RDNA4ArgumentLayout *nested;
@end

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
@property BOOL bindlessQueried;
@end

@interface RDNA4ArgumentResourceToken : NSObject
@property (nonatomic, weak) id object;     // texture; sampler wrappers live in aliases
@property (nonatomic, weak) id identity;   // object or the sampler storage that owns the token
@property (nonatomic, weak) id device;
@property BOOL texture;
@property NSMutableArray<RDNA4ArgumentSamplerAlias *> *aliases;
@end

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

@interface RDNA4ArgumentEncoder (GarnetInternalMethods)
- (id)device;
- (NSString *)label;
- (void)setLabel:(NSString *)label;
- (NSUInteger)encodedLength;
- (NSUInteger)alignment;
- (void)setArgumentBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset;
- (void)setArgumentBuffer:(id<MTLBuffer>)buffer startOffset:(NSUInteger)startOffset arrayElement:(NSUInteger)element;
- (void *)rdna4Destination:(NSUInteger)index type:(MTLDataType)type;
- (uint64_t)rdna4BufferAddress:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index;
- (void)setBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index;
- (void)setBuffers:(const id<MTLBuffer> __unsafe_unretained *)buffers offsets:(const NSUInteger *)offsets withRange:(NSRange)range;
- (uint64_t)rdna4TextureToken:(id<MTLTexture>)texture;
- (void)setTexture:(id<MTLTexture>)texture atIndex:(NSUInteger)index;
- (uint64_t)rdna4SamplerToken:(id<MTLSamplerState>)sampler;
- (void)setSamplerState:(id<MTLSamplerState>)sampler atIndex:(NSUInteger)index;
- (void)setTextures:(const id<MTLTexture> __unsafe_unretained *)textures withRange:(NSRange)range;
- (void)setSamplerStates:(const id<MTLSamplerState> __unsafe_unretained *)samplers withRange:(NSRange)range;
- (uint64_t)rdna4IndirectCommandBufferAddress:(id<MTLIndirectCommandBuffer>)icb;
- (void)setIndirectCommandBuffer:(id<MTLIndirectCommandBuffer>)indirectCommandBuffer atIndex:(NSUInteger)index;
- (void)setIndirectCommandBuffers:(const id<MTLIndirectCommandBuffer> __unsafe_unretained *)buffers withRange:(NSRange)range;
- (uint64_t)rdna4RenderPipelineToken:(id<MTLRenderPipelineState>)pipeline;
- (void)setRenderPipelineState:(id<MTLRenderPipelineState>)pipeline atIndex:(NSUInteger)index;
- (void)setRenderPipelineStates:(const id<MTLRenderPipelineState> __unsafe_unretained *)pipelines withRange:(NSRange)range;
- (uint64_t)rdna4ComputePipelineToken:(id<MTLComputePipelineState>)pipeline;
- (void)setComputePipelineState:(id<MTLComputePipelineState>)pipeline atIndex:(NSUInteger)index;
- (void)setComputePipelineStates:(const id<MTLComputePipelineState> __unsafe_unretained *)pipelines withRange:(NSRange)range;
- (void)setAccelerationStructure:(id<MTLAccelerationStructure>)accelerationStructure atIndex:(NSUInteger)index;
- (void)setIntersectionFunctionTable:(id<MTLIntersectionFunctionTable>)intersectionFunctionTable atIndex:(NSUInteger)index;
- (void)setIntersectionFunctionTables:(const id<MTLIntersectionFunctionTable> __unsafe_unretained *)intersectionFunctionTables withRange:(NSRange)range;
- (void *)constantDataAtIndex:(NSUInteger)index;
- (id)newArgumentEncoderForBufferAtIndex:(NSUInteger)index;
@end

@interface RDNA4ArgumentLayout (GarnetInternalMethods)
- (id)device;
- (BOOL)bufferLayoutMatchesFrontEndLayout;
- (void)setStructType:(MTLStructType *)type withDevice:(id)device;
- (NSUInteger)constantOffsetAtIndex:(NSUInteger)index;
- (id)bufferLayoutForResourceAtIndex:(NSUInteger)index;
@end

@interface RDNA4ArgumentResourceToken (GarnetInternalMethods)
- (instancetype)init;
@end

@interface RDNA4ArgumentSamplerOwners (GarnetInternalMethods)
- (instancetype)init;
@end

@interface RDNA4ICBArgOwners (GarnetInternalMethods)
- (instancetype)init;
@end

@interface RDNA4ReflectionCompiler (GarnetInternalMethods)
- (NSUInteger)compilerFlags;
- (void)reflectionWithFunction:(id)function options:(NSUInteger)options sync:(BOOL)sync binaryArchives:(id)archives completionHandler:(void (^)(NSUInteger, dispatch_data_t, NSString *))completion;
- (void)reflectionWithFunction:(id)function options:(NSUInteger)options sync:(BOOL)sync pipelineLibrary:(id)library completionHandler:(void (^)(NSUInteger, dispatch_data_t, NSString *))completion;
- (void)reflectionWithFunction:(id)function options:(NSUInteger)options sync:(BOOL)sync completionHandler:(void (^)(NSUInteger, dispatch_data_t, NSString *))completion;
@end

GARNET_INTERNAL_EXTERN VkShaderStageFlags rdna4_argument_stage_flags(uint32_t stages,
                                                     BOOL compute);
GARNET_INTERNAL_EXTERN BOOL rdna4_argument_shape(unsigned shape, VkImageViewType *viewType, BOOL *depth);
GARNET_INTERNAL_EXTERN BOOL rdna4_argument_reflection_version(NSDictionary *metadata,
                                              BOOL *present,
                                              uint64_t *version);
GARNET_INTERNAL_EXTERN BOOL rdna4_argument_normalized_metadata(NSDictionary *metadata,
                                                BOOL *normalized,
                                                NSError **error);
GARNET_INTERNAL_EXTERN BOOL rdna4_argument_table_uses_shape_registry(NSDictionary *metadata);
GARNET_INTERNAL_EXTERN BOOL rdna4_argument_table_stages_compatible(NSDictionary *a,
                                                    NSDictionary *b);
GARNET_INTERNAL_EXTERN NSData *rdna4_gpu_argument_spv(NSData *spv, NSDictionary **metadata, uint64_t deadline, NSError **error);
GARNET_INTERNAL_EXTERN BOOL rdna4_argument_table_metadata(
    const RDNA4sRefl *reflection, int stageSet, NSDictionary *metadata,
    NSError **error);
GARNET_INTERNAL_EXTERN BOOL rdna4_argument_table_metadata_apply(
    RDNA4ArgumentTables *tables, const RDNA4sRefl *reflection,
    int stageSet, NSDictionary *metadata, NSError **error);
GARNET_INTERNAL_EXTERN BOOL rdna4_argument_descriptor_validate(
    const rdna4_argument_descriptor_usage *usage, BOOL uab, NSError **error);
GARNET_INTERNAL_EXTERN BOOL rdna4_argument_table_layouts(RDNA4ArgumentTables *tables, const RDNA4sRefl *vertex,
    const RDNA4sRefl *fragment, BOOL compute, NSError **error);
GARNET_INTERNAL_EXTERN void rdna4_argument_tables_destroy(RDNA4ArgumentTables *tables);
GARNET_INTERNAL_EXTERN BOOL rdna4_argument_stage_remap(const RDNA4ArgumentTables *tables,
    RDNA4PB *bindings, uint32_t count, NSError **error);
GARNET_INTERNAL_EXTERN void rdna4_argbuf_icb_snapshot(RDNA4Buffer *buf, NSMutableSet *icbs, NSMutableSet *psos);
GARNET_INTERNAL_EXTERN BOOL rdna4_argument_scalar(MTLDataType type, NSUInteger *size, NSUInteger *alignment);
GARNET_INTERNAL_EXTERN id rdna4_argument_resource_for_token(uint64_t token, BOOL texture, id device);
GARNET_INTERNAL_EXTERN NSArray *rdna4_argument_resource_snapshot(id device, NSSet *declared, NSSet *samplers);
GARNET_INTERNAL_EXTERN BOOL rdna4_argument_sampler_intent_matches(id token, id buffer, NSRange range,
                                                   BOOL fillOnly);
GARNET_INTERNAL_EXTERN BOOL rdna4_argument_sampler_intent_matches_ranges(
        id token, id buffer, NSRange range, NSArray<NSValue *> *writeRanges,
        BOOL fillOnly);
GARNET_INTERNAL_EXTERN void rdna4_argument_sampler_store(id buffer, NSUInteger offset, uint64_t value, id sampler);
GARNET_INTERNAL_EXTERN void rdna4_argument_sampler_write(id buffer, NSUInteger offset, id sampler, uint64_t token);
GARNET_INTERNAL_EXTERN BOOL rdna4_argument_sampler_scope(
        id device, const RDNA4ArgumentScanPrepared *scan,
        NSSet *declaredBuffers, NSSet *writable,
        NSMutableSet *samplers, NSMutableArray<NSDictionary *> *writes,
        NSString **why);
GARNET_INTERNAL_EXTERN RDNA4ArgumentWriteIntent *rdna4_argument_sampler_intent_ranges(id buffer, NSRange range,
        NSArray<NSValue *> *writeRanges, NSSet *candidates, BOOL fillOnly);
GARNET_INTERNAL_EXTERN RDNA4ArgumentWriteIntent *rdna4_argument_sampler_intent(id buffer, NSRange range,
        NSSet *candidates, BOOL fillOnly);
GARNET_INTERNAL_EXTERN void rdna4_argument_sampler_add_candidates(id token, NSSet *candidates);
GARNET_INTERNAL_EXTERN RDNA4ArgumentWriteIntent *rdna4_argument_sampler_copy(id source, NSUInteger sourceOffset,
        id destination, NSUInteger offset, NSUInteger size);
GARNET_INTERNAL_EXTERN uint64_t rdna4_argument_next_submission_epoch(void);
GARNET_INTERNAL_EXTERN void rdna4_argument_sampler_submit(id token, uint64_t submissionEpoch);
GARNET_INTERNAL_EXTERN void rdna4_argument_sampler_abandon(id token);
GARNET_INTERNAL_EXTERN void rdna4_argument_sampler_complete(id token);
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
GARNET_INTERNAL_EXTERN BOOL rdna4_argument_reflection(id<MTLFunction> function, NSArray<MTLArgument *> *arguments);
#pragma clang diagnostic pop
GARNET_INTERNAL_EXTERN BOOL rdna4_argument_summary_owners_needed(NSArray *intents);
GARNET_INTERNAL_EXTERN BOOL rdna4_argument_summary_owners_finish(
    id attempt, BOOL fenceSucceeded, uint64_t submissionEpoch,
    NSError **error);
GARNET_INTERNAL_EXTERN BOOL rdna4_argument_summary_owners_encode(
    NSArray<RDNA4ArgumentWriteIntent *> *intents,
    VkCommandBuffer command,
    NSArray * __strong *outAttempts,
    NSError **error);

typedef struct {
    BOOL loaded;
    void *dictRetained;
    const void **objects;
    rdna4_as_live_entry *entries;
    rdna4_as_live_table table;
} RDNA4ArgumentLiveContext;

@interface RDNA4ArgumentSummaryTask : NSObject {
@public
    rdna4_argument_summary_runtime_plan _plan;
    RDNA4Buffer *_sourceBuffer;
    RDNA4Buffer *_dictionaryBuffer;
    RDNA4Buffer *_outputBuffer;
    uint64_t _sourceOffset;
    uint64_t _dictionaryOffset;
    uint64_t _outputOffset;
}
- (instancetype)initWithSourceBuffer:(RDNA4Buffer *)sourceBuffer
                          sourceOffset:(NSUInteger)sourceOffset
                            sourceSize:(NSUInteger)sourceSize
                    dictionaryBuffer:(RDNA4Buffer *)dictionaryBuffer
                    dictionaryOffset:(NSUInteger)dictionaryOffset
                      candidateCount:(uint32_t)candidateCount
                        outputBuffer:(RDNA4Buffer *)outputBuffer
                        outputOffset:(NSUInteger)outputOffset
                          generation:(uint64_t)generation
                              limits:(const rdna4_argument_summary_runtime_limits *)limits
                  requestedChunkBytes:(uint32_t)requestedChunkBytes
             requestedDictionaryPage:(uint32_t)requestedDictionaryPage
                               error:(NSError **)error;
- (uint64_t)generation;
- (uint32_t)outputWordCount;
- (RDNA4Buffer *)sourceBuffer;
- (RDNA4Buffer *)dictionaryBuffer;
- (RDNA4Buffer *)outputBuffer;
- (uint64_t)sourceOffset;
- (uint64_t)dictionaryOffset;
- (uint64_t)outputOffset;
- (const rdna4_argument_summary_runtime_plan *)runtimePlan;
@end

@interface RDNA4ArgumentSummaryAttempt : NSObject {
@public
    RDNA4ArgumentSummaryTask *_task;
    rdna4_argument_summary_completion _completion;
    RDNA4Buffer *_sourceScratch;
    RDNA4Buffer *_dictionaryScratch;
    RDNA4Buffer *_outputScratch;
    VkDescriptorPool _descriptorPool;
    VkDescriptorSet _descriptorSet;
}
- (uint64_t)generation;
- (uint64_t)jobCount;
- (uint64_t)encodedJobs;
@end

GARNET_INTERNAL_EXTERN NSMutableDictionary<NSNumber *, RDNA4ArgumentResourceToken *> *rdna4_argument_tokens;
GARNET_INTERNAL_EXTERN NSLock *rdna4_argument_tokens_lock;
GARNET_INTERNAL_EXTERN void rdna4_argument_tokens_init(void);
GARNET_INTERNAL_EXTERN id rdna4_argument_token_identity(id object, BOOL texture);
GARNET_INTERNAL_EXTERN void rdna4_argument_token_prune_locked(void);
GARNET_INTERNAL_EXTERN RDNA4ArgumentSamplerOwners *rdna4_argument_sampler_owners(id buffer, BOOL create);
GARNET_INTERNAL_EXTERN void rdna4_argument_live_ctx_destroy(RDNA4ArgumentLiveContext *ctx);
GARNET_INTERNAL_EXTERN BOOL rdna4_argument_sampler_scope_work(
        id device, RDNA4Buffer *buffer, const rdna4_as_work *work,
        RDNA4ArgumentLiveContext *liveCtx, NSMutableSet *samplers,
        NSMutableArray<NSDictionary *> *writes, NSString **why);
GARNET_INTERNAL_EXTERN void rdna4_argument_sampler_add_possible_locked(RDNA4ArgumentSamplerOwners *owners,
        NSRange range, NSSet *candidates);

GARNET_INTERNAL_EXTERN RDNA4ArgumentSummaryTask *rdna4_argument_summary_task_new(
    RDNA4Buffer *sourceBuffer, NSUInteger sourceOffset,
    NSUInteger sourceSize, RDNA4Buffer *dictionaryBuffer,
    NSUInteger dictionaryOffset, uint32_t candidateCount,
    RDNA4Buffer *outputBuffer, NSUInteger outputOffset,
    uint64_t generation,
    const rdna4_argument_summary_runtime_limits *limits,
    uint32_t requestedChunkBytes, uint32_t requestedDictionaryPage,
    NSError **error);
GARNET_INTERNAL_EXTERN BOOL rdna4_argument_summary_encode(
    RDNA4ArgumentSummaryTask *task, VkCommandBuffer command,
    RDNA4ArgumentSummaryAttempt **outAttempt, NSError **error);
GARNET_INTERNAL_EXTERN rdna4_argument_summary_status
rdna4_argument_summary_attempt_complete(
    RDNA4ArgumentSummaryAttempt *attempt, BOOL fenceSucceeded,
    uint64_t currentGeneration);
GARNET_INTERNAL_EXTERN rdna4_argument_summary_status
rdna4_argument_summary_attempt_publish(
    RDNA4ArgumentSummaryAttempt *attempt, uint64_t currentGeneration,
    rdna4_argument_summary_publication *publication);

#endif /* !RDNA4_ARGUMENT_SUMMARY_RUNTIME_TEST && !RDNA4_ARGUMENT_SUMMARY_OWNERS_TEST */

#endif /* GARNET_ARGUMENTS_INTERNAL_H */
