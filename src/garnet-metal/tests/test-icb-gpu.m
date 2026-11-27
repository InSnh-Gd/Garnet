#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#import <objc/runtime.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void fail(NSString *message) __attribute__((noreturn));
static void fail(NSString *message) {
    fprintf(stderr, "ICB_GPU_FAIL %s\n", message.UTF8String);
    _exit(1);
}

static void passCase(NSString *name) {
    printf("ICB_GPU_CASE_PASS %s\n", name.UTF8String);
}

static uint16_t f32_to_f16(float f) {
    __fp16 h = (__fp16)f;
    uint16_t bits = 0;
    memcpy(&bits, &h, 2);
    return bits;
}

static uint64_t cpuReadbackCount(id<MTLIndirectCommandBuffer> icb) {
    SEL sel = sel_registerName("rdna4ICBCPUReadbackCount");
    if (![icb respondsToSelector:sel]) fail(@"ICB missing rdna4ICBCPUReadbackCount");
    return ((uint64_t (*)(id, SEL))objc_msgSend)(icb, sel);
}

static void assertZeroReadback(id<MTLIndirectCommandBuffer> icb, NSString *where) {
    uint64_t c = cpuReadbackCount(icb);
    if (c != 0)
        fail([NSString stringWithFormat:@"%@ triggered %llu CPU ICB readbacks", where, (unsigned long long)c]);
}

static void finish(id<MTLCommandBuffer> cb, NSString *op) {
    if (!cb) fail([NSString stringWithFormat:@"%@ command buffer nil", op]);
    [cb commit];
    [cb waitUntilCompleted];
    if (cb.status != MTLCommandBufferStatusCompleted || cb.error)
        fail([NSString stringWithFormat:@"%@ status=%lu error=%@", op, (unsigned long)cb.status, cb.error]);
}

static void expectCommandError(id<MTLCommandBuffer> cb, NSString *op) {
    if (!cb) fail([NSString stringWithFormat:@"%@ command buffer nil", op]);
    [cb commit];
    [cb waitUntilCompleted];
    if (cb.status != MTLCommandBufferStatusError || !cb.error)
        fail([NSString stringWithFormat:@"%@ did not fail cleanly (status=%lu)", op, (unsigned long)cb.status]);
}

static id<MTLTexture> newTarget(id<MTLDevice> device) {
    MTLTextureDescriptor *td =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                           width:32 height:32 mipmapped:NO];
    td.storageMode = MTLStorageModePrivate;
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    id<MTLTexture> tex = [device newTextureWithDescriptor:td];
    if (!tex) fail(@"render target nil");
    return tex;
}

static NSData *readTexture(id<MTLDevice> device, id<MTLCommandQueue> queue,
                           id<MTLTexture> tex, NSString *op) {
    NSUInteger len = tex.width * tex.height * 4;
    id<MTLBuffer> dst = [device newBufferWithLength:len options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit copyFromTexture:tex sourceSlice:0 sourceLevel:0
             sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake(tex.width, tex.height, 1)
                 toBuffer:dst destinationOffset:0
        destinationBytesPerRow:tex.width * 4
      destinationBytesPerImage:len];
    [blit endEncoding];
    finish(cb, [op stringByAppendingString:@" readback"]);
    return [NSData dataWithBytes:dst.contents length:len];
}

static void pixelAt(NSData *img, NSUInteger w, NSUInteger x, NSUInteger y,
                    uint8_t wantR, uint8_t wantG, uint8_t wantB, uint8_t wantA, NSString *op) {
    const uint8_t *p = img.bytes;
    NSUInteger off = (y * w + x) * 4;
    int got[4] = { p[off], p[off + 1], p[off + 2], p[off + 3] };
    int exp[4] = { wantR, wantG, wantB, wantA };
    for (int i = 0; i < 4; i++) {
        if (got[i] < exp[i] - 2 || got[i] > exp[i] + 2)
            fail([NSString stringWithFormat:@"%@ (%lu,%lu) ch%d got %d expected %d (rgba=%d,%d,%d,%d)",
                  op, (unsigned long)x, (unsigned long)y, i, got[i], exp[i],
                  got[0], got[1], got[2], got[3]]);
    }
}

static void expectSamePixels(NSData *a, NSData *b, NSString *op) {
    if (a.length != b.length) fail([NSString stringWithFormat:@"%@ size mismatch", op]);
    const uint8_t *pa = a.bytes, *pb = b.bytes;
    for (NSUInteger i = 0; i < a.length; i++) {
        int d = (int)pa[i] - (int)pb[i];
        if (d < -2 || d > 2)
            fail([NSString stringWithFormat:@"%@ byte %lu diff: direct=%u gpu_icb=%u",
                  op, (unsigned long)i, (unsigned)pa[i], (unsigned)pb[i]]);
    }
}

static id<MTLRenderCommandEncoder> beginRender(id<MTLCommandQueue> queue, id<MTLTexture> target,
                                                id<MTLCommandBuffer> *outCB) {
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = target;
    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
    [enc setViewport:(MTLViewport){ 0, 0, 32, 32, 0, 1 }];
    [enc setScissorRect:(MTLScissorRect){ 0, 0, 32, 32 }];
    *outCB = cb;
    return enc;
}

static id<MTLRenderPipelineState> makeRenderPSO(id<MTLDevice> dev, id<MTLLibrary> lib,
                                                NSString *vs, NSString *fs, BOOL hasAttr) {
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [lib newFunctionWithName:vs];
    d.fragmentFunction = [lib newFunctionWithName:fs];
    d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    d.supportIndirectCommandBuffers = YES;
    if (hasAttr) {
        MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
        vd.attributes[0].format = MTLVertexFormatFloat2;
        vd.attributes[0].offset = 0;
        vd.attributes[0].bufferIndex = 0;
        vd.layouts[0].stride = 2 * sizeof(float);
        vd.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;
        d.vertexDescriptor = vd;
    }
    NSError *err = nil;
    id<MTLRenderPipelineState> pso = [dev newRenderPipelineStateWithDescriptor:d error:&err];
    if (!pso) fail([NSString stringWithFormat:@"pso %@/%@: %@", vs, fs, err]);
    return pso;
}

static id<MTLRenderPipelineState> makeTessPSO(id<MTLDevice> dev, id<MTLLibrary> lib,
                                              NSString *vs, NSString *fs,
                                              MTLTessellationControlPointIndexType cpIdx,
                                              MTLTessellationFactorStepFunction fstep) {
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [lib newFunctionWithName:vs];
    d.fragmentFunction = [lib newFunctionWithName:fs];
    d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    d.tessellationPartitionMode = MTLTessellationPartitionModeInteger;
    d.tessellationOutputWindingOrder = MTLWindingClockwise;
    d.tessellationFactorFormat = MTLTessellationFactorFormatHalf;
    d.tessellationFactorStepFunction = fstep;
    d.tessellationControlPointIndexType = cpIdx;
    d.tessellationFactorScaleEnabled = YES;
    d.maxTessellationFactor = 16;
    d.supportIndirectCommandBuffers = YES;
    MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
    vd.attributes[0].format = MTLVertexFormatFloat2;
    vd.attributes[0].offset = 0;
    vd.attributes[0].bufferIndex = 0;
    vd.layouts[0].stride = 2 * sizeof(float);
    vd.layouts[0].stepFunction = MTLVertexStepFunctionPerPatchControlPoint;
    vd.layouts[0].stepRate = 1;
    d.vertexDescriptor = vd;
    NSError *err = nil;
    id<MTLRenderPipelineState> pso = [dev newRenderPipelineStateWithDescriptor:d error:&err];
    if (!pso) fail([NSString stringWithFormat:@"tess pso %@/%@: %@", vs, fs, err]);
    return pso;
}

static id<MTLComputePipelineState> makeComputePSO(id<MTLDevice> dev, id<MTLLibrary> lib, NSString *name) {
    NSError *err = nil;
    id<MTLFunction> fn = [lib newFunctionWithName:name];
    if (!fn) fail([NSString stringWithFormat:@"missing kernel %@", name]);
    id<MTLComputePipelineState> cpso = [dev newComputePipelineStateWithFunction:fn error:&err];
    if (!cpso) fail([NSString stringWithFormat:@"compute pso %@: %@", name, err]);
    return cpso;
}

static id<MTLIndirectCommandBuffer> makeICB(id<MTLDevice> dev, MTLIndirectCommandType types,
                                            BOOL inhPipe, BOOL inhBufs, NSUInteger count) {
    MTLIndirectCommandBufferDescriptor *d = [MTLIndirectCommandBufferDescriptor new];
    d.commandTypes = types;
    d.inheritPipelineState = inhPipe;
    d.inheritBuffers = inhBufs;
    if (!inhBufs) {
        d.maxVertexBufferBindCount = 4;
        d.maxFragmentBufferBindCount = 4;
    }
    id<MTLIndirectCommandBuffer> icb =
        [dev newIndirectCommandBufferWithDescriptor:d maxCommandCount:count
                                            options:MTLResourceStorageModePrivate];
    if (!icb) fail(@"newIndirectCommandBufferWithDescriptor nil");
    return icb;
}

static void dispatch1DWrite(id<MTLCommandQueue> queue, id<MTLComputePipelineState> cpso,
                            id<MTLBuffer> argBuf, NSUInteger threads, id<MTLResource> writeRes) {
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
    [ce setComputePipelineState:cpso];
    [ce setBuffer:argBuf offset:0 atIndex:0];
    if (writeRes) [ce useResource:writeRes usage:MTLResourceUsageWrite];
    [ce dispatchThreads:MTLSizeMake(threads, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [ce endEncoding];
    finish(cb, @"gpu-icb-producer");
}

static void dispatch1D(id<MTLCommandQueue> queue, id<MTLComputePipelineState> cpso,
                       id<MTLBuffer> argBuf, NSUInteger threads) {
    dispatch1DWrite(queue, cpso, argBuf, threads, nil);
}

static const char *kGPUICBShaderSource =
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"\n"
"struct VIn { float2 p [[attribute(0)]]; };\n"
"struct VOut { float4 pos [[position]]; float4 tint; };\n"
"\n"
"vertex VOut vs_attr_A(VIn in [[stage_in]], uint iid [[instance_id]]) {\n"
"    VOut o;\n"
"    o.pos = float4(in.p, 0.0, 1.0);\n"
"    o.tint = (iid == 2u) ? float4(0.0, 1.0, 0.0, 1.0) : float4(1.0, 1.0, 1.0, 1.0);\n"
"    return o;\n"
"}\n"
"\n"
"vertex VOut vs_attr_B(VIn in [[stage_in]], constant float4 &bias [[buffer(1)]]) {\n"
"    VOut o;\n"
"    o.pos = float4(in.p, 0.0, 1.0);\n"
"    o.tint = bias;\n"
"    return o;\n"
"}\n"
"\n"
"vertex VOut vs_vid_full(uint vid [[vertex_id]]) {\n"
"    float2 tri[3] = { float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0) };\n"
"    VOut o;\n"
"    o.pos = float4(tri[vid % 3u], 0.0, 1.0);\n"
"    o.tint = float4(1.0, 1.0, 1.0, 1.0);\n"
"    return o;\n"
"}\n"
"\n"
"fragment float4 fs_mul(VOut in [[stage_in]], constant float4 &col [[buffer(0)]]) {\n"
"    return in.tint * col;\n"
"}\n"
"\n"
"struct PatchIn { patch_control_point<VIn> cp; };\n"
"[[patch(triangle, 3)]]\n"
"vertex VOut tes_tri(PatchIn in [[stage_in]], float3 b [[position_in_patch]], uint pid [[patch_id]]) {\n"
"    VOut o;\n"
"    o.pos = float4(b.x * in.cp[0].p + b.y * in.cp[1].p + b.z * in.cp[2].p, 0.0, 1.0);\n"
"    o.tint = float4(b.x, b.y, float(pid) * 0.5 + 0.25, 1.0);\n"
"    return o;\n"
"}\n"
"\n"
"[[patch(quad, 4)]]\n"
"vertex VOut tes_quad(PatchIn in [[stage_in]], float2 uv [[position_in_patch]], uint iid [[instance_id]]) {\n"
"    VOut o;\n"
"    float2 bot = mix(in.cp[0].p, in.cp[1].p, uv.x);\n"
"    float2 top = mix(in.cp[2].p, in.cp[3].p, uv.x);\n"
"    o.pos = float4(mix(bot, top, uv.y), 0.0, 1.0);\n"
"    o.tint = float4(uv.x, uv.y, 0.2 + 0.3 * float(iid), 1.0);\n"
"    return o;\n"
"}\n"
"\n"
"struct BasicArgs {\n"
"    command_buffer icb;\n"
"    render_pipeline_state psoA;\n"
"    render_pipeline_state psoB;\n"
"    device float2 *vtxLeft;\n"
"    device float2 *vtxWithBase;\n"
"    device float2 *vtxBottomRight;\n"
"    constant float4 *colWhite;\n"
"    constant float4 *colRed;\n"
"    constant float4 *biasBlue;\n"
"    device ushort *idx16;\n"
"    device uint *idx32;\n"
"};\n"
"\n"
"kernel void k_basic_draws(device BasicArgs &a [[buffer(0)]], uint gid [[thread_position_in_grid]]) {\n"
"    render_command c(a.icb, gid);\n"
"    c.reset();\n"
"    if (gid == 0u) {\n"
"        c.set_render_pipeline_state(a.psoA);\n"
"        c.set_vertex_buffer(a.vtxLeft, 0);\n"
"        c.set_fragment_buffer(a.colRed, 0);\n"
"        c.draw_primitives(primitive_type::triangle, 0u, 3u, 1u, 0u);\n"
"    } else if (gid == 1u) {\n"
"        c.set_render_pipeline_state(a.psoA);\n"
"        c.set_vertex_buffer(a.vtxWithBase, 0);\n"
"        c.set_fragment_buffer(a.colWhite, 0);\n"
"        // base_vertex=3 selects triangle 1 (top-right); base_instance=2 gives green tint\n"
"        c.draw_indexed_primitives(primitive_type::triangle, 3u, a.idx16, 1u, 3, 2u);\n"
"    } else if (gid == 2u) {\n"
"        c.set_render_pipeline_state(a.psoB);\n"
"        c.set_vertex_buffer(a.vtxBottomRight, 0);\n"
"        c.set_vertex_buffer(a.biasBlue, 1);\n"
"        c.set_fragment_buffer(a.colWhite, 0);\n"
"        // base_vertex=1 shifts 32-bit indices 0,1,2 -> vertices 1,2,3 (bottom-right)\n"
"        c.draw_indexed_primitives(primitive_type::triangle, 3u, a.idx32, 1u, 1, 0u);\n"
"    }\n"
"}\n"
"\n"
"struct OrderArgs {\n"
"    command_buffer icb;\n"
"    render_pipeline_state psoA;\n"
"    render_pipeline_state psoB;\n"
"    device float2 *vtx;\n"
"    constant float4 *col0;\n"
"    constant float4 *col1;\n"
"    constant float4 *col2;\n"
"    constant float4 *col3;\n"
"    constant float4 *colWhite;\n"
"    uint mode;\n"
"};\n"
"\n"
"kernel void k_mixed_order(device OrderArgs &a [[buffer(0)]], uint gid [[thread_position_in_grid]]) {\n"
"    render_command c(a.icb, gid);\n"
"    if (a.mode == 2u) {\n"
"        // Copy slot 1 into slot 2 and reset slot 0\n"
"        if (gid == 2u) c.copy_command(render_command(a.icb, 1u));\n"
"        else if (gid == 0u) c.reset();\n"
"        return;\n"
"    }\n"
"    c.reset();\n"
"    constant float4 *cols[4] = { a.col0, a.col1, a.col2, a.col3 };\n"
"    bool useB = (a.mode == 0u) ? ((gid & 1u) != 0u) : ((gid & 1u) == 0u);\n"
"    c.set_render_pipeline_state(useB ? a.psoB : a.psoA);\n"
"    c.set_vertex_buffer(a.vtx, 0);\n"
"    if (useB) {\n"
"        c.set_vertex_buffer(cols[gid], 1);\n"
"        c.set_fragment_buffer(a.colWhite, 0);\n"
"    } else {\n"
"        c.set_fragment_buffer(cols[gid], 0);\n"
"    }\n"
"    c.draw_primitives(primitive_type::triangle, 0u, 3u, 1u, 0u);\n"
"}\n"
"\n"
"struct InheritArgs {\n"
"    command_buffer icb;\n"
"    device float2 *vtx;\n"
"    constant float4 *colOffset;\n"
"    uint mode;\n"
"};\n"
"\n"
"kernel void k_inherit_draw(device InheritArgs &a [[buffer(0)]], uint gid [[thread_position_in_grid]]) {\n"
"    render_command c(a.icb, gid);\n"
"    c.reset();\n"
"    if (a.mode == 1u) {\n"
"        // inheritPipelineState=YES, inheritBuffers=NO: non-zero byte offset via device/constant ptr\n"
"        c.set_vertex_buffer(a.vtx, 0);\n"
"        c.set_fragment_buffer(a.colOffset + 1, 0);\n"
"    }\n"
"    c.draw_primitives(primitive_type::triangle, 0u, 3u, 1u, 0u);\n"
"}\n"
"\n"
"struct PatchArgs {\n"
"    command_buffer icb;\n"
"    render_pipeline_state triPso;\n"
"    render_pipeline_state quadPso;\n"
"    device float2 *triVB;\n"
"    device float2 *quadVB;\n"
"    constant float4 *white;\n"
"    device uint *patchIdx;\n"
"    device ushort *cpIdx16;\n"
"    const device MTLTriangleTessellationFactorsHalf *triFactors;\n"
"    const device MTLQuadTessellationFactorsHalf *quadFactors;\n"
"};\n"
"\n"
"kernel void k_patch_draws(device PatchArgs &a [[buffer(0)]], uint gid [[thread_position_in_grid]]) {\n"
"    render_command c(a.icb, gid);\n"
"    c.reset();\n"
"    if (gid == 0u) {\n"
"        c.set_render_pipeline_state(a.triPso);\n"
"        c.set_vertex_buffer(a.triVB, 0);\n"
"        c.set_fragment_buffer(a.white, 0);\n"
"        c.draw_patches(3u, 0u, 1u, (const device uint *)nullptr, 1u, 0u, a.triFactors, 0u);\n"
"    } else if (gid == 1u) {\n"
"        c.set_render_pipeline_state(a.quadPso);\n"
"        c.set_vertex_buffer(a.quadVB, 0);\n"
"        c.set_fragment_buffer(a.white, 0);\n"
"        c.draw_indexed_patches(4u, 0u, 1u, a.patchIdx + 1, a.cpIdx16, 1u, 1u, a.quadFactors, 16u);\n"
"    }\n"
"}\n"
"\n"
"struct RangeWriteArgs {\n"
"    device uint *rangeBuf;\n"
"    uint loc;\n"
"    uint len;\n"
"};\n"
"\n"
"kernel void k_write_range(device RangeWriteArgs &a [[buffer(0)]]) {\n"
"    a.rangeBuf[4] = a.loc;\n"
"    a.rangeBuf[5] = a.len;\n"
"}\n";

static id<MTLLibrary> compileMSL21(id<MTLDevice> dev) {
    MTLCompileOptions *opts = [MTLCompileOptions new];
    opts.languageVersion = MTLLanguageVersion2_1;
    NSError *err = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:@(kGPUICBShaderSource) options:opts error:&err];
    if (!lib) fail([NSString stringWithFormat:@"MSL 2.1 library: %@", err]);
    return lib;
}

static void testGPUBasicDrawsAndIndexed(id<MTLDevice> dev, id<MTLCommandQueue> queue, id<MTLLibrary> lib) {
    id<MTLRenderPipelineState> psoA = makeRenderPSO(dev, lib, @"vs_attr_A", @"fs_mul", YES);
    id<MTLRenderPipelineState> psoB = makeRenderPSO(dev, lib, @"vs_attr_B", @"fs_mul", YES);
    id<MTLFunction> kf = [lib newFunctionWithName:@"k_basic_draws"];
    id<MTLComputePipelineState> cpso = makeComputePSO(dev, lib, @"k_basic_draws");
    id<MTLArgumentEncoder> ae = [kf newArgumentEncoderWithBufferIndex:0];

    // Top-left triangle covering (4, 4) -> NDC (-1,1), (-1,0), (0,1)
    const float vLeft[6] = { -1.0f, 1.0f, -1.0f, 0.0f, 0.0f, 1.0f };
    // Vertices 0..2 dummy, 3..5 top-right triangle covering (26, 4) -> NDC (0,1), (1,1), (1,0)
    const float vWithBase[12] = { 0, 0, 0, 0, 0, 0, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f };
    // Vertex 0 dummy, vertices 1..3 bottom triangle covering (16, 26) -> NDC (-1,-1), (1,-1), (0,0)
    const float vBottom[8] = { 0, 0, -1.0f, -1.0f, 1.0f, -1.0f, 0.0f, 0.0f };
    const float white[4] = { 1, 1, 1, 1 }, red[4] = { 1, 0, 0, 1 }, blue[4] = { 0.25f, 0.5f, 1.0f, 1 };
    const uint16_t idx16[4] = { 0, 1, 2, 0 };
    const uint32_t idx32[4] = { 0, 1, 2, 0 };

    id<MTLBuffer> bLeft = [dev newBufferWithBytes:vLeft length:sizeof vLeft options:MTLResourceStorageModeShared];
    id<MTLBuffer> bBase = [dev newBufferWithBytes:vWithBase length:sizeof vWithBase options:MTLResourceStorageModeShared];
    id<MTLBuffer> bBot = [dev newBufferWithBytes:vBottom length:sizeof vBottom options:MTLResourceStorageModeShared];
    id<MTLBuffer> bWhite = [dev newBufferWithBytes:white length:sizeof white options:MTLResourceStorageModeShared];
    id<MTLBuffer> bRed = [dev newBufferWithBytes:red length:sizeof red options:MTLResourceStorageModeShared];
    id<MTLBuffer> bBlue = [dev newBufferWithBytes:blue length:sizeof blue options:MTLResourceStorageModeShared];
    id<MTLBuffer> bI16 = [dev newBufferWithBytes:idx16 length:sizeof idx16 options:MTLResourceStorageModeShared];
    id<MTLBuffer> bI32 = [dev newBufferWithBytes:idx32 length:sizeof idx32 options:MTLResourceStorageModeShared];

    id<MTLIndirectCommandBuffer> icb = makeICB(dev, MTLIndirectCommandTypeDraw | MTLIndirectCommandTypeDrawIndexed, NO, NO, 4);
    id<MTLBuffer> args = [dev newBufferWithLength:ae.encodedLength options:MTLResourceStorageModeShared];
    [ae setArgumentBuffer:args offset:0];
    [ae setIndirectCommandBuffer:icb atIndex:0];
    [ae setRenderPipelineState:psoA atIndex:1];
    [ae setRenderPipelineState:psoB atIndex:2];
    [ae setBuffer:bLeft offset:0 atIndex:3];
    [ae setBuffer:bBase offset:0 atIndex:4];
    [ae setBuffer:bBot offset:0 atIndex:5];
    [ae setBuffer:bWhite offset:0 atIndex:6];
    [ae setBuffer:bRed offset:0 atIndex:7];
    [ae setBuffer:bBlue offset:0 atIndex:8];
    [ae setBuffer:bI16 offset:0 atIndex:9];
    [ae setBuffer:bI32 offset:0 atIndex:10];

    dispatch1D(queue, cpso, args, 4);

    id<MTLTexture> target = newTarget(dev);
    id<MTLCommandBuffer> cb = nil;
    id<MTLRenderCommandEncoder> enc = beginRender(queue, target, &cb);
    [enc executeCommandsInBuffer:icb withRange:NSMakeRange(0, 4)];
    [enc endEncoding];
    finish(cb, @"gpu-basic-render");

    NSData *img = readTexture(dev, queue, target, @"gpu-basic");
    pixelAt(img, 32, 3, 3, 255, 0, 0, 255, @"slot0-tri-red");
    pixelAt(img, 32, 28, 3, 0, 255, 0, 255, @"slot1-idx16-basev-basei-green");
    pixelAt(img, 32, 16, 28, 64, 128, 255, 255, @"slot2-idx32-psoB-blue");
    pixelAt(img, 32, 16, 12, 0, 0, 0, 255, @"clear-center");
    assertZeroReadback(icb, @"gpu-basic");
    passCase(@"gpu-draw-and-indexed-u16-u32-basev-basei");
}

static void testGPUMixedOrderAndInherit(id<MTLDevice> dev, id<MTLCommandQueue> queue, id<MTLLibrary> lib) {
    id<MTLRenderPipelineState> psoA = makeRenderPSO(dev, lib, @"vs_attr_A", @"fs_mul", YES);
    id<MTLRenderPipelineState> psoB = makeRenderPSO(dev, lib, @"vs_attr_B", @"fs_mul", YES);
    id<MTLRenderPipelineState> psoVid = makeRenderPSO(dev, lib, @"vs_vid_full", @"fs_mul", NO);

    const float full[6] = { -1, -1, 3, -1, -1, 3 };
    const float red[4] = { 1, 0, 0, 1 }, green[4] = { 0, 1, 0, 1 }, blue[4] = { 0, 0, 1, 1 };
    const float cyan[4] = { 0, 1, 1, 1 }, yellow[4] = { 1, 1, 0, 1 }, white[4] = { 1, 1, 1, 1 };
    const float offsetColors[8] = { 0, 0, 0, 1, 1, 0, 1, 1 }; // offset+1 = magenta!

    id<MTLBuffer> bFull = [dev newBufferWithBytes:full length:sizeof full options:MTLResourceStorageModeShared];
    id<MTLBuffer> bR = [dev newBufferWithBytes:red length:sizeof red options:MTLResourceStorageModeShared];
    id<MTLBuffer> bG = [dev newBufferWithBytes:green length:sizeof green options:MTLResourceStorageModeShared];
    id<MTLBuffer> bB = [dev newBufferWithBytes:blue length:sizeof blue options:MTLResourceStorageModeShared];
    id<MTLBuffer> bC = [dev newBufferWithBytes:cyan length:sizeof cyan options:MTLResourceStorageModeShared];
    id<MTLBuffer> bY = [dev newBufferWithBytes:yellow length:sizeof yellow options:MTLResourceStorageModeShared];
    id<MTLBuffer> bW = [dev newBufferWithBytes:white length:sizeof white options:MTLResourceStorageModeShared];
    id<MTLBuffer> bOffCols = [dev newBufferWithBytes:offsetColors length:sizeof offsetColors options:MTLResourceStorageModeShared];

    id<MTLFunction> kf = [lib newFunctionWithName:@"k_mixed_order"];
    id<MTLComputePipelineState> cpso = makeComputePSO(dev, lib, @"k_mixed_order");
    id<MTLArgumentEncoder> ae = [kf newArgumentEncoderWithBufferIndex:0];

    // Strict slot order A(Red)->B(Green)->A(Blue)->B(Cyan) => Cyan wins, and B->A->B->A(Yellow) => Yellow wins
    for (uint32_t mode = 0; mode < 2; mode++) {
        id<MTLIndirectCommandBuffer> icb = makeICB(dev, MTLIndirectCommandTypeDraw, NO, NO, 4);
        id<MTLBuffer> args = [dev newBufferWithLength:ae.encodedLength options:MTLResourceStorageModeShared];
        [ae setArgumentBuffer:args offset:0];
        [ae setIndirectCommandBuffer:icb atIndex:0];
        [ae setRenderPipelineState:psoA atIndex:1];
        [ae setRenderPipelineState:psoB atIndex:2];
        [ae setBuffer:bFull offset:0 atIndex:3];
        [ae setBuffer:bR offset:0 atIndex:4];
        [ae setBuffer:bG offset:0 atIndex:5];
        [ae setBuffer:bB offset:0 atIndex:6];
        [ae setBuffer:(mode == 0 ? bC : bY) offset:0 atIndex:7];
        [ae setBuffer:bW offset:0 atIndex:8];
        *(uint32_t *)[ae constantDataAtIndex:9] = mode;

        dispatch1D(queue, cpso, args, 4);

        id<MTLTexture> target = newTarget(dev);
        id<MTLCommandBuffer> cb = nil;
        id<MTLRenderCommandEncoder> enc = beginRender(queue, target, &cb);
        [enc executeCommandsInBuffer:icb withRange:NSMakeRange(0, 4)];
        [enc endEncoding];
        finish(cb, mode == 0 ? @"strict-order-ABAB" : @"strict-order-BABA");

        NSData *img = readTexture(dev, queue, target, @"strict-order");
        if (mode == 0) pixelAt(img, 32, 16, 16, 0, 255, 255, 255, @"strict-order-ABAB-cyan");
        else pixelAt(img, 32, 16, 16, 255, 255, 0, 255, @"strict-order-BABA-yellow");
        assertZeroReadback(icb, @"gpu-mixed-order");
    }

    // Inherit pipeline + buffers (and inherit pipeline + offset explicit buffers)
    id<MTLFunction> ikf = [lib newFunctionWithName:@"k_inherit_draw"];
    id<MTLComputePipelineState> icpso = makeComputePSO(dev, lib, @"k_inherit_draw");
    id<MTLArgumentEncoder> iae = [ikf newArgumentEncoderWithBufferIndex:0];
    for (uint32_t imode = 0; imode < 2; imode++) {
        BOOL inhBuf = (imode == 0);
        id<MTLIndirectCommandBuffer> icb = makeICB(dev, MTLIndirectCommandTypeDraw, YES, inhBuf, 1);
        id<MTLBuffer> args = [dev newBufferWithLength:iae.encodedLength options:MTLResourceStorageModeShared];
        [iae setArgumentBuffer:args offset:0];
        [iae setIndirectCommandBuffer:icb atIndex:0];
        [iae setBuffer:bFull offset:0 atIndex:1];
        [iae setBuffer:bOffCols offset:0 atIndex:2];
        *(uint32_t *)[iae constantDataAtIndex:3] = imode;
        dispatch1D(queue, icpso, args, 1);

        id<MTLTexture> t = newTarget(dev);
        id<MTLCommandBuffer> cb = nil;
        id<MTLRenderCommandEncoder> enc = beginRender(queue, t, &cb);
        [enc setRenderPipelineState:inhBuf ? psoVid : psoA];
        if (inhBuf) [enc setFragmentBuffer:bC offset:0 atIndex:0];
        [enc executeCommandsInBuffer:icb withRange:NSMakeRange(0, 1)];
        [enc endEncoding];
        finish(cb, @"gpu-inherit");
        NSData *img = readTexture(dev, queue, t, @"gpu-inherit");
        if (inhBuf) pixelAt(img, 32, 16, 16, 0, 255, 255, 255, @"gpu-inherit-both-cyan");
        else pixelAt(img, 32, 16, 16, 255, 0, 255, 255, @"gpu-inherit-pipe-ptroff-magenta");
        assertZeroReadback(icb, @"gpu-inherit");
    }
    passCase(@"gpu-mixed-order-and-inherit-modes");
}

static void testGPUPatches(id<MTLDevice> dev, id<MTLCommandQueue> queue, id<MTLLibrary> lib) {
    id<MTLRenderPipelineState> triPso = makeTessPSO(dev, lib, @"tes_tri", @"fs_mul",
        MTLTessellationControlPointIndexTypeNone, MTLTessellationFactorStepFunctionPerPatch);
    id<MTLRenderPipelineState> quadPso = makeTessPSO(dev, lib, @"tes_quad", @"fs_mul",
        MTLTessellationControlPointIndexTypeUInt16, MTLTessellationFactorStepFunctionPerPatchAndPerInstance);

    const float triVerts[6] = { -1.0f, 1.0f, -1.0f, -1.0f, 0.2f, 1.0f };
    const float quadVerts[8] = { -0.2f, -1.0f, 1.0f, -1.0f, -0.2f, 0.2f, 1.0f, 0.2f };
    const float white[4] = { 1, 1, 1, 1 };
    const uint32_t patchIdx[2] = { 99u, 0u };
    const uint16_t cpIdx16[4] = { 0, 1, 2, 3 };
    const uint16_t triF[4] = { f32_to_f16(4), f32_to_f16(4), f32_to_f16(4), f32_to_f16(4) };
    const uint16_t quadF[16] = {
        0, 0, 0, 0, 0, 0, 0, 0,
        f32_to_f16(4), f32_to_f16(4), f32_to_f16(4), f32_to_f16(4), f32_to_f16(4), f32_to_f16(4), 0, 0,
    };

    id<MTLBuffer> bTriV = [dev newBufferWithBytes:triVerts length:sizeof triVerts options:MTLResourceStorageModeShared];
    id<MTLBuffer> bQuadV = [dev newBufferWithBytes:quadVerts length:sizeof quadVerts options:MTLResourceStorageModeShared];
    id<MTLBuffer> bW = [dev newBufferWithBytes:white length:sizeof white options:MTLResourceStorageModeShared];
    id<MTLBuffer> bPIdx = [dev newBufferWithBytes:patchIdx length:sizeof patchIdx options:MTLResourceStorageModeShared];
    id<MTLBuffer> bCpIdx = [dev newBufferWithBytes:cpIdx16 length:sizeof cpIdx16 options:MTLResourceStorageModeShared];
    id<MTLBuffer> bTriF = [dev newBufferWithBytes:triF length:sizeof triF options:MTLResourceStorageModeShared];
    id<MTLBuffer> bQuadF = [dev newBufferWithBytes:quadF length:sizeof quadF options:MTLResourceStorageModeShared];

    // Direct reference
    id<MTLTexture> dt = newTarget(dev);
    id<MTLCommandBuffer> cb = nil;
    id<MTLRenderCommandEncoder> enc = beginRender(queue, dt, &cb);
    [enc setTessellationFactorScale:1.0f];
    [enc setRenderPipelineState:triPso];
    [enc setVertexBuffer:bTriV offset:0 atIndex:0];
    [enc setFragmentBuffer:bW offset:0 atIndex:0];
    [enc setTessellationFactorBuffer:bTriF offset:0 instanceStride:0];
    [enc drawPatches:3 patchStart:0 patchCount:1 patchIndexBuffer:nil patchIndexBufferOffset:0 instanceCount:1 baseInstance:0];
    [enc setRenderPipelineState:quadPso];
    [enc setVertexBuffer:bQuadV offset:0 atIndex:0];
    [enc setFragmentBuffer:bW offset:0 atIndex:0];
    [enc setTessellationFactorBuffer:bQuadF offset:0 instanceStride:16];
    [enc drawIndexedPatches:4 patchStart:0 patchCount:1 patchIndexBuffer:bPIdx patchIndexBufferOffset:4
        controlPointIndexBuffer:bCpIdx controlPointIndexBufferOffset:0 instanceCount:1 baseInstance:1];
    [enc endEncoding];
    finish(cb, @"gpu-patch-direct");

    // GPU ICB
    id<MTLFunction> kf = [lib newFunctionWithName:@"k_patch_draws"];
    id<MTLComputePipelineState> cpso = makeComputePSO(dev, lib, @"k_patch_draws");
    id<MTLArgumentEncoder> ae = [kf newArgumentEncoderWithBufferIndex:0];
    id<MTLIndirectCommandBuffer> icb = makeICB(dev, MTLIndirectCommandTypeDrawPatches | MTLIndirectCommandTypeDrawIndexedPatches, NO, NO, 2);
    id<MTLBuffer> args = [dev newBufferWithLength:ae.encodedLength options:MTLResourceStorageModeShared];
    [ae setArgumentBuffer:args offset:0];
    [ae setIndirectCommandBuffer:icb atIndex:0];
    [ae setRenderPipelineState:triPso atIndex:1];
    [ae setRenderPipelineState:quadPso atIndex:2];
    [ae setBuffer:bTriV offset:0 atIndex:3];
    [ae setBuffer:bQuadV offset:0 atIndex:4];
    [ae setBuffer:bW offset:0 atIndex:5];
    [ae setBuffer:bPIdx offset:0 atIndex:6];
    [ae setBuffer:bCpIdx offset:0 atIndex:7];
    [ae setBuffer:bTriF offset:0 atIndex:8];
    [ae setBuffer:bQuadF offset:0 atIndex:9];
    dispatch1D(queue, cpso, args, 2);

    id<MTLTexture> it = newTarget(dev);
    cb = nil;
    enc = beginRender(queue, it, &cb);
    [enc setTessellationFactorScale:1.0f];
    [enc executeCommandsInBuffer:icb withRange:NSMakeRange(0, 2)];
    [enc endEncoding];
    finish(cb, @"gpu-patch-icb");
    expectSamePixels(readTexture(dev, queue, dt, @"patch-d"),
                     readTexture(dev, queue, it, @"patch-i"), @"gpu-patch-equivalence");

    // Parent scale=0.0f must cull GPU ICB patch draws
    id<MTLTexture> zt = newTarget(dev);
    cb = nil;
    enc = beginRender(queue, zt, &cb);
    [enc setTessellationFactorScale:0.0f];
    [enc executeCommandsInBuffer:icb withRange:NSMakeRange(0, 2)];
    [enc endEncoding];
    finish(cb, @"gpu-patch-scale0");
    NSData *zimg = readTexture(dev, queue, zt, @"gpu-patch-scale0");
    pixelAt(zimg, 32, 4, 4, 0, 0, 0, 255, @"gpu-patch-scale0-tri");
    pixelAt(zimg, 32, 24, 24, 0, 0, 0, 255, @"gpu-patch-scale0-quad");
    assertZeroReadback(icb, @"gpu-patch");
    passCase(@"gpu-draw-patches-and-indexed-patches");
}

static void testGPUCopyBlitOverlapAndIndirectRange(id<MTLDevice> dev, id<MTLCommandQueue> queue, id<MTLLibrary> lib) {
    id<MTLRenderPipelineState> psoA = makeRenderPSO(dev, lib, @"vs_attr_A", @"fs_mul", YES);
    id<MTLRenderPipelineState> psoB = makeRenderPSO(dev, lib, @"vs_attr_B", @"fs_mul", YES);
    const float full[6] = { -1, -1, 3, -1, -1, 3 };
    const float r[4] = { 1, 0, 0, 1 }, g[4] = { 0, 1, 0, 1 }, b[4] = { 0, 0, 1, 1 }, y[4] = { 1, 1, 0, 1 }, w[4] = { 1, 1, 1, 1 };
    id<MTLBuffer> bFull = [dev newBufferWithBytes:full length:sizeof full options:MTLResourceStorageModeShared];
    id<MTLBuffer> bR = [dev newBufferWithBytes:r length:sizeof r options:MTLResourceStorageModeShared];
    id<MTLBuffer> bG = [dev newBufferWithBytes:g length:sizeof g options:MTLResourceStorageModeShared];
    id<MTLBuffer> bB = [dev newBufferWithBytes:b length:sizeof b options:MTLResourceStorageModeShared];
    id<MTLBuffer> bY = [dev newBufferWithBytes:y length:sizeof y options:MTLResourceStorageModeShared];
    id<MTLBuffer> bW = [dev newBufferWithBytes:w length:sizeof w options:MTLResourceStorageModeShared];

    id<MTLFunction> kf = [lib newFunctionWithName:@"k_mixed_order"];
    id<MTLComputePipelineState> cpso = makeComputePSO(dev, lib, @"k_mixed_order");
    id<MTLArgumentEncoder> ae = [kf newArgumentEncoderWithBufferIndex:0];

    // 1) GPU compute copy_command + reset, then blit copy + overlapping self-copy + blit reset
    id<MTLIndirectCommandBuffer> src = makeICB(dev, MTLIndirectCommandTypeDraw, NO, NO, 4);
    id<MTLIndirectCommandBuffer> dst = makeICB(dev, MTLIndirectCommandTypeDraw, NO, NO, 4);
    id<MTLBuffer> args = [dev newBufferWithLength:ae.encodedLength options:MTLResourceStorageModeShared];
    [ae setArgumentBuffer:args offset:0];
    [ae setIndirectCommandBuffer:src atIndex:0];
    [ae setRenderPipelineState:psoA atIndex:1];
    [ae setRenderPipelineState:psoB atIndex:2];
    [ae setBuffer:bFull offset:0 atIndex:3];
    [ae setBuffer:bR offset:0 atIndex:4];
    [ae setBuffer:bG offset:0 atIndex:5];
    [ae setBuffer:bB offset:0 atIndex:6];
    [ae setBuffer:bY offset:0 atIndex:7];
    [ae setBuffer:bW offset:0 atIndex:8];
    *(uint32_t *)[ae constantDataAtIndex:9] = 0; // src: [0]=Red [1]=Green [2]=Blue [3]=Yellow
    dispatch1D(queue, cpso, args, 4);

    // Kernel mode=2: copy slot 1 (Green) -> slot 2, reset slot 0
    *(uint32_t *)[ae constantDataAtIndex:9] = 2;
    dispatch1D(queue, cpso, args, 3);

    // Execute slot 2 -> Green, slot 0 -> empty (clear black)
    id<MTLTexture> t = newTarget(dev);
    id<MTLCommandBuffer> cb = nil;
    id<MTLRenderCommandEncoder> enc = beginRender(queue, t, &cb);
    [enc executeCommandsInBuffer:src withRange:NSMakeRange(2, 1)];
    [enc executeCommandsInBuffer:src withRange:NSMakeRange(0, 1)];
    [enc endEncoding];
    finish(cb, @"gpu-copy-reset");
    pixelAt(readTexture(dev, queue, t, @"gpu-copy-reset"), 32, 16, 16, 0, 255, 0, 255, @"gpu-copy-command-green");

    // Blit copy [1..4) -> dst[0..3), then overlapping dst[0..2) -> dst[1..3), then reset dst[0]
    cb = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit copyIndirectCommandBuffer:src sourceRange:NSMakeRange(1, 3) destination:dst destinationIndex:0]; // dst: G, G, Y, empty
    [blit copyIndirectCommandBuffer:dst sourceRange:NSMakeRange(0, 2) destination:dst destinationIndex:1]; // dst: G, G, G, empty (if non-overlapping broken, [0]->[1] then old [1]->[2] works only if scratch!)
    // Better overlap test: prepare dst with [0]=Blue, [1]=Yellow, copy [0..2) -> [1..3) => dst should be [0]=B, [1]=B, [2]=Y!
    [blit endEncoding];
    finish(cb, @"gpu-blit-prime");

    // Explicitly populate dst[0]=Blue, dst[1]=Yellow via compute mode=0 (Blue, Yellow, Blue, Yellow), then overlap-copy [0..2)->[1..3) and reset [0]
    id<MTLBuffer> argsDst = [dev newBufferWithLength:ae.encodedLength options:MTLResourceStorageModeShared];
    [ae setArgumentBuffer:argsDst offset:0];
    [ae setIndirectCommandBuffer:dst atIndex:0];
    [ae setRenderPipelineState:psoA atIndex:1];
    [ae setRenderPipelineState:psoB atIndex:2];
    [ae setBuffer:bFull offset:0 atIndex:3];
    [ae setBuffer:bB offset:0 atIndex:4]; // slot 0 = Blue
    [ae setBuffer:bY offset:0 atIndex:5]; // slot 1 = Yellow
    [ae setBuffer:bR offset:0 atIndex:6]; // slot 2 = Red (will be overwritten by dst[1]=Yellow? Wait: [0..2)->[1..3) overwrites [1]=Blue, [2]=Yellow!)
    [ae setBuffer:bR offset:0 atIndex:7]; // slot 3 = Red
    [ae setBuffer:bW offset:0 atIndex:8];
    *(uint32_t *)[ae constantDataAtIndex:9] = 0;
    dispatch1D(queue, cpso, argsDst, 4);

    cb = [queue commandBuffer];
    blit = [cb blitCommandEncoder];
    [blit copyIndirectCommandBuffer:dst sourceRange:NSMakeRange(0, 2) destination:dst destinationIndex:1];
    [blit resetCommandsInBuffer:dst withRange:NSMakeRange(0, 1)];
    [blit endEncoding];
    finish(cb, @"gpu-blit-overlap");

    t = newTarget(dev);
    cb = nil;
    enc = beginRender(queue, t, &cb);
    [enc executeCommandsInBuffer:dst withRange:NSMakeRange(0, 3)]; // [0]=empty, [1]=Blue, [2]=Yellow -> Yellow wins!
    [enc endEncoding];
    finish(cb, @"gpu-blit-overlap-exec");
    pixelAt(readTexture(dev, queue, t, @"gpu-blit-overlap-12"), 32, 16, 16, 255, 255, 0, 255, @"overlap-slot2-yellow");

    // Verify [1..2) alone is Blue (not overwritten by [1]!)
    t = newTarget(dev);
    cb = nil;
    enc = beginRender(queue, t, &cb);
    [enc executeCommandsInBuffer:dst withRange:NSMakeRange(1, 1)];
    [enc endEncoding];
    finish(cb, @"gpu-blit-overlap-slot1");
    pixelAt(readTexture(dev, queue, t, @"gpu-blit-overlap-1"), 32, 16, 16, 0, 0, 255, 255, @"overlap-slot1-blue");

    // 2) Indirect range buffer in Private memory written by compute kernel
    id<MTLBuffer> indRangeBuf = [dev newBufferWithLength:64 options:MTLResourceStorageModePrivate];
    id<MTLFunction> rkf = [lib newFunctionWithName:@"k_write_range"];
    id<MTLComputePipelineState> rcpso = makeComputePSO(dev, lib, @"k_write_range");
    id<MTLArgumentEncoder> rae = [rkf newArgumentEncoderWithBufferIndex:0];
    id<MTLBuffer> rArgs = [dev newBufferWithLength:rae.encodedLength options:MTLResourceStorageModeShared];
    [rae setArgumentBuffer:rArgs offset:0];
    [rae setBuffer:indRangeBuf offset:0 atIndex:0];

    // src still has slot 1=Green, slot 2=Green; use fresh src with R(0), G(1), B(2), Y(3)
    id<MTLIndirectCommandBuffer> icbInd = makeICB(dev, MTLIndirectCommandTypeDraw, NO, NO, 4);
    [ae setArgumentBuffer:args offset:0];
    [ae setIndirectCommandBuffer:icbInd atIndex:0];
    [ae setBuffer:bR offset:0 atIndex:4];
    [ae setBuffer:bG offset:0 atIndex:5];
    [ae setBuffer:bB offset:0 atIndex:6];
    [ae setBuffer:bY offset:0 atIndex:7];
    *(uint32_t *)[ae constantDataAtIndex:9] = 0;
    dispatch1D(queue, cpso, args, 4);

    // Indirect range { loc = 1, len = 1 } -> Green
    *(uint32_t *)[rae constantDataAtIndex:1] = 1;
    *(uint32_t *)[rae constantDataAtIndex:2] = 1;
    dispatch1DWrite(queue, rcpso, rArgs, 1, indRangeBuf);
    t = newTarget(dev);
    cb = nil;
    enc = beginRender(queue, t, &cb);
    [enc executeCommandsInBuffer:icbInd indirectBuffer:indRangeBuf indirectBufferOffset:16];
    [enc endEncoding];
    finish(cb, @"gpu-indirect-range-1-1");
    pixelAt(readTexture(dev, queue, t, @"gpu-indirect-1-1"), 32, 16, 16, 0, 255, 0, 255, @"indirect-loc1-len1-green");

    // Indirect range { loc = 2, len = 2 } -> Blue then Yellow -> Yellow
    *(uint32_t *)[rae constantDataAtIndex:1] = 2;
    *(uint32_t *)[rae constantDataAtIndex:2] = 2;
    dispatch1DWrite(queue, rcpso, rArgs, 1, indRangeBuf);
    t = newTarget(dev);
    cb = nil;
    enc = beginRender(queue, t, &cb);
    [enc executeCommandsInBuffer:icbInd indirectBuffer:indRangeBuf indirectBufferOffset:16];
    [enc endEncoding];
    finish(cb, @"gpu-indirect-range-2-2");
    pixelAt(readTexture(dev, queue, t, @"gpu-indirect-2-2"), 32, 16, 16, 255, 255, 0, 255, @"indirect-loc2-len2-yellow");

    // Indirect range { loc = 0, len = 0 } -> empty (clear black)
    *(uint32_t *)[rae constantDataAtIndex:1] = 0;
    *(uint32_t *)[rae constantDataAtIndex:2] = 0;
    dispatch1DWrite(queue, rcpso, rArgs, 1, indRangeBuf);
    t = newTarget(dev);
    cb = nil;
    enc = beginRender(queue, t, &cb);
    [enc executeCommandsInBuffer:icbInd indirectBuffer:indRangeBuf indirectBufferOffset:16];
    [enc endEncoding];
    finish(cb, @"gpu-indirect-range-0-0");
    pixelAt(readTexture(dev, queue, t, @"gpu-indirect-0-0"), 32, 16, 16, 0, 0, 0, 255, @"indirect-loc0-len0-clear");

    assertZeroReadback(src, @"gpu-copy-src");
    assertZeroReadback(dst, @"gpu-copy-dst");
    assertZeroReadback(icbInd, @"gpu-indirect-range");
    passCase(@"gpu-copy-reset-blit-overlap-and-indirect-range");
}

static void testGPUNegativeBoundaries(id<MTLDevice> dev, id<MTLCommandQueue> queue, id<MTLLibrary> lib) {
    id<MTLRenderPipelineState> psoA = makeRenderPSO(dev, lib, @"vs_vid_full", @"fs_mul", NO);
    const float red[4] = { 1, 0, 0, 1 };
    id<MTLBuffer> bR = [dev newBufferWithBytes:red length:sizeof red options:MTLResourceStorageModeShared];
    id<MTLIndirectCommandBuffer> icb = makeICB(dev, MTLIndirectCommandTypeDraw, NO, NO, 2);
    id<MTLBuffer> rBuf = [dev newBufferWithLength:32 options:MTLResourceStorageModeShared];

    // Seed GPU backing cleanly
    id<MTLFunction> kf = [lib newFunctionWithName:@"k_inherit_draw"];
    id<MTLComputePipelineState> icpso = makeComputePSO(dev, lib, @"k_inherit_draw");
    id<MTLArgumentEncoder> iae = [kf newArgumentEncoderWithBufferIndex:0];
    id<MTLBuffer> args = [dev newBufferWithLength:iae.encodedLength options:MTLResourceStorageModeShared];
    [iae setArgumentBuffer:args offset:0];
    [iae setIndirectCommandBuffer:icb atIndex:0];
    [iae setBuffer:bR offset:0 atIndex:1];
    [iae setBuffer:bR offset:0 atIndex:2];
    *(uint32_t *)[iae constantDataAtIndex:3] = 0;

    // 1) Misaligned indirectBufferOffset (2) and out-of-bounds offset (28 for 32B buffer)
    id<MTLCommandBuffer> cb = nil;
    id<MTLRenderCommandEncoder> enc = beginRender(queue, newTarget(dev), &cb);
    [enc setRenderPipelineState:psoA];
    [enc executeCommandsInBuffer:icb indirectBuffer:rBuf indirectBufferOffset:2];
    [enc endEncoding];
    expectCommandError(cb, @"indirect-range-misaligned");

    cb = nil;
    enc = beginRender(queue, newTarget(dev), &cb);
    [enc setRenderPipelineState:psoA];
    [enc executeCommandsInBuffer:icb indirectBuffer:rBuf indirectBufferOffset:28];
    [enc endEncoding];
    expectCommandError(cb, @"indirect-range-oob");

    // 2) Compute ICB descriptor refused
    MTLIndirectCommandBufferDescriptor *cd = [MTLIndirectCommandBufferDescriptor new];
    cd.commandTypes = MTLIndirectCommandTypeConcurrentDispatch;
    if ([dev newIndirectCommandBufferWithDescriptor:cd maxCommandCount:2 options:MTLResourceStorageModePrivate])
        fail(@"compute ICB accepted unexpectedly");

    // 3) >32 candidate pipelines refused
    id<MTLFunction> okf = [lib newFunctionWithName:@"k_mixed_order"];
    id<MTLComputePipelineState> ocpso = makeComputePSO(dev, lib, @"k_mixed_order");
    id<MTLArgumentEncoder> oae = [okf newArgumentEncoderWithBufferIndex:0];
    id<MTLIndirectCommandBuffer> icb32 = makeICB(dev, MTLIndirectCommandTypeDraw, NO, NO, 2);
    id<MTLBuffer> oargs = [dev newBufferWithLength:oae.encodedLength options:MTLResourceStorageModeShared];
    for (int i = 0; i < 33; i++) {
        id<MTLRenderPipelineState> pi = makeRenderPSO(dev, lib, @"vs_vid_full", @"fs_mul", NO);
        [oae setArgumentBuffer:oargs offset:0];
        [oae setIndirectCommandBuffer:icb32 atIndex:0];
        [oae setRenderPipelineState:pi atIndex:1];
        [oae setRenderPipelineState:pi atIndex:2];
        [oae setBuffer:bR offset:0 atIndex:3];
        [oae setBuffer:bR offset:0 atIndex:4];
        [oae setBuffer:bR offset:0 atIndex:5];
        [oae setBuffer:bR offset:0 atIndex:6];
        [oae setBuffer:bR offset:0 atIndex:7];
        [oae setBuffer:bR offset:0 atIndex:8];
        *(uint32_t *)[oae constantDataAtIndex:9] = 0;
        id<MTLCommandBuffer> pcb = [queue commandBuffer];
        id<MTLComputeCommandEncoder> pce = [pcb computeCommandEncoder];
        [pce setComputePipelineState:ocpso];
        [pce setBuffer:oargs offset:0 atIndex:0];
        [pce dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
        [pce endEncoding];
        if (i < 32) finish(pcb, @"cand-within-32");
        else expectCommandError(pcb, @"cand-exceeds-32");
    }
    (void)icpso;
    assertZeroReadback(icb, @"gpu-neg");
    assertZeroReadback(icb32, @"gpu-neg-32");
    passCase(@"gpu-negative-boundaries-and-candidate-cap");
}

int main(int argc, const char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        puts("Usage: sudo test-icb-gpu BUNDLE|--reference");
        return 0;
    }
    if (argc != 2) return 2;
    BOOL ref = !strcmp(argv[1], "--reference");
    if (!ref && geteuid() != 0) return 2;
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(180);
    @autoreleasepool {
        id<MTLDevice> dev = nil;
        if (ref) {
            dev = MTLCreateSystemDefaultDevice();
        } else {
            setenv("RDNA4M_ALLOW", "1", 1);
            setenv("RDNA4M_TEST_INPROC", "1", 1);
            NSError *err = nil;
            if (![[NSBundle bundleWithPath:@(argv[1])] loadAndReturnError:&err])
                fail(err.description);
            dev = [[objc_getClass("GarnetDevice") alloc] init];
            ((void (*)(id, SEL))objc_msgSend)(dev, sel_registerName("initLimits"));
        }
        if (!dev) fail(@"device nil");
        id<MTLCommandQueue> queue = [dev newCommandQueue];
        if (!queue) fail(@"queue nil");
        id<MTLLibrary> lib = compileMSL21(dev);

        testGPUBasicDrawsAndIndexed(dev, queue, lib);
        testGPUMixedOrderAndInherit(dev, queue, lib);
        testGPUPatches(dev, queue, lib);
        testGPUCopyBlitOverlapAndIndirectRange(dev, queue, lib);
        if (!ref) testGPUNegativeBoundaries(dev, queue, lib);
        printf("ICB_GPU_SUITE_PASS cpu_readback=0\n");
    }
    return 0;
}
