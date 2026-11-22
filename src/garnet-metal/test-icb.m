#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#import <objc/runtime.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static BOOL referenceMode;

static void fail(NSString *message) __attribute__((noreturn));
static void fail(NSString *message) {
    fprintf(stderr, "ICB_FAIL %s\n", message.UTF8String);
    _exit(1);
}

static void passCase(NSString *name) {
    printf("ICB_CASE_PASS %s\n", name.UTF8String);
}

static void finish(id<MTLCommandBuffer> commands, NSString *operation) {
    if (!commands) fail([NSString stringWithFormat:@"%@ command buffer unavailable", operation]);
    [commands commit];
    [commands waitUntilCompleted];
    if (commands.status != MTLCommandBufferStatusCompleted || commands.error)
        fail([NSString stringWithFormat:@"%@ status=%lu error=%@",
              operation, (unsigned long)commands.status, commands.error]);
}

static void expectCommandError(id<MTLCommandBuffer> commands, NSString *operation) {
    if (!commands) fail([NSString stringWithFormat:@"%@ command buffer unavailable", operation]);
    [commands commit];
    [commands waitUntilCompleted];
    if (commands.status != MTLCommandBufferStatusError || !commands.error)
        fail([NSString stringWithFormat:@"%@ did not produce a controlled command-buffer error (status=%lu)",
              operation, (unsigned long)commands.status]);
}

static id<MTLTexture> newTarget(id<MTLDevice> device) {
    const NSUInteger width = 32, height = 32;
    MTLTextureDescriptor *descriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                           width:width height:height mipmapped:NO];
    descriptor.storageMode = MTLStorageModePrivate;
    descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];
    if (!texture) fail(@"render target unavailable");
    return texture;
}

static NSData *readTexture(id<MTLDevice> device, id<MTLCommandQueue> queue,
                           id<MTLTexture> texture, NSString *operation) {
    const NSUInteger width = texture.width, height = texture.height;
    NSUInteger length = width * height * 4;
    id<MTLBuffer> readback = [device newBufferWithLength:length options:MTLResourceStorageModeShared];
    if (!readback) fail([NSString stringWithFormat:@"%@ readback buffer unavailable", operation]);
    id<MTLCommandBuffer> commands = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
    if (!commands || !blit) fail([NSString stringWithFormat:@"%@ readback encoder unavailable", operation]);
    [blit copyFromTexture:texture sourceSlice:0 sourceLevel:0
             sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake(width, height, 1)
                 toBuffer:readback destinationOffset:0
        destinationBytesPerRow:width * 4
      destinationBytesPerImage:length];
    [blit endEncoding];
    finish(commands, [operation stringByAppendingString:@" readback"]);
    return [NSData dataWithBytes:readback.contents length:length];
}

static void expectColor(NSData *pixels, uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha,
                        NSString *operation) {
    const uint8_t *bytes = pixels.bytes;
    const uint8_t expected[4] = { red, green, blue, alpha };
    for (NSUInteger i = 0; i < pixels.length / 4; ++i) {
        for (NSUInteger channel = 0; channel < 4; ++channel) {
            int actual = bytes[i * 4 + channel];
            int want = expected[channel];
            if (actual < want - 1 || actual > want + 1)
                fail([NSString stringWithFormat:@"%@ pixel %lu channel %lu got %d expected %d",
                      operation, (unsigned long)i, (unsigned long)channel, actual, want]);
        }
    }
}

static void expectSamePixels(NSData *a, NSData *b, NSString *operation) {
    if (a.length != b.length) fail([NSString stringWithFormat:@"%@ length mismatch", operation]);
    const uint8_t *ap = a.bytes, *bp = b.bytes;
    for (NSUInteger i = 0; i < a.length; ++i) {
        int difference = (int)ap[i] - (int)bp[i];
        if (difference < -1 || difference > 1)
            fail([NSString stringWithFormat:@"%@ byte %lu differs: direct=%u indirect=%u",
                  operation, (unsigned long)i, (unsigned)ap[i], (unsigned)bp[i]]);
    }
}

static id<MTLRenderCommandEncoder> beginRender(id<MTLCommandQueue> queue, id<MTLTexture> target,
                                                id<MTLCommandBuffer> *outCommands,
                                                NSString *operation) {
    id<MTLCommandBuffer> commands = [queue commandBuffer];
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = target;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
    if (!commands || !encoder) fail([NSString stringWithFormat:@"%@ render encoder unavailable", operation]);
    *outCommands = commands;
    return encoder;
}

static id<MTLBuffer> newBuffer(id<MTLDevice> device, const void *bytes, NSUInteger length) {
    id<MTLBuffer> buffer = [device newBufferWithBytes:bytes length:length
                                              options:MTLResourceStorageModeShared];
    if (!buffer) fail(@"buffer unavailable");
    return buffer;
}

static id<MTLRenderPipelineState> newPipeline(id<MTLDevice> device, id<MTLLibrary> library,
                                               NSString *vertexName, NSString *fragmentName,
                                               BOOL vertexDescriptor, NSError **error) {
    MTLRenderPipelineDescriptor *descriptor = [MTLRenderPipelineDescriptor new];
    descriptor.vertexFunction = [library newFunctionWithName:vertexName];
    descriptor.fragmentFunction = [library newFunctionWithName:fragmentName];
    descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    descriptor.supportIndirectCommandBuffers = YES;
    if (vertexDescriptor) {
        MTLVertexDescriptor *layout = [MTLVertexDescriptor vertexDescriptor];
        layout.attributes[0].format = MTLVertexFormatFloat2;
        layout.attributes[0].offset = 0;
        layout.attributes[0].bufferIndex = 0;
        layout.layouts[0].stride = 2 * sizeof(float);
        layout.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;
        descriptor.vertexDescriptor = layout;
    }
    if (!descriptor.vertexFunction || !descriptor.fragmentFunction) {
        fail([NSString stringWithFormat:@"pipeline functions unavailable: %@ / %@",
              vertexName, fragmentName]);
    }
    id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithDescriptor:descriptor
                                                                                error:error];
    return pipeline;
}

static id<MTLIndirectCommandBuffer> newICB(id<MTLDevice> device,
                                            MTLIndirectCommandType commandTypes,
                                            BOOL inheritPipeline,
                                            BOOL inheritBuffers,
                                            NSUInteger commandCount) {
    MTLIndirectCommandBufferDescriptor *descriptor = [MTLIndirectCommandBufferDescriptor new];
    descriptor.commandTypes = commandTypes;
    descriptor.inheritPipelineState = inheritPipeline;
    descriptor.inheritBuffers = inheritBuffers;
    if (!inheritBuffers) {
        descriptor.maxVertexBufferBindCount = 2;
        descriptor.maxFragmentBufferBindCount = 1;
    }
    id<MTLIndirectCommandBuffer> icb =
        [device newIndirectCommandBufferWithDescriptor:descriptor
                                       maxCommandCount:commandCount
                                                options:MTLResourceStorageModePrivate];
    if (!icb) fail(@"indirect command buffer unavailable");
    return icb;
}

static void encodeICBDraw(id<MTLIndirectCommandBuffer> icb, NSUInteger index,
                          id<MTLRenderPipelineState> pipeline,
                          id<MTLBuffer> vertexBuffer, id<MTLBuffer> colorBuffer,
                          id<MTLBuffer> indexBuffer, BOOL indexed,
                          BOOL inheritPipeline, BOOL inheritBuffers) {
    id<MTLIndirectRenderCommand> command = [icb indirectRenderCommandAtIndex:index];
    if (!command) fail(@"indirect render command unavailable");
    if (!inheritPipeline) [command setRenderPipelineState:pipeline];
    if (!inheritBuffers) {
        [command setVertexBuffer:vertexBuffer offset:0 atIndex:0];
        [command setFragmentBuffer:colorBuffer offset:0 atIndex:0];
    }
    if (indexed) {
        [command drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                            indexCount:3 indexType:MTLIndexTypeUInt16
                           indexBuffer:indexBuffer indexBufferOffset:0
                         instanceCount:1 baseVertex:0 baseInstance:0];
    } else {
        [command drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3
                  instanceCount:1 baseInstance:0];
    }
}

static void encodeDirectDraw(id<MTLRenderCommandEncoder> encoder,
                             id<MTLRenderPipelineState> pipeline,
                             id<MTLBuffer> vertexBuffer, id<MTLBuffer> colorBuffer,
                             id<MTLBuffer> indexBuffer, BOOL indexed,
                             BOOL inheritPipeline, BOOL inheritBuffers) {
    if (!inheritPipeline) [encoder setRenderPipelineState:pipeline];
    if (!inheritBuffers) {
        [encoder setVertexBuffer:vertexBuffer offset:0 atIndex:0];
        [encoder setFragmentBuffer:colorBuffer offset:0 atIndex:0];
    }
    if (indexed) {
        [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                            indexCount:3 indexType:MTLIndexTypeUInt16
                           indexBuffer:indexBuffer indexBufferOffset:0
                         instanceCount:1 baseVertex:0 baseInstance:0];
    } else {
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3
                  instanceCount:1 baseInstance:0];
    }
}

static void testEquivalentCommands(id<MTLDevice> device, id<MTLCommandQueue> queue,
                                   id<MTLRenderPipelineState> pipeline,
                                   id<MTLBuffer> vertexBuffer, id<MTLBuffer> indexBuffer,
                                   id<MTLBuffer> redBuffer) {
    const MTLViewport viewport = { 0, 0, 32, 32, 0, 1 };
    const MTLScissorRect scissor = { 0, 0, 32, 32 };
    for (NSUInteger indexed = 0; indexed < 2; ++indexed) {
        for (NSUInteger inheritPipeline = 0; inheritPipeline < 2; ++inheritPipeline) {
            for (NSUInteger inheritBuffers = 0; inheritBuffers < 2; ++inheritBuffers) {
                NSString *name = [NSString stringWithFormat:@"equivalent-%@-pipeline%lu-buffers%lu",
                                  indexed ? @"indexed" : @"draw",
                                  (unsigned long)inheritPipeline,
                                  (unsigned long)inheritBuffers];
                id<MTLTexture> directTarget = newTarget(device);
                id<MTLTexture> indirectTarget = newTarget(device);
                id<MTLCommandBuffer> commands = nil;
                id<MTLRenderCommandEncoder> encoder = beginRender(queue, directTarget, &commands, name);
                [encoder setViewport:viewport];
                [encoder setScissorRect:scissor];
                if (inheritPipeline) [encoder setRenderPipelineState:pipeline];
                if (inheritBuffers) {
                    [encoder setVertexBuffer:vertexBuffer offset:0 atIndex:0];
                    [encoder setFragmentBuffer:redBuffer offset:0 atIndex:0];
                }
                encodeDirectDraw(encoder, pipeline, vertexBuffer, redBuffer, indexBuffer,
                                 indexed != 0, inheritPipeline != 0, inheritBuffers != 0);
                [encoder endEncoding];
                finish(commands, name);

                MTLIndirectCommandType type = indexed ? MTLIndirectCommandTypeDrawIndexed
                                                      : MTLIndirectCommandTypeDraw;
                id<MTLIndirectCommandBuffer> icb =
                    newICB(device, type, inheritPipeline != 0, inheritBuffers != 0, 1);
                encodeICBDraw(icb, 0, pipeline, vertexBuffer, redBuffer, indexBuffer,
                              indexed != 0, inheritPipeline != 0, inheritBuffers != 0);
                commands = nil;
                encoder = beginRender(queue, indirectTarget, &commands, name);
                [encoder setViewport:viewport];
                [encoder setScissorRect:scissor];
                if (inheritPipeline) [encoder setRenderPipelineState:pipeline];
                if (inheritBuffers) {
                    [encoder setVertexBuffer:vertexBuffer offset:0 atIndex:0];
                    [encoder setFragmentBuffer:redBuffer offset:0 atIndex:0];
                }
                [encoder executeCommandsInBuffer:icb withRange:NSMakeRange(0, 1)];
                [encoder endEncoding];
                finish(commands, name);

                NSData *directPixels = readTexture(device, queue, directTarget, name);
                NSData *indirectPixels = readTexture(device, queue, indirectTarget, name);
                expectSamePixels(directPixels, indirectPixels, name);
                expectColor(directPixels, 255, 0, 0, 255, name);
                passCase(name);
            }
        }
    }
}

static void testParentState(id<MTLDevice> device, id<MTLCommandQueue> queue,
                            id<MTLRenderPipelineState> statePipeline,
                            id<MTLRenderPipelineState> otherPipeline,
                            id<MTLBuffer> vertexBuffer, id<MTLBuffer> redBuffer) {
    id<MTLTexture> target = newTarget(device);
    id<MTLIndirectCommandBuffer> icb =
        newICB(device, MTLIndirectCommandTypeDraw, NO, NO, 1);
    encodeICBDraw(icb, 0, otherPipeline, vertexBuffer, redBuffer, nil, NO, NO, NO);

    id<MTLCommandBuffer> commands = nil;
    id<MTLRenderCommandEncoder> encoder = beginRender(queue, target, &commands, @"parent-state");
    [encoder setViewport:(MTLViewport){ 0, 0, 32, 32, 0, 1 }];
    [encoder setRenderPipelineState:statePipeline];
    [encoder setVertexBuffer:vertexBuffer offset:0 atIndex:0];
    [encoder setFragmentBuffer:redBuffer offset:0 atIndex:0];
    [encoder setScissorRect:(MTLScissorRect){ 0, 0, 11, 32 }];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder setScissorRect:(MTLScissorRect){ 11, 0, 10, 32 }];
    [encoder executeCommandsInBuffer:icb withRange:NSMakeRange(0, 1)];
    [encoder setScissorRect:(MTLScissorRect){ 21, 0, 11, 32 }];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
    finish(commands, @"parent-state");

    NSData *pixels = readTexture(device, queue, target, @"parent-state");
    const uint8_t *bytes = pixels.bytes;
    for (NSUInteger y = 0; y < 32; ++y) {
        for (NSUInteger x = 0; x < 32; ++x) {
            const uint8_t *pixel = bytes + (y * 32 + x) * 4;
            BOOL green = x >= 11 && x < 21;
            uint8_t expected[4] = { green ? 0 : 255, green ? 255 : 0, 0, 255 };
            for (NSUInteger channel = 0; channel < 4; ++channel) {
                int difference = (int)pixel[channel] - (int)expected[channel];
                if (difference < -1 || difference > 1)
                    fail([NSString stringWithFormat:
                          @"parent-state x=%lu y=%lu channel=%lu got=%u expected=%u",
                          (unsigned long)x, (unsigned long)y, (unsigned long)channel,
                          (unsigned)pixel[channel], (unsigned)expected[channel]]);
            }
        }
    }
    passCase(@"parent-state-after-explicit-pipeline-and-buffers");
}

static void testCopyAndReset(id<MTLDevice> device, id<MTLCommandQueue> queue,
                             id<MTLRenderPipelineState> pipeline,
                             id<MTLBuffer> vertexBuffer, id<MTLBuffer> redBuffer,
                             id<MTLBuffer> greenBuffer, id<MTLBuffer> blueBuffer) {
    id<MTLIndirectCommandBuffer> source =
        newICB(device, MTLIndirectCommandTypeDraw, NO, NO, 2);
    encodeICBDraw(source, 0, pipeline, vertexBuffer, redBuffer, nil, NO, NO, NO);
    encodeICBDraw(source, 1, pipeline, vertexBuffer, greenBuffer, nil, NO, NO, NO);
    id<MTLIndirectCommandBuffer> destination =
        newICB(device, MTLIndirectCommandTypeDraw, NO, NO, 4);

    id<MTLTexture> target = newTarget(device);
    id<MTLCommandBuffer> commands = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
    [blit copyIndirectCommandBuffer:source sourceRange:NSMakeRange(0, 2)
                       destination:destination destinationIndex:1];
    [blit endEncoding];
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = target;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
    [encoder executeCommandsInBuffer:destination withRange:NSMakeRange(1, 2)];
    [encoder endEncoding];
    finish(commands, @"copy-order");
    expectColor(readTexture(device, queue, target, @"copy-order"),
                0, 255, 0, 255, @"copy-order");
    passCase(@"copy-preserves-slot-order");

    id<MTLIndirectCommandBuffer> protectedDestination =
        newICB(device, MTLIndirectCommandTypeDraw, NO, NO, 1);
    encodeICBDraw(protectedDestination, 0, pipeline, vertexBuffer, blueBuffer, nil, NO, NO, NO);
    id<MTLIndirectCommandBuffer> incompatible =
        newICB(device, MTLIndirectCommandTypeDraw | MTLIndirectCommandTypeDrawIndexed,
               NO, NO, 1);
    commands = [queue commandBuffer];
    blit = [commands blitCommandEncoder];
    [blit copyIndirectCommandBuffer:source sourceRange:NSMakeRange(0, 1)
                       destination:incompatible destinationIndex:0];
    [blit endEncoding];
    expectCommandError(commands, @"incompatible-copy-rejection");

    target = newTarget(device);
    commands = nil;
    encoder = beginRender(queue, target, &commands, @"copy-atomic");
    [encoder executeCommandsInBuffer:protectedDestination withRange:NSMakeRange(0, 1)];
    [encoder endEncoding];
    finish(commands, @"copy-atomic");
    expectColor(readTexture(device, queue, target, @"copy-atomic"),
                0, 0, 255, 255, @"copy-atomic");
    passCase(@"copy-rejection-is-atomic");

    target = newTarget(device);
    commands = [queue commandBuffer];
    blit = [commands blitCommandEncoder];
    [blit optimizeIndirectCommandBuffer:source withRange:NSMakeRange(0, 2)];
    [blit resetCommandsInBuffer:source withRange:NSMakeRange(0, 1)];
    [blit endEncoding];
    pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = target;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    encoder = [commands renderCommandEncoderWithDescriptor:pass];
    [encoder executeCommandsInBuffer:source withRange:NSMakeRange(0, 2)];
    [encoder endEncoding];
    finish(commands, @"reset-empty");
    expectColor(readTexture(device, queue, target, @"reset-empty"),
                0, 255, 0, 255, @"reset-empty");
    passCase(@"reset-and-empty-slot");
}

static void testDeferredCommandBufferOrdering(id<MTLDevice> device,
                                               id<MTLCommandQueue> queue,
                                               id<MTLRenderPipelineState> pipeline,
                                               id<MTLBuffer> vertexBuffer,
                                               id<MTLBuffer> redBuffer,
                                               id<MTLBuffer> greenBuffer,
                                               id<MTLBuffer> blueBuffer) {
    id<MTLIndirectCommandBuffer> resetSource =
        newICB(device, MTLIndirectCommandTypeDraw, NO, NO, 1);
    encodeICBDraw(resetSource, 0, pipeline, vertexBuffer, redBuffer, nil, NO, NO, NO);

    id<MTLCommandBuffer> resetCommands = [queue commandBuffer];
    id<MTLBlitCommandEncoder> resetBlit = [resetCommands blitCommandEncoder];
    [resetBlit resetCommandsInBuffer:resetSource withRange:NSMakeRange(0, 1)];
    [resetBlit endEncoding];

    id<MTLTexture> target = newTarget(device);
    id<MTLCommandBuffer> commands = nil;
    id<MTLRenderCommandEncoder> encoder = beginRender(queue, target, &commands, @"reset-before-commit");
    [encoder executeCommandsInBuffer:resetSource withRange:NSMakeRange(0, 1)];
    [encoder endEncoding];
    finish(commands, @"reset-before-commit");
    expectColor(readTexture(device, queue, target, @"reset-before-commit"),
                255, 0, 0, 255, @"reset-before-commit");

    finish(resetCommands, @"reset-commit");
    target = newTarget(device);
    commands = nil;
    encoder = beginRender(queue, target, &commands, @"reset-after-commit");
    [encoder executeCommandsInBuffer:resetSource withRange:NSMakeRange(0, 1)];
    [encoder endEncoding];
    finish(commands, @"reset-after-commit");
    expectColor(readTexture(device, queue, target, @"reset-after-commit"),
                0, 0, 0, 255, @"reset-after-commit");
    passCase(@"reset-visible-only-after-commit");

    id<MTLIndirectCommandBuffer> copySource =
        newICB(device, MTLIndirectCommandTypeDraw, NO, NO, 2);
    encodeICBDraw(copySource, 0, pipeline, vertexBuffer, redBuffer, nil, NO, NO, NO);
    encodeICBDraw(copySource, 1, pipeline, vertexBuffer, greenBuffer, nil, NO, NO, NO);
    id<MTLIndirectCommandBuffer> copyDestination =
        newICB(device, MTLIndirectCommandTypeDraw, NO, NO, 2);
    encodeICBDraw(copyDestination, 0, pipeline, vertexBuffer, blueBuffer, nil, NO, NO, NO);
    encodeICBDraw(copyDestination, 1, pipeline, vertexBuffer, blueBuffer, nil, NO, NO, NO);

    id<MTLCommandBuffer> copyCommands = [queue commandBuffer];
    id<MTLBlitCommandEncoder> copyBlit = [copyCommands blitCommandEncoder];
    [copyBlit copyIndirectCommandBuffer:copySource sourceRange:NSMakeRange(0, 2)
                            destination:copyDestination destinationIndex:0];
    [copyBlit endEncoding];

    target = newTarget(device);
    commands = nil;
    encoder = beginRender(queue, target, &commands, @"copy-before-commit");
    [encoder executeCommandsInBuffer:copyDestination withRange:NSMakeRange(1, 1)];
    [encoder endEncoding];
    finish(commands, @"copy-before-commit");
    expectColor(readTexture(device, queue, target, @"copy-before-commit"),
                0, 0, 255, 255, @"copy-before-commit");

    finish(copyCommands, @"copy-commit");
    target = newTarget(device);
    commands = nil;
    encoder = beginRender(queue, target, &commands, @"copy-after-commit");
    [encoder executeCommandsInBuffer:copyDestination withRange:NSMakeRange(1, 1)];
    [encoder endEncoding];
    finish(commands, @"copy-after-commit");
    expectColor(readTexture(device, queue, target, @"copy-after-commit"),
                0, 255, 0, 255, @"copy-after-commit");
    passCase(@"copy-visible-only-after-commit");
}

static void testSparseLargeCommandCount(id<MTLDevice> device, id<MTLCommandQueue> queue,
                                        id<MTLRenderPipelineState> pipeline,
                                        id<MTLBuffer> vertexBuffer,
                                        id<MTLBuffer> redBuffer) {
    MTLIndirectCommandBufferDescriptor *oversizedDescriptor =
        [MTLIndirectCommandBufferDescriptor new];
    oversizedDescriptor.commandTypes = MTLIndirectCommandTypeDraw;
    id oversized =
        [device newIndirectCommandBufferWithDescriptor:oversizedDescriptor
                                       maxCommandCount:(NSUInteger)UINT32_MAX + 1
                                                options:MTLResourceStorageModePrivate];
    if (oversized) fail(@"maxCommandCount above the 32-bit execution range was accepted");
    passCase(@"oversized-command-count-refused");

    const NSUInteger commandCount = (NSUInteger)UINT32_MAX;
    id<MTLIndirectCommandBuffer> large =
        newICB(device, MTLIndirectCommandTypeDraw, NO, NO, commandCount);
    encodeICBDraw(large, commandCount - 1, pipeline, vertexBuffer, redBuffer, nil, NO, NO, NO);

    id<MTLTexture> target = newTarget(device);
    id<MTLCommandBuffer> commands = nil;
    id<MTLRenderCommandEncoder> encoder = beginRender(queue, target, &commands, @"sparse-large-icb");
    [encoder executeCommandsInBuffer:large withRange:NSMakeRange(commandCount - 1, 1)];
    [encoder endEncoding];
    finish(commands, @"sparse-large-icb");
    expectColor(readTexture(device, queue, target, @"sparse-large-icb"),
                255, 0, 0, 255, @"sparse-large-icb");

    id<MTLCommandBuffer> resetCommands = [queue commandBuffer];
    id<MTLBlitCommandEncoder> resetBlit = [resetCommands blitCommandEncoder];
    [resetBlit resetCommandsInBuffer:large withRange:NSMakeRange(0, commandCount)];
    [resetBlit endEncoding];
    finish(resetCommands, @"sparse-large-reset");
    target = newTarget(device);
    commands = nil;
    encoder = beginRender(queue, target, &commands, @"sparse-large-reset");
    [encoder executeCommandsInBuffer:large withRange:NSMakeRange(commandCount - 1, 1)];
    [encoder endEncoding];
    finish(commands, @"sparse-large-reset");
    expectColor(readTexture(device, queue, target, @"sparse-large-reset"),
                0, 0, 0, 255, @"sparse-large-reset");
    passCase(@"sparse-large-command-count-and-reset");
}

static void testInvalidIndex(id<MTLDevice> device, id<MTLCommandQueue> queue,
                             id<MTLRenderPipelineState> pipeline,
                             id<MTLBuffer> vertexBuffer, id<MTLBuffer> indexBuffer,
                             id<MTLBuffer> redBuffer) {
    id<MTLIndirectCommandBuffer> icb =
        newICB(device, MTLIndirectCommandTypeDraw, NO, NO, 1);
    encodeICBDraw(icb, 0, pipeline, vertexBuffer, redBuffer, nil, NO, NO, NO);
    id<MTLIndirectRenderCommand> invalid = [icb indirectRenderCommandAtIndex:1];
    if (invalid) {
        [invalid setRenderPipelineState:pipeline];
        [invalid setVertexBuffer:vertexBuffer offset:0 atIndex:0];
        [invalid setFragmentBuffer:redBuffer offset:0 atIndex:0];
        [invalid drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:3
                             indexType:MTLIndexTypeUInt16 indexBuffer:indexBuffer
                    indexBufferOffset:0 instanceCount:1 baseVertex:0 baseInstance:0];
    }
    id<MTLTexture> target = newTarget(device);
    id<MTLCommandBuffer> commands = nil;
    id<MTLRenderCommandEncoder> encoder = beginRender(queue, target, &commands, @"invalid-index");
    [encoder executeCommandsInBuffer:icb withRange:NSMakeRange(0, 1)];
    [encoder endEncoding];
    finish(commands, @"invalid-index");
    expectColor(readTexture(device, queue, target, @"invalid-index"),
                255, 0, 0, 255, @"invalid-index");
    passCase(@"invalid-command-index-does-not-mutate-slot");
}

typedef NSUInteger (*ExecuteICBFn)(id, SEL, id, NSRange, NSString **);

static NSUInteger executeICBDirectly(id<MTLIndirectCommandBuffer> icb,
                                     id<MTLRenderCommandEncoder> encoder,
                                     NSRange range, NSString **reason) {
    return ((ExecuteICBFn)objc_msgSend)(icb,
        sel_registerName("rdna4ExecuteOnEncoder:range:reason:"), encoder, range, reason);
}

static void testWholeRangeValidation(id<MTLDevice> device, id<MTLCommandQueue> queue,
                                     id<MTLRenderPipelineState> pipeline,
                                     id<MTLBuffer> vertexBuffer, id<MTLBuffer> indexBuffer,
                                     id<MTLBuffer> redBuffer) {
    id<MTLIndirectCommandBuffer> icb =
        newICB(device, MTLIndirectCommandTypeDraw, NO, NO, 2);
    encodeICBDraw(icb, 0, pipeline, vertexBuffer, redBuffer, nil, NO, NO, NO);
    id<MTLIndirectRenderCommand> invalid = [icb indirectRenderCommandAtIndex:1];
    [invalid setRenderPipelineState:pipeline];
    [invalid setVertexBuffer:vertexBuffer offset:0 atIndex:0];
    [invalid setFragmentBuffer:redBuffer offset:0 atIndex:0];
    [invalid drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:3
                         indexType:MTLIndexTypeUInt16 indexBuffer:indexBuffer
                indexBufferOffset:0 instanceCount:1 baseVertex:0 baseInstance:0];

    id<MTLTexture> target = newTarget(device);
    id<MTLCommandBuffer> commands = nil;
    id<MTLRenderCommandEncoder> encoder = beginRender(queue, target, &commands, @"range-atomic");
    NSString *reason = nil;
    NSUInteger replayed = executeICBDirectly(icb, encoder, NSMakeRange(0, 2), &reason);
    if (replayed || !reason)
        fail([NSString stringWithFormat:@"range-atomic replayed=%lu reason=%@", (unsigned long)replayed, reason]);
    [encoder endEncoding];
    finish(commands, @"range-atomic");
    expectColor(readTexture(device, queue, target, @"range-atomic"),
                0, 0, 0, 255, @"range-atomic");
    passCase(@"whole-range-validation-precedes-replay");
}

static void testPatchRejection(id<MTLDevice> device) {
    MTLIndirectCommandBufferDescriptor *descriptor =
        [MTLIndirectCommandBufferDescriptor new];
    descriptor.commandTypes = MTLIndirectCommandTypeDraw | MTLIndirectCommandTypeDrawPatches;
    descriptor.inheritPipelineState = NO;
    descriptor.inheritBuffers = NO;
    descriptor.maxVertexBufferBindCount = 2;
    descriptor.maxFragmentBufferBindCount = 1;
    id<MTLIndirectCommandBuffer> icb =
        [device newIndirectCommandBufferWithDescriptor:descriptor
                                       maxCommandCount:2
                                                options:MTLResourceStorageModePrivate];
    if (icb)
        fail(@"patch command type was accepted at indirect command buffer creation");
    passCase(@"patch-command-type-rejected-at-creation");
}

static void testRangeRejection(id<MTLDevice> device, id<MTLCommandQueue> queue) {
    id<MTLIndirectCommandBuffer> one =
        newICB(device, MTLIndirectCommandTypeDraw, NO, NO, 1);
    id<MTLCommandBuffer> commands = [queue commandBuffer];
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = newTarget(device);
    id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
    [encoder executeCommandsInBuffer:one withRange:NSMakeRange(1, 1)];
    [encoder endEncoding];
    expectCommandError(commands, @"range-location-boundary");

    commands = [queue commandBuffer];
    pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = newTarget(device);
    encoder = [commands renderCommandEncoderWithDescriptor:pass];
    [encoder executeCommandsInBuffer:one withRange:NSMakeRange(1, NSUIntegerMax)];
    [encoder endEncoding];
    expectCommandError(commands, @"range-overflow");

    id<MTLIndirectCommandBuffer> oversized =
        newICB(device, MTLIndirectCommandTypeDraw, NO, NO, 0x4001);
    commands = [queue commandBuffer];
    pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = newTarget(device);
    encoder = [commands renderCommandEncoderWithDescriptor:pass];
    [encoder executeCommandsInBuffer:oversized withRange:NSMakeRange(0, 0x4001)];
    [encoder endEncoding];
    expectCommandError(commands, @"range-mac2-limit");
    passCase(@"range-boundary-and-overflow-rejection");
}

int main(int argc, const char *argv[]) {
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        puts("Usage: sudo test-icb BUNDLE|--reference\n"
             "Compares direct and CPU-recorded render ICB commands through a production Metal bundle.");
        return 0;
    }
    if (argc != 2) return 2;
    referenceMode = !strcmp(argv[1], "--reference");
    if (!referenceMode && geteuid() != 0) return 2;
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(180);
    @autoreleasepool {
        @try {
            id<MTLDevice> device = nil;
            if (referenceMode) {
                device = MTLCreateSystemDefaultDevice();
            } else {
                setenv("RDNA4M_ALLOW", "1", 1);
                setenv("RDNA4M_TEST_INPROC", "1", 1);
                setenv("MVK_CONFIG_LOG_LEVEL", "2", 1);
                setenv("MVK_CONFIG_USE_MTLHEAP", "0", 1);
                NSError *loadError = nil;
                if (![[NSBundle bundleWithPath:@(argv[1])] loadAndReturnError:&loadError])
                    fail(loadError.description);
                device = [[objc_getClass("GarnetDevice") alloc] init];
                ((void (*)(id, SEL))objc_msgSend)(device, sel_registerName("initLimits"));
            }
            if (!device) fail(@"device unavailable");
            id<MTLCommandQueue> queue = [device newCommandQueue];
            if (!queue) fail(@"command queue unavailable");

            NSString *source =
                @"#include <metal_stdlib>\n"
                 "using namespace metal;\n"
                 "struct ICBVIn { float2 p [[attribute(0)]]; };\n"
                 "struct ICBVOut { float4 position [[position]]; };\n"
                 "vertex ICBVOut icb_stage_vs(ICBVIn in [[stage_in]]) {\n"
                 "  ICBVOut out; out.position = float4(in.p, 0.0, 1.0); return out;\n"
                 "}\n"
                 "vertex ICBVOut icb_id_vs(uint vid [[vertex_id]]) {\n"
                 "  float2 p[3] = { float2(-1.0,-1.0), float2(3.0,-1.0), float2(-1.0,3.0) };\n"
                 "  ICBVOut out; out.position = float4(p[vid], 0.0, 1.0); return out;\n"
                 "}\n"
                 "fragment float4 icb_const_fs(constant float4 &color [[buffer(0)]]) { return color; }\n"
                 "fragment float4 icb_green_fs() { return float4(0.0, 1.0, 0.0, 1.0); }\n";
            NSError *error = nil;
            id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
            if (!library) fail([NSString stringWithFormat:@"source library: %@", error]);
            id<MTLRenderPipelineState> pipeline =
                newPipeline(device, library, @"icb_stage_vs", @"icb_const_fs", YES, &error);
            if (!pipeline) fail([NSString stringWithFormat:@"stage pipeline: %@", error]);
            id<MTLRenderPipelineState> otherPipeline =
                newPipeline(device, library, @"icb_id_vs", @"icb_green_fs", NO, &error);
            if (!otherPipeline) fail([NSString stringWithFormat:@"id pipeline: %@", error]);
            SEL support = sel_registerName("supportIndirectCommandBuffers");
            if (![pipeline respondsToSelector:support] || ![otherPipeline respondsToSelector:support] ||
                !((BOOL (*)(id, SEL))objc_msgSend)(pipeline, support) ||
                !((BOOL (*)(id, SEL))objc_msgSend)(otherPipeline, support))
                fail(@"render pipeline does not advertise indirect command buffer support");

            const float fullTriangle[6] = { -1, -1, 3, -1, -1, 3 };
            const float red[4] = { 1, 0, 0, 1 };
            const float green[4] = { 0, 1, 0, 1 };
            const float blue[4] = { 0, 0, 1, 1 };
            const uint16_t indices[3] = { 0, 1, 2 };
            id<MTLBuffer> vertexBuffer = newBuffer(device, fullTriangle, sizeof fullTriangle);
            id<MTLBuffer> indexBuffer = newBuffer(device, indices, sizeof indices);
            id<MTLBuffer> redBuffer = newBuffer(device, red, sizeof red);
            id<MTLBuffer> greenBuffer = newBuffer(device, green, sizeof green);
            id<MTLBuffer> blueBuffer = newBuffer(device, blue, sizeof blue);

            testEquivalentCommands(device, queue, pipeline, vertexBuffer, indexBuffer, redBuffer);
            testParentState(device, queue, pipeline, otherPipeline, vertexBuffer, redBuffer);
            testCopyAndReset(device, queue, pipeline, vertexBuffer, redBuffer, greenBuffer, blueBuffer);
            testDeferredCommandBufferOrdering(device, queue, pipeline, vertexBuffer,
                                               redBuffer, greenBuffer, blueBuffer);
            if (!referenceMode) {
                testSparseLargeCommandCount(device, queue, pipeline, vertexBuffer, redBuffer);
                testInvalidIndex(device, queue, pipeline, vertexBuffer, indexBuffer, redBuffer);
                testWholeRangeValidation(device, queue, pipeline, vertexBuffer, indexBuffer, redBuffer);
                testPatchRejection(device);
                testRangeRejection(device, queue);
            }
            printf("ICB_SUITE_PASS reference=%u production_bundle=%u\n",
                   referenceMode ? 1u : 0u, referenceMode ? 0u : 1u);
            return 0;
        } @catch (NSException *exception) {
            fail(exception.description);
        }
    }
}
