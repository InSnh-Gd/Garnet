// Runtime reflection through Metal's real source compiler and native pipelines.
// Link with Metal/Foundation and the test-only librdna4featureaudit; no GPU work.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <unistd.h>

extern unsigned rdna4_audit_submission_count(void);

@interface RDNA4UnreflectedFunction : NSObject
@property(nonatomic, copy) NSData *bitcodeData;
@property(nonatomic, copy) NSString *name;
@property(nonatomic) MTLFunctionType functionType;
@end
@implementation RDNA4UnreflectedFunction
@end

static void fail(NSString *message) __attribute__((noreturn));
static void fail(NSString *message) {
    fprintf(stderr, "REFLECTION_FAIL %s\n", message.UTF8String);
    _exit(1);
}

static id<MTLBinding> binding(NSArray<id<MTLBinding>> *bindings, MTLBindingType type, NSUInteger index, NSString *name) {
    for (id<MTLBinding> value in bindings) if (value.type == type && value.index == index) {
        if (![value.name isEqual:name] || !value.used) fail([NSString stringWithFormat:@"binding %@ inactive or wrong name: %@", name, value]);
        return value;
    }
    fail([NSString stringWithFormat:@"missing binding %@ at %lu", name, (unsigned long)index]);
}

static void checkParams(NSArray<id<MTLBinding>> *bindings, NSUInteger index) {
    id<MTLBufferBinding> params = (id)binding(bindings, MTLBindingTypeBuffer, index, @"params");
    MTLStructType *structure = params.bufferStructType;
    MTLPointerType *pointer = params.bufferPointerType;
    MTLStructMember *color = [structure memberByName:@"color"], *count = [structure memberByName:@"count"];
    if (params.access != MTLBindingAccessReadOnly || params.bufferDataSize != 32 || !params.bufferAlignment ||
        params.bufferAlignment != 16 || !pointer || pointer.alignment != 16 ||
        params.bufferDataType != MTLDataTypeStruct || structure.members.count != 2 || !color || !count ||
        color.offset != 0 || color.dataType != MTLDataTypeFloat4 || count.offset != 16 || count.dataType != MTLDataTypeUInt)
        fail([NSString stringWithFormat:@"constant-buffer layout/access/alignment mismatch: binding %lu pointer %lu",
            (unsigned long)params.bufferAlignment, (unsigned long)(pointer ? pointer.alignment : 0)]);
}

static void checkCompute(id<MTLComputePipelineState> pipeline, MTLComputePipelineReflection *reflection, NSError *error, BOOL reference) {
    if (!pipeline || !reflection || error) fail([NSString stringWithFormat:@"compute pipeline=%@ reflection=%@ error=%@", pipeline, reflection, error]);
    if (reflection.bindings.count != 4 || (!reference && pipeline.reflection != reflection)) fail(@"compute reflection missing bindings or pipeline property");
    id<MTLBufferBinding> values = (id)binding(reflection.bindings, MTLBindingTypeBuffer, 0, @"values");
    id<MTLTextureBinding> image = (id)binding(reflection.bindings, MTLBindingTypeTexture, 2, @"image");
    id<MTLBinding> sampling = binding(reflection.bindings, MTLBindingTypeSampler, 1, @"sampling");
    if (values.access != MTLBindingAccessReadWrite || values.bufferDataSize != 4 || values.bufferDataType != MTLDataTypeUInt ||
        image.textureType != MTLTextureType2D || image.textureDataType != MTLDataTypeFloat || image.access != MTLBindingAccessReadOnly ||
        sampling.access != MTLBindingAccessReadOnly) fail(@"compute resource metadata mismatch");
    checkParams(reflection.bindings, 3);
}

static void checkRender(id<MTLRenderPipelineState> pipeline, MTLRenderPipelineReflection *reflection, NSError *error, BOOL reference) {
    if (!pipeline || !reflection || error) fail([NSString stringWithFormat:@"render pipeline=%@ reflection=%@ error=%@", pipeline, reflection, error]);
    if (reflection.vertexBindings.count != 1 || reflection.fragmentBindings.count != 2 ||
        (!reference && pipeline.reflection != reflection)) fail(@"render reflection stage separation or pipeline property mismatch");
    checkParams(reflection.vertexBindings, 1);
    id<MTLTextureBinding> image = (id)binding(reflection.fragmentBindings, MTLBindingTypeTexture, 4, @"image");
    binding(reflection.fragmentBindings, MTLBindingTypeSampler, 2, @"sampling");
    if (image.textureType != MTLTextureType2D || image.textureDataType != MTLDataTypeFloat) fail(@"fragment texture metadata mismatch");
}

static void checkLayouts(id<MTLDevice> device, NSString *directory) {
    NSString *source = @"#include <metal_stdlib>\nusing namespace metal;\n"
        "struct Resources {device float *values [[id(0)]]; texture2d<float> tex [[id(1)]]; sampler smp [[id(2)]];};\n"
        "struct Params {float4 color; uint count;};\n"
        "struct Inner {float2 uv; uint tag;};\n"
        "struct Outer {float3x3 matrix; Inner nested; float gain;};\n"
        "kernel void resources24(device float *out [[buffer(0)]], constant Resources &params [[buffer(1)]]) {out[0]=params.values[0]+params.tex.sample(params.smp,float2(0.5)).x;}\n"
        "kernel void params32(device float *out [[buffer(0)]], constant Params &params [[buffer(1)]]) {out[0]=params.color.x+params.count;}\n"
        "kernel void matrix48(device float *out [[buffer(0)]], constant float3x3 &params [[buffer(1)]]) {out[0]=params[0][0]+params[2][2];}\n"
        "kernel void nested80(device float *out [[buffer(0)]], constant Outer &params [[buffer(1)]]) {out[0]=params.matrix[0][0]+params.nested.uv.x+params.nested.tag+params.gain;}\n";
    NSError *error = nil;
    MTLCompileOptions *compile = [MTLCompileOptions new];
    compile.languageVersion = MTLLanguageVersion2_4;
    id<MTLLibrary> library = [device newLibraryWithSource:source options:compile error:&error];
    if (!library || error) fail([NSString stringWithFormat:@"layout source: %@", error]);
    if (directory) {
        if (![[NSFileManager defaultManager] createDirectoryAtPath:directory withIntermediateDirectories:YES attributes:nil error:&error] ||
            ![source writeToFile:[directory stringByAppendingPathComponent:@"layouts.metal"] atomically:YES encoding:NSUTF8StringEncoding error:&error])
            fail(error.description);
    }
    NSArray *names = @[@"resources24", @"params32", @"matrix48", @"nested80"];
    const NSUInteger sizes[] = {24, 32, 48, 80};
    for (NSUInteger i = 0; i < names.count; ++i) {
        id<MTLFunction> function = [library newFunctionWithName:names[i]];
        MTLComputePipelineReflection *reflection = nil;
        id pipeline = [device newComputePipelineStateWithFunction:function options:3 reflection:&reflection error:&error];
        if (!pipeline || !reflection || error) fail([NSString stringWithFormat:@"layout %@: %@", names[i], error]);
        id<MTLBufferBinding> params = (id)binding(reflection.bindings, MTLBindingTypeBuffer, 1, @"params");
        NSUInteger minimum = ((NSUInteger (*)(id, SEL))objc_msgSend)(device, sel_registerName("minConstantBufferAlignmentBytes"));
        NSUInteger alignment = MAX(i ? 16u : 8u, minimum);
        if (params.bufferDataSize != sizes[i] || params.bufferAlignment != alignment ||
            params.bufferPointerType.alignment != alignment || params.bufferPointerType.elementIsArgumentBuffer != (i == 0))
            fail([NSString stringWithFormat:@"layout %@ size=%lu align=%lu pointer=%lu minimum=%lu", names[i],
                (unsigned long)params.bufferDataSize, (unsigned long)params.bufferAlignment,
                (unsigned long)params.bufferPointerType.alignment, (unsigned long)minimum]);
        MTLStructType *structure = params.bufferStructType;
        if (i == 0) {
            if (structure.members.count != 3 || [structure memberByName:@"values"].offset != 0 ||
                [structure memberByName:@"tex"].offset != 8 || [structure memberByName:@"smp"].offset != 16)
                fail(@"24-byte resource member offsets");
        } else if (i == 1) checkParams(reflection.bindings, 1);
        else if (i == 2 && params.bufferDataType != MTLDataTypeFloat3x3) fail(@"matrix type");
        else if (i == 3) {
            MTLStructType *nested = [structure memberByName:@"nested"].structType;
            if (structure.members.count != 3 || [structure memberByName:@"matrix"].offset != 0 ||
                [structure memberByName:@"nested"].offset != 48 || [structure memberByName:@"gain"].offset != 64 ||
                nested.members.count != 2 || [nested memberByName:@"uv"].offset != 0 || [nested memberByName:@"tag"].offset != 8)
                fail(@"nested structure member offsets");
        }
        if (directory) for (NSString *selector in @[@"bitcodeData", @"reflectionData"]) {
            NSData *data = [function respondsToSelector:NSSelectorFromString(selector)] ?
                ((id (*)(id, SEL))objc_msgSend)(function, NSSelectorFromString(selector)) : nil;
            NSString *path = [directory stringByAppendingPathComponent:[names[i] stringByAppendingFormat:@".%@", selector]];
            if (!data.length || ![data writeToFile:path options:NSDataWritingAtomic error:&error]) fail(@"layout AIR/reflection capture");
        }
        printf("REFLECTION_LAYOUT_PASS case=%s bytes=%lu alignment=%lu minimum=%lu\n", [names[i] UTF8String],
            (unsigned long)params.bufferDataSize, (unsigned long)alignment, (unsigned long)minimum);
    }
}

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        setvbuf(stdout, NULL, _IONBF, 0);
        if (argc == 2 && !strcmp(argv[1], "--help")) {
            puts("Usage: test-pipeline-reflection BUNDLE|--reference [LAYOUT_ARTIFACTS]\nTests shader binding reflection, without command submission.");
            return 0;
        }
        if (argc != 2 && argc != 3) return 2;
        BOOL reference = !strcmp(argv[1], "--reference");
        if (!reference && (argv[1][0] == '-' || geteuid() != 0)) return 2;
        alarm(45);
        setenv("RDNA4_AUDIT_SUBMIT", "reject", 1);
        id<MTLDevice> device = nil;
        if (reference) device = MTLCreateSystemDefaultDevice();
        else {
            setenv("RADV_DARWIN_MOCK", "1", 1);
            setenv("RADV_DARWIN_MOCK_GC", "gfx1200", 1);
            setenv("RADV_DEBUG", "nocache", 1);
            setenv("MESA_SHADER_CACHE_DISABLE", "true", 1);
            setenv("RDNA4M_ALLOW", "1", 1);
            setenv("RDNA4M_TEST_INPROC", "1", 1);
            NSError *loadError = nil;
            if (![[NSBundle bundleWithPath:@(argv[1])] loadAndReturnError:&loadError]) fail(loadError.description);
            device = [[objc_getClass("GarnetDevice") alloc] init];
            ((void (*)(id, SEL))objc_msgSend)(device, sel_registerName("initLimits"));
        }
        if (!device) fail(@"Metal device unavailable");
        NSString *alignmentSelector = @"minConstantBufferAlignmentBytes";
        SEL alignmentSel = NSSelectorFromString(alignmentSelector);
        if (![device respondsToSelector:alignmentSel])
            fail(@"device does not expose minConstantBufferAlignmentBytes");
        NSUInteger alignmentValue = ((NSUInteger (*)(id, SEL))objc_msgSend)(device, alignmentSel);
        if (alignmentValue < 4 || (alignmentValue & (alignmentValue - 1)))
            fail([NSString stringWithFormat:@"minConstantBufferAlignmentBytes is invalid: %lu", (unsigned long)alignmentValue]);
        printf("REFLECTION_CONSTANT_ALIGNMENT_GETTER_PASS value=%llu\n",
            (unsigned long long)alignmentValue);
        NSString *source = @"#include <metal_stdlib>\nusing namespace metal;\n"
            "struct Params { float4 color; uint count; };\n"
            "kernel void reflection_kernel(device uint *values [[buffer(0)]], constant Params &params [[buffer(3)]], "
            "texture2d<float, access::sample> image [[texture(2)]], sampler sampling [[sampler(1)]], uint i [[thread_position_in_grid]]) "
            "{ values[i] = uint(image.sample(sampling, float2(0.5)).x + params.color.x) + params.count; }\n"
            "vertex float4 reflection_vertex(uint i [[vertex_id]], constant Params &params [[buffer(1)]]) "
            "{ return float4(float(i), params.color.x, 0.0, 1.0); }\n"
            "fragment half4 reflection_fragment(texture2d<float> image [[texture(4)]], sampler sampling [[sampler(2)]]) "
            "{ return half4(image.sample(sampling, float2(0.5))); }\n";
        NSError *error = nil;
        id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
        id<MTLFunction> function = [library newFunctionWithName:@"reflection_kernel"];
        if (!function) fail([NSString stringWithFormat:@"source compiler: %@", error]);
        MTLPipelineOption options = MTLPipelineOptionBindingInfo | MTLPipelineOptionBufferTypeInfo;
        MTLComputePipelineReflection *reflection = nil;
        id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:function options:options reflection:&reflection error:&error];
        checkCompute(pipeline, reflection, error, reference);
        puts("REFLECTION_COMPUTE_FUNCTION_PASS");
        MTLComputePipelineDescriptor *compute = [MTLComputePipelineDescriptor new];
        compute.computeFunction = function;
        pipeline = [device newComputePipelineStateWithDescriptor:compute options:options reflection:&reflection error:&error];
        checkCompute(pipeline, reflection, error, reference);
        puts("REFLECTION_COMPUTE_DESCRIPTOR_PASS");
        for (BOOL descriptor = NO; ; descriptor = YES) {
            dispatch_semaphore_t completed = dispatch_semaphore_create(0);
            __block id<MTLComputePipelineState> result = nil;
            __block MTLComputePipelineReflection *info = nil;
            __block NSError *failure = nil;
            void (^handler)(id<MTLComputePipelineState>, MTLComputePipelineReflection *, NSError *) =
                ^(id<MTLComputePipelineState> p, MTLComputePipelineReflection *r, NSError *e) {
                    result = p; info = r; failure = e; dispatch_semaphore_signal(completed);
                };
            if (descriptor) [device newComputePipelineStateWithDescriptor:compute options:options completionHandler:handler];
            else [device newComputePipelineStateWithFunction:function options:options completionHandler:handler];
            if (dispatch_semaphore_wait(completed, dispatch_time(DISPATCH_TIME_NOW, 15 * NSEC_PER_SEC))) fail(@"compute callback deadline");
            checkCompute(result, info, failure, reference);
            printf("REFLECTION_COMPUTE_ASYNC_PASS descriptor=%d\n", descriptor);
            if (descriptor) break;
        }
        // Apple's driver may leave the output untouched when no reflection is requested.
        if (reference) reflection = nil;
        pipeline = [device newComputePipelineStateWithFunction:function options:MTLPipelineOptionNone reflection:&reflection error:&error];
        if (!pipeline || reflection || error) fail(@"compute options=None did not clear reflection");
        puts("REFLECTION_COMPUTE_NONE_PASS");
        MTLRenderPipelineDescriptor *render = [MTLRenderPipelineDescriptor new];
        render.vertexFunction = [library newFunctionWithName:@"reflection_vertex"];
        render.fragmentFunction = [library newFunctionWithName:@"reflection_fragment"];
        render.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        MTLRenderPipelineReflection *renderInfo = nil;
        id<MTLRenderPipelineState> renderPipeline = [device newRenderPipelineStateWithDescriptor:render options:options reflection:&renderInfo error:&error];
        checkRender(renderPipeline, renderInfo, error, reference);
        puts("REFLECTION_RENDER_PASS");
        puts("REFLECTION_RENDER_CONSTANT_ALIGNMENT_PASS");
        dispatch_semaphore_t completed = dispatch_semaphore_create(0);
        __block id<MTLRenderPipelineState> asyncRender = nil;
        __block MTLRenderPipelineReflection *asyncInfo = nil;
        __block NSError *asyncError = nil;
        [device newRenderPipelineStateWithDescriptor:render options:options
            completionHandler:^(id<MTLRenderPipelineState> p, MTLRenderPipelineReflection *r, NSError *e) {
                asyncRender = p; asyncInfo = r; asyncError = e; dispatch_semaphore_signal(completed);
            }];
        if (dispatch_semaphore_wait(completed, dispatch_time(DISPATCH_TIME_NOW, 15 * NSEC_PER_SEC))) fail(@"render callback deadline");
        checkRender(asyncRender, asyncInfo, asyncError, reference);
        puts("REFLECTION_RENDER_ASYNC_PASS");
        if (reference) renderInfo = nil;
        renderPipeline = [device newRenderPipelineStateWithDescriptor:render options:MTLPipelineOptionNone reflection:&renderInfo error:&error];
        if (!renderPipeline || renderInfo || error) fail(@"render options=None did not clear reflection");
        puts("REFLECTION_RENDER_NONE_PASS");
        if (!reference) {
            pipeline = [device newComputePipelineStateWithFunction:function options:options reflection:NULL error:&error];
            if (!pipeline || !pipeline.reflection || error) fail(@"compute nil output pointer failed");
            renderPipeline = [device newRenderPipelineStateWithDescriptor:render options:options reflection:NULL error:&error];
            if (!renderPipeline || !renderPipeline.reflection || error) fail(@"render nil output pointer failed");
            puts("REFLECTION_NULL_OUTPUT_PASS");
            RDNA4UnreflectedFunction *missing = [RDNA4UnreflectedFunction new];
            missing.name = function.name;
            missing.functionType = function.functionType;
            missing.bitcodeData = ((id (*)(id, SEL))objc_msgSend)(function, sel_registerName("bitcodeData"));
            reflection = pipeline.reflection;
            pipeline = [device newComputePipelineStateWithFunction:(id)missing options:options reflection:&reflection error:&error];
            if (pipeline || reflection || !error) fail(@"compute missing reflection did not fail and clear output");
            error = nil;
            id<MTLFunction> fragment = render.fragmentFunction;
            missing.name = fragment.name;
            missing.functionType = fragment.functionType;
            missing.bitcodeData = ((id (*)(id, SEL))objc_msgSend)(fragment, sel_registerName("bitcodeData"));
            render.fragmentFunction = (id)missing;
            renderInfo = renderPipeline.reflection;
            renderPipeline = [device newRenderPipelineStateWithDescriptor:render options:options reflection:&renderInfo error:&error];
            if (renderPipeline || renderInfo || !error) fail(@"render missing reflection did not fail and clear output");
            puts("REFLECTION_MISSING_DATA_PASS");
        }
        checkLayouts(device, argc == 3 ? @(argv[2]) : nil);
        if (rdna4_audit_submission_count()) fail(@"unexpected GPU submission");
        puts("REFLECTION_PASS no submissions");
        _exit(0);
    }
}
