// test-metal3-mesh-shaders.m: live macOS / RX 9060 XT verification for Task 6.2
// (RDNA4MeshRenderPipelineState & Mesh/Object Draw Encoder Runtime:
//  mesh-only triangle + PerPrimitiveEXT, object+mesh+16 KiB payload with multi-group grid,
//  drawMeshThreads: + object threadgroup memory, GPU-driven drawMeshThreadgroupsWithIndirectBuffer:
//  including zero-grid no-op + non-zero offset, MTLLinkedFunctions.privateFunctions allowed,
//  ARC early-release lifetime, Footnote 10 render-fn-ptr refusal, and payload/grid/indirect negative checks).
//
// Build on macOS (verify BOTH -O0 and -O2 for ARC lifetime):
//   clang -arch x86_64 -mmacosx-version-min=13.0 -fobjc-arc -O2 -Wall -Wextra -Werror -framework Foundation -framework Metal src/garnet-metal/test-metal3-mesh-shaders.m -o "$TMPDIR/test-metal3-mesh-shaders"
// Run:
//   RDNA4M_ALLOW=1 RDNA4M_TEST_INPROC=1 RDNA4M_TEST_USERCACHE="$TMPDIR/xlcache" "$TMPDIR/test-metal3-mesh-shaders" /path/to/GarnetMetal.bundle
//   "$TMPDIR/test-metal3-mesh-shaders" --reference
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#import <objc/runtime.h>

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

@interface NSObject (GarnetTestAccelInit)
- (instancetype)initWithAcceleratorPort:(uint32_t)port;
@end

static id<MTLDevice> createGarnetDevice(Class cls) {
    id<MTLDevice> dev = nil;
    void *iokit = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_LAZY);
    if (iokit) {
        typedef void *(*io_matching_fn)(const char *);
        typedef uint32_t (*io_get_svc_fn)(uint32_t, void *);
        typedef int (*io_rel_fn)(uint32_t);
        io_matching_fn matchFn = (io_matching_fn)dlsym(iokit, "IOServiceMatching");
        io_get_svc_fn getSvcFn = (io_get_svc_fn)dlsym(iokit, "IOServiceGetMatchingService");
        io_rel_fn relFn = (io_rel_fn)dlsym(iokit, "IOObjectRelease");
        if (matchFn && getSvcFn) {
            void *matching = matchFn("GarnetAccelerator");
            uint32_t port = matching ? getSvcFn(0u /* kIOMainPortDefault */, matching) : 0u;
            if (port) {
                if ([cls instancesRespondToSelector:@selector(initWithAcceleratorPort:)])
                    dev = [[cls alloc] initWithAcceleratorPort:port];
                if (relFn) relFn(port);
            }
        }
    }
    if (!dev) {
        dev = [[cls alloc] init];
        if (dev && [dev respondsToSelector:sel_registerName("initLimits")])
            ((void (*)(id, SEL))objc_msgSend)(dev, sel_registerName("initLimits"));
    }
    return dev;
}

static void failf(NSString *msg) __attribute__((noreturn));
static void failf(NSString *msg) {
    fprintf(stderr, "METAL3_MESH_FAIL %s\n", msg.UTF8String);
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
    require(cb.status == MTLCommandBufferStatusError || cb.error != nil,
            [NSString stringWithFormat:@"%@: expected error status, got status=%lu err=%@",
             tag, (unsigned long)cb.status, cb.error]);
}

static id<MTLTexture> makeRenderTarget(id<MTLDevice> dev, NSUInteger w, NSUInteger h, NSString *tag) {
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                  width:w height:h mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModePrivate;
    id<MTLTexture> rt = [dev newTextureWithDescriptor:td];
    require(rt != nil, [NSString stringWithFormat:@"%@: render target nil", tag]);
    return rt;
}

static void blitReadback(id<MTLCommandBuffer> cb, id<MTLTexture> rt, id<MTLBuffer> readback, NSUInteger w, NSUInteger h, NSString *tag) {
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    require(blit != nil, [NSString stringWithFormat:@"%@: blit encoder nil", tag]);
    const NSUInteger bpr = w * 4;
    [blit copyFromTexture:rt sourceSlice:0 sourceLevel:0
             sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1)
                 toBuffer:readback destinationOffset:0 destinationBytesPerRow:bpr destinationBytesPerImage:bpr * h];
    [blit endEncoding];
}

static void verifyPixelRect(const uint8_t *px, NSUInteger w,
                            NSUInteger x0, NSUInteger y0, NSUInteger x1, NSUInteger y1,
                            int r, int g, int b, int a, NSString *tag) {
    const int expRgba[4] = { r, g, b, a };
    for (NSUInteger y = y0; y < y1; y++) {
        for (NSUInteger x = x0; x < x1; x++) {
            const NSUInteger idx = (y * w + x) * 4u;
            for (int ch = 0; ch < 4; ch++) {
                int diff = (int)px[idx + (NSUInteger)ch] - expRgba[ch];
                if (diff < 0) diff = -diff;
                require(diff <= 2,
                        [NSString stringWithFormat:@"%@ (%lu,%lu) ch %d: got %u expected %d",
                         tag, (unsigned long)x, (unsigned long)y, ch, px[idx + (NSUInteger)ch], expRgba[ch]]);
            }
        }
    }
}

static NSString *const kMeshShadersMSL = @""
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "\n"
    "struct VertOut {\n"
    "    float4 position [[position]];\n"
    "    float4 vcolor [[user(locn0)]];\n"
    "};\n"
    "struct PrimOut {\n"
    "    float4 pcolor [[flat, user(locn1)]];\n"
    "};\n"
    "struct FragIn {\n"
    "    VertOut v;\n"
    "    PrimOut p;\n"
    "};\n"
    "using QuadMesh = metal::mesh<VertOut, PrimOut, 4, 2, metal::topology::triangle>;\n"
    "\n"
    "// 1. Mesh-only fullscreen quad (2 triangles) with distinct per-vertex + per-primitive colors.\n"
    "[[mesh]]\n"
    "void mesh_only_per_prim(QuadMesh out,\n"
    "                        constant float4 *vTint [[buffer(0)]],\n"
    "                        uint tid [[thread_index_in_threadgroup]]) {\n"
    "    if (tid == 0) {\n"
    "        const float2 pos[4] = {\n"
    "            float2(-1.0f, -1.0f), float2(1.0f, -1.0f),\n"
    "            float2(-1.0f,  1.0f), float2(1.0f,  1.0f)\n"
    "        };\n"
    "        float4 vc = vTint[0];\n"
    "        for (uint i = 0; i < 4; i++) {\n"
    "            VertOut v; v.position = float4(pos[i], 0.0f, 1.0f); v.vcolor = vc;\n"
    "            out.set_vertex(i, v);\n"
    "        }\n"
    "        // Triangle 0 (lower-right) + Triangle 1 (upper-left) with same per-primitive green boost.\n"
    "        out.set_index(0, 0); out.set_index(1, 1); out.set_index(2, 3);\n"
    "        out.set_index(3, 0); out.set_index(4, 3); out.set_index(5, 2);\n"
    "        PrimOut p; p.pcolor = float4(0.0f, 0.5f, 0.25f, 0.0f);\n"
    "        out.set_primitive(0, p);\n"
    "        out.set_primitive(1, p);\n"
    "        out.set_primitive_count(2);\n"
    "    }\n"
    "}\n"
    "\n"
    "fragment float4 frag_per_prim(FragIn in [[stage_in]]) {\n"
    "    return clamp(in.v.vcolor + in.p.pcolor, 0.0f, 1.0f);\n"
    "}\n"
    "\n"
    "// 2. Full 16,384-byte object_data payload (4096 uints): writes word[0] & word[4095].\n"
    "struct Payload16K {\n"
    "    uint words[4096];\n"
    "};\n"
    "\n"
    "[[object, max_total_threadgroups_per_mesh_grid(4)]]\n"
    "void obj_payload_16k(object_data Payload16K &payload [[payload]],\n"
    "                     mesh_grid_properties mgp,\n"
    "                     constant uint4 *cfg [[buffer(0)]],\n"
    "                     uint tid [[thread_index_in_threadgroup]],\n"
    "                     uint tgid [[threadgroup_position_in_grid]]) {\n"
    "    if (tid == 0) {\n"
    "        payload.words[0] = cfg[0].x + tgid;\n"
    "        payload.words[4095] = cfg[0].y + tgid * 16u;\n"
    "        mgp.set_threadgroups_per_grid(uint3(cfg[0].z, 1u, 1u));\n"
    "    }\n"
    "}\n"
    "\n"
    "[[mesh]]\n"
    "void mesh_payload_16k(QuadMesh out,\n"
    "                      const object_data Payload16K &payload [[payload]],\n"
    "                      uint tid [[thread_index_in_threadgroup]],\n"
    "                      uint tgid [[threadgroup_position_in_grid]]) {\n"
    "    if (tid == 0) {\n"
    "        // Group 0 covers left half [-1, 0], group 1 covers right half [0, +1].\n"
    "        float x0 = (tgid == 0) ? -1.0f : 0.0f;\n"
    "        float x1 = (tgid == 0) ?  0.0f : 1.0f;\n"
    "        const float2 pos[4] = {\n"
    "            float2(x0, -1.0f), float2(x1, -1.0f),\n"
    "            float2(x0,  1.0f), float2(x1,  1.0f)\n"
    "        };\n"
    "        float r = float(payload.words[0] & 255u) / 255.0f;\n"
    "        float b = float(payload.words[4095] & 255u) / 255.0f;\n"
    "        float g = (tgid == 0) ? 0.25f : 0.75f;\n"
    "        for (uint i = 0; i < 4; i++) {\n"
    "            VertOut v; v.position = float4(pos[i], 0.0f, 1.0f); v.vcolor = float4(r, g, b, 1.0f);\n"
    "            out.set_vertex(i, v);\n"
    "        }\n"
    "        out.set_index(0, 0); out.set_index(1, 1); out.set_index(2, 3);\n"
    "        out.set_index(3, 0); out.set_index(4, 3); out.set_index(5, 2);\n"
    "        PrimOut p; p.pcolor = float4(0.0f, 0.0f, 0.0f, 0.0f);\n"
    "        out.set_primitive(0, p);\n"
    "        out.set_primitive(1, p);\n"
    "        out.set_primitive_count(2);\n"
    "    }\n"
    "}\n"
    "\n"
    "// 3. Object shader using threadgroup memory + drawMeshThreads: (emits 1 quad per object group).\n"
    "struct SmallPayload {\n"
    "    uint4 colorBytes;\n"
    "};\n"
    "\n"
    "[[object]]\n"
    "void obj_tgmem_threads(object_data SmallPayload &payload [[payload]],\n"
    "                       mesh_grid_properties mgp,\n"
    "                       threadgroup uint *tgScratch [[threadgroup(0)]],\n"
    "                       uint tid [[thread_index_in_threadgroup]],\n"
    "                       uint tgid [[threadgroup_position_in_grid]]) {\n"
    "    if (tid == 0) {\n"
    "        tgScratch[0] = (tgid == 0) ? 64u : 192u;\n"
    "    }\n"
    "    threadgroup_barrier(mem_flags::mem_threadgroup);\n"
    "    if (tid == 0) {\n"
    "        payload.colorBytes = uint4(tgScratch[0], tgid * 128u, 255u, 255u);\n"
    "        mgp.set_threadgroups_per_grid(uint3(1u, 1u, 1u));\n"
    "    }\n"
    "}\n"
    "\n"
    "[[mesh]]\n"
    "void mesh_tgmem_threads(QuadMesh out,\n"
    "                        const object_data SmallPayload &payload [[payload]],\n"
    "                        uint tid [[thread_index_in_threadgroup]]) {\n"
    "    if (tid == 0) {\n"
    "        const bool right = payload.colorBytes.x > 128u;\n"
    "        float x0 = right ? 0.0f : -1.0f;\n"
    "        float x1 = right ? 1.0f :  0.0f;\n"
    "        const float2 pos[4] = {\n"
    "            float2(x0, -1.0f), float2(x1, -1.0f),\n"
    "            float2(x0,  1.0f), float2(x1,  1.0f)\n"
    "        };\n"
    "        float4 c = float4(payload.colorBytes) / 255.0f;\n"
    "        for (uint i = 0; i < 4; i++) {\n"
    "            VertOut v; v.position = float4(pos[i], 0.0f, 1.0f); v.vcolor = c;\n"
    "            out.set_vertex(i, v);\n"
    "        }\n"
    "        out.set_index(0, 0); out.set_index(1, 1); out.set_index(2, 3);\n"
    "        out.set_index(3, 0); out.set_index(4, 3); out.set_index(5, 2);\n"
    "        PrimOut p; p.pcolor = float4(0.0f);\n"
    "        out.set_primitive(0, p);\n"
    "        out.set_primitive(1, p);\n"
    "        out.set_primitive_count(2);\n"
    "    }\n"
    "}\n"
    "\n"
    "// 4. Compute kernel populating indirect mesh draw arguments (zero-grid @ 0 B, {2,1,1} @ 16 B).\n"
    "kernel void fill_mesh_indirect(device uint *args [[buffer(0)]],\n"
    "                               uint tid [[thread_position_in_grid]]) {\n"
    "    if (tid == 0) {\n"
    "        // Offset 0: zero-grid no-op {0, 0, 0}\n"
    "        args[0] = 0u; args[1] = 0u; args[2] = 0u; args[3] = 0u;\n"
    "        // Offset 16: {1, 1, 1} for mesh-only or object+mesh indirect draw\n"
    "        args[4] = 1u; args[5] = 1u; args[6] = 1u; args[7] = 0u;\n"
    "    }\n"
    "}\n"
    "\n"
    "// 5. Footnote 10: privateFunctions helper (ALLOWED) vs visible function pointer (FORBIDDEN).\n"
    "[[visible]] float4 helper_add_blue(float4 c) {\n"
    "    return float4(c.x, c.y, 1.0f, 1.0f);\n"
    "}\n"
    "\n"
    "[[mesh]]\n"
    "void mesh_with_private_helper(QuadMesh out, uint tid [[thread_index_in_threadgroup]]) {\n"
    "    if (tid == 0) {\n"
    "        const float2 pos[4] = {\n"
    "            float2(-1.0f, -1.0f), float2(1.0f, -1.0f),\n"
    "            float2(-1.0f,  1.0f), float2(1.0f,  1.0f)\n"
    "        };\n"
    "        float4 c = helper_add_blue(float4(0.5f, 0.25f, 0.0f, 1.0f));\n"
    "        for (uint i = 0; i < 4; i++) {\n"
    "            VertOut v; v.position = float4(pos[i], 0.0f, 1.0f); v.vcolor = c;\n"
    "            out.set_vertex(i, v);\n"
    "        }\n"
    "        out.set_index(0, 0); out.set_index(1, 1); out.set_index(2, 3);\n"
    "        out.set_index(3, 0); out.set_index(4, 3); out.set_index(5, 2);\n"
    "        PrimOut p; p.pcolor = float4(0.0f);\n"
    "        out.set_primitive(0, p);\n"
    "        out.set_primitive(1, p);\n"
    "        out.set_primitive_count(2);\n"
    "    }\n"
    "}\n";

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
            setenv("RDNA4M_TEST_INPROC", "1", 1);
            NSError *err = nil;
            if (![[NSBundle bundleWithPath:@(argv[1])] loadAndReturnError:&err])
                failf([NSString stringWithFormat:@"bundle load: %@", err]);
            Class cls = objc_getClass("GarnetDevice");
            require(cls != Nil, @"GarnetDevice missing");
            dev = createGarnetDevice(cls);
        }
        require(dev != nil, @"device unavailable");
        BOOL supportsMetal3 = [dev supportsFamily:MTLGPUFamilyMetal3];
        printf("METAL3_MESH_INFO mode=%s supportsMetal3=%d\n",
               reference ? "reference" : "garnet", supportsMetal3 ? 1 : 0);
        require([dev respondsToSelector:@selector(newRenderPipelineStateWithMeshDescriptor:options:reflection:error:)],
                @"device does not respond to newRenderPipelineStateWithMeshDescriptor:options:reflection:error:");
        if (!reference) {
            require(![dev supportsFamily:MTLGPUFamilyApple6],
                    @"EX01 violation: supportsFamily(MTLGPUFamilyApple6) must stay NO on Garnet");
        }

        id<MTLCommandQueue> q = [dev newCommandQueue];
        require(q != nil, @"command queue unavailable");

        MTLCompileOptions *opts = [MTLCompileOptions new];
        opts.languageVersion = MTLLanguageVersion3_0;
        NSError *err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:kMeshShadersMSL options:opts error:&err];
        require(lib != nil, [NSString stringWithFormat:@"MSL 3.0 compile failed: %@", err]);

        id<MTLFunction> fnMeshOnly = [lib newFunctionWithName:@"mesh_only_per_prim"];
        id<MTLFunction> fnFrag = [lib newFunctionWithName:@"frag_per_prim"];
        id<MTLFunction> fnObj16K = [lib newFunctionWithName:@"obj_payload_16k"];
        id<MTLFunction> fnMesh16K = [lib newFunctionWithName:@"mesh_payload_16k"];
        id<MTLFunction> fnObjTg = [lib newFunctionWithName:@"obj_tgmem_threads"];
        id<MTLFunction> fnMeshTg = [lib newFunctionWithName:@"mesh_tgmem_threads"];
        id<MTLFunction> fnFillInd = [lib newFunctionWithName:@"fill_mesh_indirect"];
        id<MTLFunction> fnHelper = [lib newFunctionWithName:@"helper_add_blue"];
        id<MTLFunction> fnMeshPriv = [lib newFunctionWithName:@"mesh_with_private_helper"];
        require(fnMeshOnly && fnFrag && fnObj16K && fnMesh16K && fnObjTg && fnMeshTg &&
                fnFillInd && fnHelper && fnMeshPriv, @"failed to load MSL 3.0 functions");

        const NSUInteger W = 32u, H = 32u, bytes = W * H * 4u;
        id<MTLBuffer> readback = [dev newBufferWithLength:bytes options:MTLResourceStorageModeShared];
        require(readback && readback.contents, @"readback buffer nil");
        uint8_t pixels[32u * 32u * 4u];

        // -----------------------------------------------------------------------------------------
        // Test 1: Mesh-only triangle quad + PerPrimitiveEXT
        // -----------------------------------------------------------------------------------------
        MTLMeshRenderPipelineDescriptor *d1 = [MTLMeshRenderPipelineDescriptor new];
        d1.meshFunction = fnMeshOnly;
        d1.fragmentFunction = fnFrag;
        d1.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        id<MTLRenderPipelineState> psoMeshOnly =
            [dev newRenderPipelineStateWithMeshDescriptor:d1 options:0 reflection:nil error:&err];
        require(psoMeshOnly != nil, [NSString stringWithFormat:@"mesh-only PSO failed: %@", err]);
        require(psoMeshOnly.maxTotalThreadsPerMeshThreadgroup >= 64u, @"mesh max threads < 64");

        {
            id<MTLTexture> rt = makeRenderTarget(dev, W, H, @"mesh-only");
            const float vTint[4] = { 0.5f, 0.25f, 0.0f, 1.0f }; // + pcolor(0, 0.5, 0.25, 0) = (128, 191, 64, 255)
            MTLRenderPassDescriptor *rpd = [MTLRenderPassDescriptor renderPassDescriptor];
            rpd.colorAttachments[0].texture = rt;
            rpd.colorAttachments[0].loadAction = MTLLoadActionClear;
            rpd.colorAttachments[0].storeAction = MTLStoreActionStore;
            rpd.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);

            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rpd];
            require(enc != nil, @"mesh-only encoder nil");
            [enc setRenderPipelineState:psoMeshOnly];
            [enc setMeshBytes:vTint length:sizeof(vTint) atIndex:0];
            [enc drawMeshThreadgroups:MTLSizeMake(1, 1, 1)
          threadsPerObjectThreadgroup:MTLSizeMake(1, 1, 1)
            threadsPerMeshThreadgroup:MTLSizeMake(1, 1, 1)];
            [enc endEncoding];
            blitReadback(cb, rt, readback, W, H, @"mesh-only");
            finishOK(cb, @"mesh-only");
            memcpy(pixels, readback.contents, bytes);
            verifyPixelRect(pixels, W, 0, 0, W, H, 128, 191, 64, 255, @"mesh-only-per-prim");
        }

        // -----------------------------------------------------------------------------------------
        // Test 2: Object + Mesh + full 16 KiB (16,384 B) payload + 2 mesh groups (left & right halves)
        // -----------------------------------------------------------------------------------------
        MTLMeshRenderPipelineDescriptor *d2 = [MTLMeshRenderPipelineDescriptor new];
        d2.objectFunction = fnObj16K;
        d2.meshFunction = fnMesh16K;
        d2.fragmentFunction = fnFrag;
        d2.payloadMemoryLength = 16384u;
        d2.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        id<MTLRenderPipelineState> pso16K =
            [dev newRenderPipelineStateWithMeshDescriptor:d2 options:0 reflection:nil error:&err];
        require(pso16K != nil, [NSString stringWithFormat:@"object+mesh 16KiB PSO failed: %@", err]);

        {
            id<MTLTexture> rt = makeRenderTarget(dev, W, H, @"obj-16k");
            const uint32_t cfg[4] = { 160u, 200u, 2u, 0u }; // word[0]=160, word[4095]=200, meshGridX=2
            MTLRenderPassDescriptor *rpd = [MTLRenderPassDescriptor renderPassDescriptor];
            rpd.colorAttachments[0].texture = rt;
            rpd.colorAttachments[0].loadAction = MTLLoadActionClear;
            rpd.colorAttachments[0].storeAction = MTLStoreActionStore;
            rpd.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);

            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rpd];
            [enc setRenderPipelineState:pso16K];
            [enc setObjectBytes:cfg length:sizeof(cfg) atIndex:0];
            [enc drawMeshThreadgroups:MTLSizeMake(1, 1, 1)
          threadsPerObjectThreadgroup:MTLSizeMake(32, 1, 1)
            threadsPerMeshThreadgroup:MTLSizeMake(1, 1, 1)];
            [enc endEncoding];
            blitReadback(cb, rt, readback, W, H, @"obj-16k");
            finishOK(cb, @"obj-16k");
            memcpy(pixels, readback.contents, bytes);
            // Left half: (160, 64, 200, 255); Right half: (160, 191, 200, 255).
            verifyPixelRect(pixels, W, 1, 1, W / 2u - 1u, H - 1u, 160, 64, 200, 255, @"obj-16k-left");
            verifyPixelRect(pixels, W, W / 2u + 1u, 1, W - 1u, H - 1u, 160, 191, 200, 255, @"obj-16k-right");
        }

        // -----------------------------------------------------------------------------------------
        // Test 3: drawMeshThreads: (ceil_div 33 threads / 32 per TG -> 2 object groups) + object TG memory
        // -----------------------------------------------------------------------------------------
        MTLMeshRenderPipelineDescriptor *d3 = [MTLMeshRenderPipelineDescriptor new];
        d3.objectFunction = fnObjTg;
        d3.meshFunction = fnMeshTg;
        d3.fragmentFunction = fnFrag;
        d3.payloadMemoryLength = 16u;
        d3.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        id<MTLRenderPipelineState> psoTg =
            [dev newRenderPipelineStateWithMeshDescriptor:d3 options:0 reflection:nil error:&err];
        require(psoTg != nil, [NSString stringWithFormat:@"drawMeshThreads PSO failed: %@", err]);

        {
            id<MTLTexture> rt = makeRenderTarget(dev, W, H, @"mesh-threads");
            MTLRenderPassDescriptor *rpd = [MTLRenderPassDescriptor renderPassDescriptor];
            rpd.colorAttachments[0].texture = rt;
            rpd.colorAttachments[0].loadAction = MTLLoadActionClear;
            rpd.colorAttachments[0].storeAction = MTLStoreActionStore;
            rpd.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);

            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rpd];
            [enc setRenderPipelineState:psoTg];
            [enc setObjectThreadgroupMemoryLength:256u atIndex:0];
            [enc drawMeshThreads:MTLSizeMake(33, 1, 1)
     threadsPerObjectThreadgroup:MTLSizeMake(32, 1, 1)
       threadsPerMeshThreadgroup:MTLSizeMake(1, 1, 1)];
            [enc endEncoding];
            blitReadback(cb, rt, readback, W, H, @"mesh-threads");
            finishOK(cb, @"mesh-threads");
            memcpy(pixels, readback.contents, bytes);
            // Group 0 -> left (64, 0, 255, 255); Group 1 -> right (192, 128, 255, 255).
            verifyPixelRect(pixels, W, 1, 1, W / 2u - 1u, H - 1u, 64, 0, 255, 255, @"mesh-threads-left");
            verifyPixelRect(pixels, W, W / 2u + 1u, 1, W - 1u, H - 1u, 192, 128, 255, 255, @"mesh-threads-right");
        }

        // -----------------------------------------------------------------------------------------
        // Test 4: GPU-driven indirect mesh draws (zero-grid no-op + non-zero offset + ARC lifetime)
        // -----------------------------------------------------------------------------------------
        id<MTLComputePipelineState> cpsoFill = [dev newComputePipelineStateWithFunction:fnFillInd error:&err];
        require(cpsoFill != nil, [NSString stringWithFormat:@"indirect fill CPSO failed: %@", err]);

        {
            id<MTLTexture> rt = makeRenderTarget(dev, W, H, @"mesh-indirect");
            id<MTLCommandBuffer> cb = [q commandBuffer];
            @autoreleasepool {
                id<MTLBuffer> indBuf = [dev newBufferWithLength:64u options:MTLResourceStorageModePrivate];
                require(indBuf != nil, @"indirect buffer nil");
                id<MTLComputeCommandEncoder> cenc = [cb computeCommandEncoder];
                [cenc setComputePipelineState:cpsoFill];
                [cenc setBuffer:indBuf offset:0 atIndex:0];
                [cenc dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
                [cenc endEncoding];

                // 4a: Indirect zero-grid at offset 0 must leave clear color (32, 64, 96, 255) untouched.
                MTLRenderPassDescriptor *rpd0 = [MTLRenderPassDescriptor renderPassDescriptor];
                rpd0.colorAttachments[0].texture = rt;
                rpd0.colorAttachments[0].loadAction = MTLLoadActionClear;
                rpd0.colorAttachments[0].storeAction = MTLStoreActionStore;
                rpd0.colorAttachments[0].clearColor = MTLClearColorMake(32.0 / 255.0, 64.0 / 255.0, 96.0 / 255.0, 1.0);
                id<MTLRenderCommandEncoder> enc0 = [cb renderCommandEncoderWithDescriptor:rpd0];
                [enc0 setRenderPipelineState:pso16K];
                const uint32_t cfg[4] = { 220u, 110u, 2u, 0u };
                [enc0 setObjectBytes:cfg length:sizeof(cfg) atIndex:0];
                [enc0 drawMeshThreadgroupsWithIndirectBuffer:indBuf indirectBufferOffset:0u
                                 threadsPerObjectThreadgroup:MTLSizeMake(1, 1, 1)
                                   threadsPerMeshThreadgroup:MTLSizeMake(1, 1, 1)];
                [enc0 endEncoding];
                blitReadback(cb, rt, readback, W, H, @"mesh-indirect-zero");
                indBuf = nil;
            }
            finishOK(cb, @"mesh-indirect-zero");
            memcpy(pixels, readback.contents, bytes);
            verifyPixelRect(pixels, W, 0, 0, W, H, 32, 64, 96, 255, @"mesh-indirect-zero-clear");

            // 4b: Indirect non-zero grid {1,1,1} at offset 16 with object+mesh+16KiB payload (and early ARC release).
            id<MTLCommandBuffer> cb2 = [q commandBuffer];
            @autoreleasepool {
                id<MTLBuffer> indBuf = [dev newBufferWithLength:64u options:MTLResourceStorageModePrivate];
                id<MTLComputeCommandEncoder> cenc = [cb2 computeCommandEncoder];
                [cenc setComputePipelineState:cpsoFill];
                [cenc setBuffer:indBuf offset:0 atIndex:0];
                [cenc dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
                [cenc endEncoding];

                MTLRenderPassDescriptor *rpd1 = [MTLRenderPassDescriptor renderPassDescriptor];
                rpd1.colorAttachments[0].texture = rt;
                rpd1.colorAttachments[0].loadAction = MTLLoadActionClear;
                rpd1.colorAttachments[0].storeAction = MTLStoreActionStore;
                rpd1.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);
                id<MTLRenderCommandEncoder> enc1 = [cb2 renderCommandEncoderWithDescriptor:rpd1];
                [enc1 setRenderPipelineState:pso16K];
                const uint32_t cfg[4] = { 220u, 110u, 2u, 0u };
                [enc1 setObjectBytes:cfg length:sizeof(cfg) atIndex:0];
                [enc1 drawMeshThreadgroupsWithIndirectBuffer:indBuf indirectBufferOffset:16u
                                 threadsPerObjectThreadgroup:MTLSizeMake(1, 1, 1)
                                   threadsPerMeshThreadgroup:MTLSizeMake(1, 1, 1)];
                [enc1 endEncoding];
                blitReadback(cb2, rt, readback, W, H, @"mesh-indirect-16k");
                indBuf = nil;
            }
            finishOK(cb2, @"mesh-indirect-16k");
            memcpy(pixels, readback.contents, bytes);
            verifyPixelRect(pixels, W, 1, 1, W / 2u - 1u, H - 1u, 220, 64, 110, 255, @"mesh-indirect-16k-left");
            verifyPixelRect(pixels, W, W / 2u + 1u, 1, W - 1u, H - 1u, 220, 191, 110, 255, @"mesh-indirect-16k-right");
        }

        // -----------------------------------------------------------------------------------------
        // Test 5: Footnote 10 positive — MTLLinkedFunctions.privateFunctions IS ALLOWED in mesh PSO
        // -----------------------------------------------------------------------------------------
        {
            MTLMeshRenderPipelineDescriptor *dp = [MTLMeshRenderPipelineDescriptor new];
            dp.meshFunction = fnMeshPriv;
            dp.fragmentFunction = fnFrag;
            MTLLinkedFunctions *plf = [MTLLinkedFunctions linkedFunctions];
            plf.privateFunctions = @[ fnHelper ];
            dp.meshLinkedFunctions = plf;
            dp.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
            id<MTLRenderPipelineState> psoPriv =
                [dev newRenderPipelineStateWithMeshDescriptor:dp options:0 reflection:nil error:&err];
            require(psoPriv != nil, [NSString stringWithFormat:@"Footnote 10 privateFunctions must succeed: %@", err]);

            id<MTLTexture> rt = makeRenderTarget(dev, W, H, @"mesh-privfn");
            MTLRenderPassDescriptor *rpd = [MTLRenderPassDescriptor renderPassDescriptor];
            rpd.colorAttachments[0].texture = rt;
            rpd.colorAttachments[0].loadAction = MTLLoadActionClear;
            rpd.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rpd];
            [enc setRenderPipelineState:psoPriv];
            [enc drawMeshThreadgroups:MTLSizeMake(1, 1, 1)
          threadsPerObjectThreadgroup:MTLSizeMake(1, 1, 1)
            threadsPerMeshThreadgroup:MTLSizeMake(1, 1, 1)];
            [enc endEncoding];
            blitReadback(cb, rt, readback, W, H, @"mesh-privfn");
            finishOK(cb, @"mesh-privfn");
            memcpy(pixels, readback.contents, bytes);
            verifyPixelRect(pixels, W, 0, 0, W, H, 128, 64, 255, 255, @"mesh-privfn");
        }

        // -----------------------------------------------------------------------------------------
        // Test 6: Negative checks (Footnote 10 fn-ptr refusal, >16 KiB payload, grid limit, indirect OOB)
        // -----------------------------------------------------------------------------------------
        if (!reference) {
            // 6a. Footnote 10: meshLinkedFunctions.functions (public render fn ptrs) rejected.
            MTLMeshRenderPipelineDescriptor *badFn10 = [MTLMeshRenderPipelineDescriptor new];
            badFn10.meshFunction = fnMeshOnly;
            badFn10.fragmentFunction = fnFrag;
            MTLLinkedFunctions *badLf = [MTLLinkedFunctions linkedFunctions];
            badLf.functions = @[ fnHelper ];
            badFn10.meshLinkedFunctions = badLf;
            badFn10.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
            NSError *ne = nil;
            id<MTLRenderPipelineState> badPso =
                [dev newRenderPipelineStateWithMeshDescriptor:badFn10 options:0 reflection:nil error:&ne];
            require(badPso == nil && ne != nil, @"Footnote 10: meshLinkedFunctions.functions must fail");

            // 6b. Footnote 10: fragmentLinkedFunctions.functions rejected on mesh PSO.
            MTLMeshRenderPipelineDescriptor *badFragFn10 = [MTLMeshRenderPipelineDescriptor new];
            badFragFn10.meshFunction = fnMeshOnly;
            badFragFn10.fragmentFunction = fnFrag;
            badFragFn10.fragmentLinkedFunctions = badLf;
            badFragFn10.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
            ne = nil;
            badPso = [dev newRenderPipelineStateWithMeshDescriptor:badFragFn10 options:0 reflection:nil error:&ne];
            require(badPso == nil && ne != nil, @"Footnote 10: fragmentLinkedFunctions.functions must fail");

            // 6c. Payload > 16,384 bytes rejected.
            MTLMeshRenderPipelineDescriptor *badPay = [MTLMeshRenderPipelineDescriptor new];
            badPay.objectFunction = fnObj16K;
            badPay.meshFunction = fnMesh16K;
            badPay.fragmentFunction = fnFrag;
            badPay.payloadMemoryLength = 16385u;
            badPay.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
            ne = nil;
            badPso = [dev newRenderPipelineStateWithMeshDescriptor:badPay options:0 reflection:nil error:&ne];
            require(badPso == nil && ne != nil, @"payloadMemoryLength > 16384 must fail");

            // 6d. Excessive maxTotalThreadgroupsPerMeshGrid rejected.
            MTLMeshRenderPipelineDescriptor *badGrid = [MTLMeshRenderPipelineDescriptor new];
            badGrid.meshFunction = fnMeshOnly;
            badGrid.fragmentFunction = fnFrag;
            badGrid.maxTotalThreadgroupsPerMeshGrid = (NSUInteger)1u << 30;
            badGrid.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
            ne = nil;
            badPso = [dev newRenderPipelineStateWithMeshDescriptor:badGrid options:0 reflection:nil error:&ne];
            require(badPso == nil && ne != nil, @"excessive maxTotalThreadgroupsPerMeshGrid must fail");

            // 6e. Nil meshFunction rejected.
            MTLMeshRenderPipelineDescriptor *nilMesh = [MTLMeshRenderPipelineDescriptor new];
            nilMesh.fragmentFunction = fnFrag;
            nilMesh.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
            ne = nil;
            badPso = [dev newRenderPipelineStateWithMeshDescriptor:nilMesh options:0 reflection:nil error:&ne];
            require(badPso == nil && ne != nil, @"nil meshFunction must fail");

            // 6f. Misaligned / OOB indirect mesh draw fails command buffer cleanly.
            id<MTLTexture> rt = makeRenderTarget(dev, W, H, @"mesh-neg");
            id<MTLBuffer> tinyInd = [dev newBufferWithLength:16u options:MTLResourceStorageModeShared];
            MTLRenderPassDescriptor *rpd = [MTLRenderPassDescriptor renderPassDescriptor];
            rpd.colorAttachments[0].texture = rt;
            rpd.colorAttachments[0].loadAction = MTLLoadActionClear;
            rpd.colorAttachments[0].storeAction = MTLStoreActionStore;

            id<MTLCommandBuffer> badCb1 = [q commandBuffer];
            id<MTLRenderCommandEncoder> badEnc1 = [badCb1 renderCommandEncoderWithDescriptor:rpd];
            [badEnc1 setRenderPipelineState:psoMeshOnly];
            [badEnc1 drawMeshThreadgroupsWithIndirectBuffer:tinyInd indirectBufferOffset:2u
                                threadsPerObjectThreadgroup:MTLSizeMake(1, 1, 1)
                                  threadsPerMeshThreadgroup:MTLSizeMake(1, 1, 1)];
            [badEnc1 endEncoding];
            finishExpectError(badCb1, @"indirect-misaligned");

            id<MTLCommandBuffer> badCb2 = [q commandBuffer];
            id<MTLRenderCommandEncoder> badEnc2 = [badCb2 renderCommandEncoderWithDescriptor:rpd];
            [badEnc2 setRenderPipelineState:psoMeshOnly];
            [badEnc2 drawMeshThreadgroupsWithIndirectBuffer:tinyInd indirectBufferOffset:8u
                                threadsPerObjectThreadgroup:MTLSizeMake(1, 1, 1)
                                  threadsPerMeshThreadgroup:MTLSizeMake(1, 1, 1)];
            [badEnc2 endEncoding];
            finishExpectError(badCb2, @"indirect-oob");
        }

        printf("METAL3_MESH_PASS mode=%s meshOnly=ok obj16k=ok drawThreads=ok indirect=ok privateFns=ok neg=%s\n",
               reference ? "reference" : "garnet",
               reference ? "skipped" : "ok");
        _exit(0);
    }
}
