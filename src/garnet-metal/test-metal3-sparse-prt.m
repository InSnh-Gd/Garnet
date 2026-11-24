// test-metal3-sparse-prt.m: live RX 9060 XT / reference Metal verification for Task 5.3
// (64 KiB sparse tile queries, pixel<->tile region conversions, MTLHeapTypeSparse +
// sparse 2D texture creation, MTLResourceStateCommandEncoder direct/batch/indirect map &
// unmap with MTLFence synchronization, MSL sparse_color<float4>::resident() / non-resident
// zero-read & write-discard, negative error checks, and EX01 MTLGPUFamilyApple6 == NO).
// Build on macOS:
//   clang -arch x86_64 -fobjc-arc -O2 -Wall -Wextra -Werror -framework Foundation -framework Metal src/garnet-metal/test-metal3-sparse-prt.m -o "$TMPDIR/test-metal3-sparse-prt"
// Run:
//   sudo RDNA4M_ALLOW=1 RDNA4M_TEST_INPROC=1 RDNA4M_TEST_USERCACHE="$TMPDIR/xlcache" "$TMPDIR/test-metal3-sparse-prt" /path/to/GarnetMetal.bundle
//   "$TMPDIR/test-metal3-sparse-prt" --reference
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#import <objc/runtime.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    float r, g, b, a;
    uint32_t sampleResident;
    uint32_t readResident;
    uint32_t pad0, pad1;
} ProbeOut;

typedef struct {
    uint32_t x, y, mip, pad;
    float r, g, b, a;
} WriteArgs;

typedef struct {
    uint32_t x, y, mip, outIndex;
} ReadArgs;

static void failf(NSString *msg) __attribute__((noreturn));
static void failf(NSString *msg) {
    fprintf(stderr, "METAL3_SPARSE_PRT_FAIL %s\n", msg.UTF8String);
    _exit(1);
}

static void require(BOOL cond, NSString *msg) {
    if (!cond) failf(msg);
}

static void finishOK(id<MTLCommandBuffer> cb, NSString *tag) {
    require(cb != nil, [NSString stringWithFormat:@"%@: nil command buffer", tag]);
    [cb commit];
    [cb waitUntilCompleted];
    require(cb.status == MTLCommandBufferStatusCompleted && !cb.error,
            [NSString stringWithFormat:@"%@: status=%lu err=%@", tag, (unsigned long)cb.status, cb.error]);
}

static void finishExpectError(id<MTLCommandBuffer> cb, NSString *tag) {
    require(cb != nil, [NSString stringWithFormat:@"%@: nil command buffer", tag]);
    [cb commit];
    [cb waitUntilCompleted];
    require(cb.status == MTLCommandBufferStatusError && cb.error != nil,
            [NSString stringWithFormat:@"%@: expected error status, got status=%lu err=%@",
             tag, (unsigned long)cb.status, cb.error]);
}

static BOOL nearf(float a, float b) {
    return fabsf(a - b) <= 0.02f;
}

static void dispatchWrite(id<MTLCommandQueue> q, id<MTLComputePipelineState> pso,
                          id<MTLTexture> tex, id<MTLFence> waitFence,
                          uint32_t x, uint32_t y, uint32_t mip,
                          float r, float g, float b, float a, NSString *tag) {
    WriteArgs args = { x, y, mip, 0u, r, g, b, a };
    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
    require(enc != nil, [NSString stringWithFormat:@"%@: compute encoder nil", tag]);
    if (waitFence) [enc waitForFence:waitFence];
    [enc setComputePipelineState:pso];
    [enc setTexture:tex atIndex:0];
    [enc setBytes:&args length:sizeof(args) atIndex:0];
    [enc dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [enc endEncoding];
    finishOK(cb, tag);
}

static ProbeOut dispatchProbe(id<MTLDevice> dev, id<MTLCommandQueue> q,
                              id<MTLComputePipelineState> pso, id<MTLTexture> tex,
                              uint32_t x, uint32_t y, uint32_t mip, NSString *tag) {
    id<MTLBuffer> outBuf = [dev newBufferWithLength:sizeof(ProbeOut) options:MTLResourceStorageModeShared];
    require(outBuf != nil, [NSString stringWithFormat:@"%@: outBuf nil", tag]);
    memset(outBuf.contents, 0xCC, sizeof(ProbeOut));
    ReadArgs args = { x, y, mip, 0u };
    id<MTLCommandBuffer> cb = [q commandBuffer];
    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
    require(enc != nil, [NSString stringWithFormat:@"%@: probe encoder nil", tag]);
    [enc setComputePipelineState:pso];
    [enc setTexture:tex atIndex:0];
    [enc setBuffer:outBuf offset:0 atIndex:0];
    [enc setBytes:&args length:sizeof(args) atIndex:1];
    [enc dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [enc endEncoding];
    finishOK(cb, tag);
    ProbeOut res;
    memcpy(&res, outBuf.contents, sizeof(res));
    return res;
}

int main(int argc, char **argv) {
    @autoreleasepool {
        setvbuf(stdout, NULL, _IONBF, 0);
        if (argc != 2) {
            fprintf(stderr, "Usage: %s BUNDLE|--reference\n", argv[0]);
            return 2;
        }
        BOOL reference = strcmp(argv[1], "--reference") == 0;
        id<MTLDevice> dev = nil;
        if (reference) {
            dev = MTLCreateSystemDefaultDevice();
        } else {
            setenv("RDNA4M_ALLOW", "1", 1);
            NSError *err = nil;
            if (![[NSBundle bundleWithPath:@(argv[1])] loadAndReturnError:&err])
                failf([NSString stringWithFormat:@"bundle load: %@", err]);
            Class cls = objc_getClass("GarnetDevice");
            require(cls != Nil, @"GarnetDevice missing");
            dev = [[cls alloc] init];
            if ([dev respondsToSelector:sel_registerName("initLimits")])
                ((void (*)(id, SEL))objc_msgSend)(dev, sel_registerName("initLimits"));
        }
        require(dev != nil, @"device unavailable");
        id<MTLCommandQueue> q = [dev newCommandQueue];
        require(q != nil, @"command queue unavailable");

        // ------------------------------------------------------------------
        // Part 1: EX01 Gate — NEVER claim MTLGPUFamilyApple6 (or Apple1..10) on Garnet
        // ------------------------------------------------------------------
        if (!reference) {
            require(![dev supportsFamily:MTLGPUFamilyApple6],
                    @"EX01 violation: supportsFamily(MTLGPUFamilyApple6) MUST be NO on Garnet");
            for (NSInteger f = (NSInteger)MTLGPUFamilyApple1; f <= 1010; f++) {
                require(![dev supportsFamily:(MTLGPUFamily)f],
                        [NSString stringWithFormat:@"EX01 violation: supportsFamily(%ld) must be NO", (long)f]);
            }
        }

        // ------------------------------------------------------------------
        // Part 2: 64 KiB Sparse Tile Queries & Coordinate Conversions
        // ------------------------------------------------------------------
        const NSUInteger tileBytes = [dev sparseTileSizeInBytes];
        require(tileBytes > 0, @"sparseTileSizeInBytes returned 0 (kernel ABI 1.13 RDNA4N_CAP_VM_PRT / RADV sparse missing?)");
        if (!reference) {
            require(tileBytes == 65536u,
                    [NSString stringWithFormat:@"expected 65536 B GFX12 sparse tile, got %lu", (unsigned long)tileBytes]);
        }

        const MTLSize tileRGBA8 = [dev sparseTileSizeWithTextureType:MTLTextureType2D
                                                         pixelFormat:MTLPixelFormatRGBA8Unorm
                                                         sampleCount:1];
        require(tileRGBA8.width > 0 && tileRGBA8.height > 0 && tileRGBA8.depth == 1,
                @"sparseTileSize(2D, RGBA8Unorm, 1) returned zero");
        if (!reference) {
            require(tileRGBA8.width == 128 && tileRGBA8.height == 128 && tileRGBA8.depth == 1,
                    [NSString stringWithFormat:@"expected 128x128x1 for RGBA8, got %lux%lux%lu",
                     (unsigned long)tileRGBA8.width, (unsigned long)tileRGBA8.height, (unsigned long)tileRGBA8.depth]);
            const MTLSize tR8 = [dev sparseTileSizeWithTextureType:MTLTextureType2D pixelFormat:MTLPixelFormatR8Unorm sampleCount:1];
            const MTLSize tF16 = [dev sparseTileSizeWithTextureType:MTLTextureType2D pixelFormat:MTLPixelFormatRGBA16Float sampleCount:1];
            const MTLSize tF32 = [dev sparseTileSizeWithTextureType:MTLTextureType2D pixelFormat:MTLPixelFormatRGBA32Float sampleCount:1];
            const MTLSize tBC1 = [dev sparseTileSizeWithTextureType:MTLTextureType2D pixelFormat:MTLPixelFormatBC1_RGBA sampleCount:1];
            const MTLSize tBC7 = [dev sparseTileSizeWithTextureType:MTLTextureType2D pixelFormat:MTLPixelFormatBC7_RGBAUnorm sampleCount:1];
            const MTLSize tMS4 = [dev sparseTileSizeWithTextureType:MTLTextureType2DMultisample pixelFormat:MTLPixelFormatRGBA8Unorm sampleCount:4];
            const MTLSize t3D = [dev sparseTileSizeWithTextureType:MTLTextureType3D pixelFormat:MTLPixelFormatRGBA8Unorm sampleCount:1];
            const MTLSize tBad = [dev sparseTileSizeWithTextureType:MTLTextureType1D pixelFormat:MTLPixelFormatRGBA8Unorm sampleCount:1];
            require(tR8.width == 256 && tR8.height == 256, @"R8Unorm 2D tile mismatch");
            require(tF16.width == 128 && tF16.height == 64, @"RGBA16Float 2D tile mismatch");
            require(tF32.width == 64 && tF32.height == 64, @"RGBA32Float 2D tile mismatch");
            require(tBC1.width == 512 && tBC1.height == 256, @"BC1_RGBA 2D tile mismatch");
            require(tBC7.width == 256 && tBC7.height == 256, @"BC7_RGBAUnorm 2D tile mismatch");
            require(tMS4.width == 64 && tMS4.height == 64, @"RGBA8 4x MSAA tile mismatch");
            require(t3D.width == 32 && t3D.height == 32 && t3D.depth == 16, @"RGBA8 3D tile mismatch");
            require(tBad.width == 0 && tBad.height == 0 && tBad.depth == 0, @"1D sparse query must return {0,0,0}");
        }

        // Pixel <-> Tile conversion check (Outward & Inward).
        const MTLRegion pxReg = MTLRegionMake2D(tileRGBA8.width / 2, tileRGBA8.height / 2,
                                                tileRGBA8.width * 2, tileRGBA8.height * 2);
        MTLRegion tileOut = MTLRegionMake2D(0, 0, 0, 0);
        MTLRegion tileIn = MTLRegionMake2D(0, 0, 0, 0);
        MTLRegion pxBack = MTLRegionMake2D(0, 0, 0, 0);
        [dev convertSparsePixelRegions:&pxReg toTileRegions:&tileOut withTileSize:tileRGBA8
                         alignmentMode:MTLSparseTextureRegionAlignmentModeOutward numRegions:1];
        [dev convertSparsePixelRegions:&pxReg toTileRegions:&tileIn withTileSize:tileRGBA8
                         alignmentMode:MTLSparseTextureRegionAlignmentModeInward numRegions:1];
        [dev convertSparseTileRegions:&tileIn toPixelRegions:&pxBack withTileSize:tileRGBA8 numRegions:1];
        require(tileOut.origin.x == 0 && tileOut.origin.y == 0 && tileOut.size.width == 3 && tileOut.size.height == 3,
                @"convertSparsePixelRegions Outward mismatch");
        require(tileIn.origin.x == 1 && tileIn.origin.y == 1 && tileIn.size.width == 1 && tileIn.size.height == 1,
                @"convertSparsePixelRegions Inward mismatch");
        require(pxBack.origin.x == tileRGBA8.width && pxBack.origin.y == tileRGBA8.height &&
                pxBack.size.width == tileRGBA8.width && pxBack.size.height == tileRGBA8.height,
                @"convertSparseTileRegions round-trip mismatch");

        // ------------------------------------------------------------------
        // Part 3: Create Sparse Heap & 4x4-Tile Mipmapped Sparse Texture
        // ------------------------------------------------------------------
        const NSUInteger texW = tileRGBA8.width * 4u;
        const NSUInteger texH = tileRGBA8.height * 4u;
        const NSUInteger mipCount = 5u; // L0=4x4, L1=2x2, L2=1x1, L3..L4=mip tail

        MTLHeapDescriptor *hd = [MTLHeapDescriptor new];
        hd.type = MTLHeapTypeSparse;
        hd.storageMode = MTLStorageModePrivate;
        hd.size = 16u * tileBytes;
#if defined(__MAC_13_0)
        if (@available(macOS 13.0, *)) {
            if ([hd respondsToSelector:@selector(setSparsePageSize:)] && !reference)
                hd.sparsePageSize = MTLSparsePageSize64;
        }
#endif
        id<MTLHeap> sparseHeap = [dev newHeapWithDescriptor:hd];
        require(sparseHeap != nil, @"newHeapWithDescriptor(MTLHeapTypeSparse) returned nil");

        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                      width:texW height:texH mipmapped:YES];
        td.mipmapLevelCount = mipCount;
        td.storageMode = MTLStorageModePrivate;
        td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
        id<MTLTexture> sparseTex = [sparseHeap newTextureWithDescriptor:td];
        require(sparseTex != nil, @"sparseHeap newTextureWithDescriptor returned nil");
        require(sparseTex.isSparse, @"sparseTex.isSparse must be YES");
        require(sparseTex.firstMipmapInTail == 3u,
                [NSString stringWithFormat:@"expected firstMipmapInTail=3, got %lu", (unsigned long)sparseTex.firstMipmapInTail]);
        require(sparseTex.tailSizeInBytes >= tileBytes && (sparseTex.tailSizeInBytes % tileBytes) == 0u,
                [NSString stringWithFormat:@"unexpected tailSizeInBytes=%lu", (unsigned long)sparseTex.tailSizeInBytes]);

        // ------------------------------------------------------------------
        // Part 4: Compile MSL sparse_color<float4> Shaders
        // ------------------------------------------------------------------
        NSString *msl =
            @"#include <metal_stdlib>\n"
             "using namespace metal;\n"
             "struct ProbeOut { float r, g, b, a; uint sampleResident; uint readResident; uint pad0, pad1; };\n"
             "struct WriteArgs { uint x, y, mip, pad; float r, g, b, a; };\n"
             "struct ReadArgs { uint x, y, mip, outIndex; };\n"
             "kernel void sparse_write(texture2d<float, access::write> tex [[texture(0)]],\n"
             "                         constant WriteArgs &a [[buffer(0)]]) {\n"
             "    tex.write(float4(a.r, a.g, a.b, a.a), uint2(a.x, a.y), a.mip);\n"
             "}\n"
             "kernel void sparse_probe(texture2d<float, access::sample> tex [[texture(0)]],\n"
             "                         device ProbeOut *out [[buffer(0)]],\n"
             "                         constant ReadArgs &a [[buffer(1)]]) {\n"
             "    constexpr sampler s(coord::normalized, address::clamp_to_edge, filter::nearest, mip_filter::nearest);\n"
             "    float2 uv = float2((float(a.x) + 0.5f) / float(tex.get_width(a.mip)),\n"
             "                       (float(a.y) + 0.5f) / float(tex.get_height(a.mip)));\n"
             "    sparse_color<float4> sc = tex.sparse_sample(s, uv, level(float(a.mip)));\n"
             "    sparse_color<float4> sr = tex.sparse_read(uint2(a.x, a.y), a.mip);\n"
             "    float4 v = sr.value();\n"
             "    ProbeOut r;\n"
             "    r.r = v.x; r.g = v.y; r.b = v.z; r.a = v.w;\n"
             "    r.sampleResident = sc.resident() ? 1u : 0u;\n"
             "    r.readResident = sr.resident() ? 1u : 0u;\n"
             "    r.pad0 = 0u; r.pad1 = 0u;\n"
             "    out[a.outIndex] = r;\n"
             "}\n";
        NSError *err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:msl options:nil error:&err];
        require(lib != nil, [NSString stringWithFormat:@"sparse MSL compile failed: %@", err]);
        id<MTLComputePipelineState> writePso =
            [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"sparse_write"] error:&err];
        require(writePso != nil, [NSString stringWithFormat:@"sparse_write PSO failed: %@", err]);
        id<MTLComputePipelineState> probePso =
            [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"sparse_probe"] error:&err];
        require(probePso != nil, [NSString stringWithFormat:@"sparse_probe PSO failed: %@", err]);

        // Helper tile center coordinates (L0) and tail coordinate (L3).
        const uint32_t cx0 = (uint32_t)(tileRGBA8.width / 2u);
        const uint32_t cy0 = (uint32_t)(tileRGBA8.height / 2u);
        const uint32_t cx1 = (uint32_t)(tileRGBA8.width + tileRGBA8.width / 2u);
        const uint32_t cy1 = (uint32_t)(tileRGBA8.height + tileRGBA8.height / 2u);
        const uint32_t cx2 = (uint32_t)(tileRGBA8.width * 2u + tileRGBA8.width / 2u);
        const uint32_t cx3 = (uint32_t)(tileRGBA8.width * 3u + tileRGBA8.width / 2u);
        const uint32_t cy3 = (uint32_t)(tileRGBA8.height * 3u + tileRGBA8.height / 2u);

        // ------------------------------------------------------------------
        // Part 5: Unmapped Baseline — Write Discard & Zero Read (resident == false)
        // ------------------------------------------------------------------
        dispatchWrite(q, writePso, sparseTex, nil, cx0, cy0, 0u, 0.9f, 0.8f, 0.7f, 1.0f, @"unmapped_write_discard");
        ProbeOut p0 = dispatchProbe(dev, q, probePso, sparseTex, cx0, cy0, 0u, @"unmapped_probe");
        require(p0.sampleResident == 0u && p0.readResident == 0u,
                [NSString stringWithFormat:@"unmapped tile reported resident (%u/%u)", p0.sampleResident, p0.readResident]);
        require(nearf(p0.r, 0.0f) && nearf(p0.g, 0.0f) && nearf(p0.b, 0.0f) && nearf(p0.a, 0.0f),
                [NSString stringWithFormat:@"unmapped tile read non-zero (%.3f,%.3f,%.3f,%.3f)", p0.r, p0.g, p0.b, p0.a]);

        // ------------------------------------------------------------------
        // Part 6: Map Tiles (0,0) + (2,1) + Mip Tail via MTLResourceStateCommandEncoder & Fence
        // ------------------------------------------------------------------
        id<MTLFence> fence = [dev newFence];
        require(fence != nil, @"newFence returned nil");
        {
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLResourceStateCommandEncoder> rse = [cb resourceStateCommandEncoder];
            require(rse != nil, @"resourceStateCommandEncoder returned nil");
            [rse updateTextureMapping:sparseTex mode:MTLSparseTextureMappingModeMap
                               region:MTLRegionMake2D(0, 0, 1, 1) mipLevel:0 slice:0];
            MTLRegion batchRegs[2] = { MTLRegionMake2D(2, 1, 1, 1), MTLRegionMake2D(0, 0, 1, 1) };
            NSUInteger batchMips[2] = { 0u, sparseTex.firstMipmapInTail };
            NSUInteger batchSlices[2] = { 0u, 0u };
            [rse updateTextureMappings:sparseTex mode:MTLSparseTextureMappingModeMap
                               regions:batchRegs mipLevels:batchMips slices:batchSlices numRegions:2];
            [rse updateFence:fence];
            [rse endEncoding];
            finishOK(cb, @"map_tiles_and_tail");
        }

        dispatchWrite(q, writePso, sparseTex, fence, cx0, cy0, 0u, 1.0f, 0.0f, 0.0f, 1.0f, @"write_tile_0_0_red");
        dispatchWrite(q, writePso, sparseTex, nil,   cx2, cy1, 0u, 0.0f, 1.0f, 0.0f, 1.0f, @"write_tile_2_1_green");
        dispatchWrite(q, writePso, sparseTex, nil,   cx1, cy0, 0u, 0.0f, 0.0f, 1.0f, 1.0f, @"write_unmapped_1_0_discard");
        dispatchWrite(q, writePso, sparseTex, nil,   4u,  4u,  (uint32_t)sparseTex.firstMipmapInTail,
                      0.25f, 0.5f, 0.75f, 1.0f, @"write_mip_tail");

        ProbeOut p00 = dispatchProbe(dev, q, probePso, sparseTex, cx0, cy0, 0u, @"probe_0_0");
        ProbeOut p21 = dispatchProbe(dev, q, probePso, sparseTex, cx2, cy1, 0u, @"probe_2_1");
        ProbeOut p10 = dispatchProbe(dev, q, probePso, sparseTex, cx1, cy0, 0u, @"probe_1_0");
        ProbeOut p33 = dispatchProbe(dev, q, probePso, sparseTex, cx3, cy3, 0u, @"probe_3_3");
        ProbeOut pTail = dispatchProbe(dev, q, probePso, sparseTex, 4u, 4u, (uint32_t)sparseTex.firstMipmapInTail, @"probe_tail");

        require(p00.sampleResident == 1u && p00.readResident == 1u &&
                nearf(p00.r, 1.0f) && nearf(p00.g, 0.0f) && nearf(p00.b, 0.0f) && nearf(p00.a, 1.0f),
                @"mapped tile (0,0) resident/color mismatch");
        require(p21.sampleResident == 1u && p21.readResident == 1u &&
                nearf(p21.r, 0.0f) && nearf(p21.g, 1.0f) && nearf(p21.b, 0.0f) && nearf(p21.a, 1.0f),
                @"mapped tile (2,1) resident/color mismatch");
        require(p10.sampleResident == 0u && p10.readResident == 0u &&
                nearf(p10.r, 0.0f) && nearf(p10.g, 0.0f) && nearf(p10.b, 0.0f) && nearf(p10.a, 0.0f),
                @"unmapped tile (1,0) must stay non-resident zero");
        require(p33.sampleResident == 0u && p33.readResident == 0u &&
                nearf(p33.r, 0.0f) && nearf(p33.g, 0.0f) && nearf(p33.b, 0.0f) && nearf(p33.a, 0.0f),
                @"unmapped tile (3,3) must stay non-resident zero");
        require(pTail.sampleResident == 1u && pTail.readResident == 1u &&
                nearf(pTail.r, 0.25f) && nearf(pTail.g, 0.5f) && nearf(pTail.b, 0.75f) && nearf(pTail.a, 1.0f),
                @"mapped mip tail resident/color mismatch");

        // ------------------------------------------------------------------
        // Part 7: Indirect Mapping (updateTextureMapping:mode:indirectBuffer:indirectBufferOffset:)
        // ------------------------------------------------------------------
        {
            MTLMapIndirectArguments ia = { 1u, 1u, 0u, 1u, 1u, 1u, 0u, 0u };
            id<MTLBuffer> indBuf = [dev newBufferWithBytes:&ia length:sizeof(ia) options:MTLResourceStorageModeShared];
            require(indBuf != nil, @"indirect arg buffer nil");
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLResourceStateCommandEncoder> rse = [cb resourceStateCommandEncoder];
            require(rse != nil, @"indirect rse nil");
            [rse updateTextureMapping:sparseTex mode:MTLSparseTextureMappingModeMap
                       indirectBuffer:indBuf indirectBufferOffset:0];
            [rse endEncoding];
            finishOK(cb, @"indirect_map_1_1");
        }
        dispatchWrite(q, writePso, sparseTex, nil, cx1, cy1, 0u, 1.0f, 1.0f, 0.0f, 1.0f, @"write_tile_1_1_yellow");
        ProbeOut p11 = dispatchProbe(dev, q, probePso, sparseTex, cx1, cy1, 0u, @"probe_1_1");
        require(p11.sampleResident == 1u && p11.readResident == 1u &&
                nearf(p11.r, 1.0f) && nearf(p11.g, 1.0f) && nearf(p11.b, 0.0f) && nearf(p11.a, 1.0f),
                @"indirect-mapped tile (1,1) resident/color mismatch");

        // ------------------------------------------------------------------
        // Part 8: Unmap Tile (0,0) & Verify Guard Tiles (2,1) / (1,1) Preserved
        // ------------------------------------------------------------------
        {
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLResourceStateCommandEncoder> rse = [cb resourceStateCommandEncoder];
            [rse updateTextureMapping:sparseTex mode:MTLSparseTextureMappingModeUnmap
                               region:MTLRegionMake2D(0, 0, 1, 1) mipLevel:0 slice:0];
            [rse endEncoding];
            finishOK(cb, @"unmap_0_0");
        }
        ProbeOut p00After = dispatchProbe(dev, q, probePso, sparseTex, cx0, cy0, 0u, @"probe_0_0_after_unmap");
        ProbeOut p21Guard = dispatchProbe(dev, q, probePso, sparseTex, cx2, cy1, 0u, @"probe_2_1_guard");
        ProbeOut p11Guard = dispatchProbe(dev, q, probePso, sparseTex, cx1, cy1, 0u, @"probe_1_1_guard");
        require(p00After.sampleResident == 0u && p00After.readResident == 0u &&
                nearf(p00After.r, 0.0f) && nearf(p00After.g, 0.0f) && nearf(p00After.b, 0.0f) && nearf(p00After.a, 0.0f),
                @"unmapped tile (0,0) did not revert to non-resident zero");
        require(p21Guard.sampleResident == 1u && nearf(p21Guard.g, 1.0f), @"guard tile (2,1) corrupted after unmap");
        require(p11Guard.sampleResident == 1u && nearf(p11Guard.r, 1.0f) && nearf(p11Guard.g, 1.0f),
                @"guard tile (1,1) corrupted after unmap");

        // ------------------------------------------------------------------
        // Part 9: Negative / Error Refusal Checks (Garnet controlled NSError)
        // ------------------------------------------------------------------
        if (!reference) {
            // 9A: Out-of-bounds tile region
            id<MTLCommandBuffer> cbOob = [q commandBuffer];
            id<MTLResourceStateCommandEncoder> rseOob = [cbOob resourceStateCommandEncoder];
            [rseOob updateTextureMapping:sparseTex mode:MTLSparseTextureMappingModeMap
                                  region:MTLRegionMake2D(10, 0, 1, 1) mipLevel:0 slice:0];
            [rseOob endEncoding];
            finishExpectError(cbOob, @"neg_oob_tile");

            // 9B: Non-sparse texture rejected
            MTLTextureDescriptor *denseDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                                 width:128 height:128 mipmapped:NO];
            denseDesc.storageMode = MTLStorageModePrivate;
            denseDesc.usage = MTLTextureUsageShaderRead;
            id<MTLTexture> denseTex = [dev newTextureWithDescriptor:denseDesc];
            require(denseTex != nil && !denseTex.isSparse, @"dense control texture failed");
            id<MTLCommandBuffer> cbDense = [q commandBuffer];
            id<MTLResourceStateCommandEncoder> rseDense = [cbDense resourceStateCommandEncoder];
            [rseDense updateTextureMapping:denseTex mode:MTLSparseTextureMappingModeMap
                                    region:MTLRegionMake2D(0, 0, 1, 1) mipLevel:0 slice:0];
            [rseDense endEncoding];
            finishExpectError(cbDense, @"neg_non_sparse");

            // 9C: mipLevel > firstMipmapInTail rejected
            id<MTLCommandBuffer> cbTail = [q commandBuffer];
            id<MTLResourceStateCommandEncoder> rseTail = [cbTail resourceStateCommandEncoder];
            [rseTail updateTextureMapping:sparseTex mode:MTLSparseTextureMappingModeMap
                                   region:MTLRegionMake2D(0, 0, 1, 1) mipLevel:sparseTex.firstMipmapInTail + 1u slice:0];
            [rseTail endEncoding];
            finishExpectError(cbTail, @"neg_mip_beyond_first_tail");
        }

        printf("METAL3_SPARSE_PRT_PASS tile_bytes=%lu tile=%lux%lu first_tail=%lu apple6=%d\n",
               (unsigned long)tileBytes, (unsigned long)tileRGBA8.width, (unsigned long)tileRGBA8.height,
               (unsigned long)sparseTex.firstMipmapInTail, [dev supportsFamily:MTLGPUFamilyApple6] ? 1 : 0);
        return 0;
    }
}
