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

static uint16_t f32_to_f16(float f) {
    __fp16 h = (__fp16)f;
    uint16_t bits = 0;
    memcpy(&bits, &h, 2);
    return bits;
}

static id<MTLRenderPipelineState> newTessPipeline(id<MTLDevice> device, id<MTLLibrary> library,
                                                  NSString *vertName, NSString *fragName,
                                                  BOOL hasAttr, MTLTessellationControlPointIndexType cpIdxType,
                                                  MTLTessellationFactorStepFunction fStep) {
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [library newFunctionWithName:vertName];
    d.fragmentFunction = [library newFunctionWithName:fragName];
    d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    d.tessellationPartitionMode = MTLTessellationPartitionModeInteger;
    d.tessellationOutputWindingOrder = MTLWindingClockwise;
    d.tessellationFactorFormat = MTLTessellationFactorFormatHalf;
    d.tessellationFactorStepFunction = fStep;
    d.tessellationControlPointIndexType = cpIdxType;
    d.tessellationFactorScaleEnabled = YES;
    d.maxTessellationFactor = 16;
    d.supportIndirectCommandBuffers = YES;
    if (hasAttr) {
        MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
        vd.attributes[0].format = MTLVertexFormatFloat2;
        vd.attributes[0].offset = 0;
        vd.attributes[0].bufferIndex = 0;
        vd.layouts[0].stride = 2 * sizeof(float);
        vd.layouts[0].stepFunction = MTLVertexStepFunctionPerPatchControlPoint;
        vd.layouts[0].stepRate = 1;
        d.vertexDescriptor = vd;
    }
    NSError *err = nil;
    id<MTLRenderPipelineState> pso = [device newRenderPipelineStateWithDescriptor:d error:&err];
    if (!pso) fail([NSString stringWithFormat:@"tess pipeline %@/%@: %@", vertName, fragName, err]);
    return pso;
}

static void testCPUPatchEquivalence(id<MTLDevice> device, id<MTLCommandQueue> queue) {
    NSString *src =
        @"#include <metal_stdlib>\n"
         "using namespace metal;\n"
         "struct POut { float4 pos [[position]]; float4 col; };\n"
         "struct CPIn { float2 xy [[attribute(0)]]; };\n"
         "struct TriIn { patch_control_point<CPIn> cp; };\n"
         "[[patch(triangle, 3)]]\n"
         "vertex POut icb_tri_tes(TriIn in [[stage_in]], float3 b [[position_in_patch]], uint pid [[patch_id]], uint iid [[instance_id]]) {\n"
         "  POut o;\n"
         "  float2 p = b.x * in.cp[0].xy + b.y * in.cp[1].xy + b.z * in.cp[2].xy;\n"
         "  o.pos = float4(p, 0.0, 1.0);\n"
         "  o.col = float4(b.x, b.y * 0.8 + float(pid) * 0.2, 0.25 + 0.5 * float(iid), 1.0);\n"
         "  return o;\n"
         "}\n"
         "struct QuadIn { patch_control_point<CPIn> cp; };\n"
         "[[patch(quad, 4)]]\n"
         "vertex POut icb_quad_tes(QuadIn in [[stage_in]], float2 uv [[position_in_patch]], uint pid [[patch_id]], uint iid [[instance_id]]) {\n"
         "  POut o;\n"
         "  float2 bot = mix(in.cp[0].xy, in.cp[1].xy, uv.x);\n"
         "  float2 top = mix(in.cp[2].xy, in.cp[3].xy, uv.x);\n"
         "  o.pos = float4(mix(bot, top, uv.y), 0.0, 1.0);\n"
         "  o.col = float4(uv.x, uv.y, 0.2 + 0.4 * float(pid + iid), 1.0);\n"
         "  return o;\n"
         "}\n"
         "[[patch(triangle, 0)]]\n"
         "vertex POut icb_zcp_tes(float3 b [[position_in_patch]]) {\n"
         "  float2 p[3] = { float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0) };\n"
         "  POut o;\n"
         "  o.pos = float4(b.x * p[0] + b.y * p[1] + b.z * p[2], 0.0, 1.0);\n"
         "  o.col = float4(0.8, 0.3, 0.6, 1.0);\n"
         "  return o;\n"
         "}\n"
         "fragment float4 icb_patch_fs(POut in [[stage_in]], constant float4 &tint [[buffer(0)]]) {\n"
         "  return in.col * tint;\n"
         "}\n";
    NSError *err = nil;
    id<MTLLibrary> lib = [device newLibraryWithSource:src options:nil error:&err];
    if (!lib) fail([NSString stringWithFormat:@"patch lib: %@", err]);

    id<MTLRenderPipelineState> triPso = newTessPipeline(device, lib, @"icb_tri_tes", @"icb_patch_fs", YES, MTLTessellationControlPointIndexTypeNone, MTLTessellationFactorStepFunctionPerPatch);
    id<MTLRenderPipelineState> quadIdx16Pso = newTessPipeline(device, lib, @"icb_quad_tes", @"icb_patch_fs", YES, MTLTessellationControlPointIndexTypeUInt16, MTLTessellationFactorStepFunctionPerPatchAndPerInstance);
    id<MTLRenderPipelineState> quadIdx32Pso = newTessPipeline(device, lib, @"icb_quad_tes", @"icb_patch_fs", YES, MTLTessellationControlPointIndexTypeUInt32, MTLTessellationFactorStepFunctionPerPatchAndPerInstance);
    id<MTLRenderPipelineState> zcpPso = newTessPipeline(device, lib, @"icb_zcp_tes", @"icb_patch_fs", NO, MTLTessellationControlPointIndexTypeNone, MTLTessellationFactorStepFunctionConstant);

    const float triVerts[6] = { -1, -1, 3, -1, -1, 3 };
    const float quadVerts[8] = { -1, -1, 1, -1, -1, 1, 1, 1 };
    const float white[4] = { 1, 1, 1, 1 };
    const uint16_t triFactors8B[4] = { f32_to_f16(4), f32_to_f16(4), f32_to_f16(4), f32_to_f16(4) }; // exact 8 bytes (<16B!)
    const uint16_t quadFactors2x16B[16] = {
        // instance 0 stride slot (16B): zero factors
        0, 0, 0, 0, 0, 0, 0, 0,
        // instance 1 stride slot (at byte 16): valid 12B quad factors
        f32_to_f16(3), f32_to_f16(3), f32_to_f16(3), f32_to_f16(3), f32_to_f16(3), f32_to_f16(3), 0, 0,
    };
    const uint32_t patchIdx[2] = { 99u, 0u }; // 4B uint32 patch indices; offset 4 selects patch 0
    const uint16_t cp16[4] = { 0, 1, 2, 3 };
    const uint32_t cp32[4] = { 0, 1, 2, 3 };

    id<MTLBuffer> triVB = newBuffer(device, triVerts, sizeof triVerts);
    id<MTLBuffer> quadVB = newBuffer(device, quadVerts, sizeof quadVerts);
    id<MTLBuffer> tintBuf = newBuffer(device, white, sizeof white);
    id<MTLBuffer> triFB8 = newBuffer(device, triFactors8B, sizeof triFactors8B); // strictly 8 bytes
    id<MTLBuffer> quadFB = newBuffer(device, quadFactors2x16B, sizeof quadFactors2x16B);
    id<MTLBuffer> pIdxBuf = newBuffer(device, patchIdx, sizeof patchIdx);
    id<MTLBuffer> cp16Buf = newBuffer(device, cp16, sizeof cp16);
    id<MTLBuffer> cp32Buf = newBuffer(device, cp32, sizeof cp32);

    const MTLViewport vp = { 0, 0, 32, 32, 0, 1 };
    const MTLScissorRect sc = { 0, 0, 32, 32 };

    // 1) drawPatches tri with 8-byte factor buffer + inherit combinations + factorScale inheritance
    for (NSUInteger ip = 0; ip < 2; ip++) {
        for (NSUInteger ib = 0; ib < 2; ib++) {
            id<MTLTexture> dt = newTarget(device), it = newTarget(device);
            id<MTLCommandBuffer> cb = nil;
            id<MTLRenderCommandEncoder> enc = beginRender(queue, dt, &cb, @"cpu-patch-tri-direct");
            [enc setViewport:vp]; [enc setScissorRect:sc];
            [enc setRenderPipelineState:triPso];
            [enc setVertexBuffer:triVB offset:0 atIndex:0];
            [enc setFragmentBuffer:tintBuf offset:0 atIndex:0];
            [enc setTessellationFactorScale:1.0f];
            [enc setTessellationFactorBuffer:triFB8 offset:0 instanceStride:0];
            [enc drawPatches:3 patchStart:0 patchCount:1 patchIndexBuffer:nil patchIndexBufferOffset:0 instanceCount:1 baseInstance:0];
            [enc endEncoding];
            finish(cb, @"cpu-patch-tri-direct");

            id<MTLIndirectCommandBuffer> icb = newICB(device, MTLIndirectCommandTypeDrawPatches, ip != 0, ib != 0, 1);
            id<MTLIndirectRenderCommand> cmd = [icb indirectRenderCommandAtIndex:0];
            if (!ip) [cmd setRenderPipelineState:triPso];
            if (!ib) { [cmd setVertexBuffer:triVB offset:0 atIndex:0]; [cmd setFragmentBuffer:tintBuf offset:0 atIndex:0]; }
            [cmd drawPatches:3 patchStart:0 patchCount:1 patchIndexBuffer:nil patchIndexBufferOffset:0 instanceCount:1 baseInstance:0
                tessellationFactorBuffer:triFB8 tessellationFactorBufferOffset:0 tessellationFactorBufferInstanceStride:0];

            cb = nil;
            enc = beginRender(queue, it, &cb, @"cpu-patch-tri-icb");
            [enc setViewport:vp]; [enc setScissorRect:sc];
            if (ip) [enc setRenderPipelineState:triPso];
            if (ib) { [enc setVertexBuffer:triVB offset:0 atIndex:0]; [enc setFragmentBuffer:tintBuf offset:0 atIndex:0]; }
            [enc setTessellationFactorScale:1.0f];
            [enc executeCommandsInBuffer:icb withRange:NSMakeRange(0, 1)];
            [enc endEncoding];
            finish(cb, @"cpu-patch-tri-icb");
            expectSamePixels(readTexture(device, queue, dt, @"tri-d"),
                             readTexture(device, queue, it, @"tri-i"), @"cpu-patch-tri");
        }
    }

    // Inherited tessellationFactorScale = 0.0f must cull the ICB patch draw to clear color
    {
        id<MTLTexture> zt = newTarget(device);
        id<MTLIndirectCommandBuffer> icb = newICB(device, MTLIndirectCommandTypeDrawPatches, NO, NO, 1);
        id<MTLIndirectRenderCommand> cmd = [icb indirectRenderCommandAtIndex:0];
        [cmd setRenderPipelineState:triPso];
        [cmd setVertexBuffer:triVB offset:0 atIndex:0];
        [cmd setFragmentBuffer:tintBuf offset:0 atIndex:0];
        [cmd drawPatches:3 patchStart:0 patchCount:1 patchIndexBuffer:nil patchIndexBufferOffset:0 instanceCount:1 baseInstance:0
            tessellationFactorBuffer:triFB8 tessellationFactorBufferOffset:0 tessellationFactorBufferInstanceStride:0];
        id<MTLCommandBuffer> cb = nil;
        id<MTLRenderCommandEncoder> enc = beginRender(queue, zt, &cb, @"cpu-patch-scale0");
        [enc setViewport:vp]; [enc setScissorRect:sc];
        [enc setTessellationFactorScale:0.0f];
        [enc executeCommandsInBuffer:icb withRange:NSMakeRange(0, 1)];
        [enc endEncoding];
        finish(cb, @"cpu-patch-scale0");
        expectColor(readTexture(device, queue, zt, @"cpu-patch-scale0"), 0, 0, 0, 255, @"cpu-patch-scale0");
    }

    // 2) Zero-control-point procedural patch in CPU ICB
    {
        id<MTLTexture> dt = newTarget(device), it = newTarget(device);
        id<MTLCommandBuffer> cb = nil;
        id<MTLRenderCommandEncoder> enc = beginRender(queue, dt, &cb, @"cpu-patch-0cp-d");
        [enc setViewport:vp]; [enc setScissorRect:sc];
        [enc setRenderPipelineState:zcpPso];
        [enc setFragmentBuffer:tintBuf offset:0 atIndex:0];
        [enc setTessellationFactorBuffer:triFB8 offset:0 instanceStride:0];
        [enc drawPatches:0 patchStart:0 patchCount:1 patchIndexBuffer:nil patchIndexBufferOffset:0 instanceCount:1 baseInstance:0];
        [enc endEncoding];
        finish(cb, @"cpu-patch-0cp-d");

        id<MTLIndirectCommandBuffer> icb = newICB(device, MTLIndirectCommandTypeDrawPatches, NO, NO, 1);
        id<MTLIndirectRenderCommand> cmd = [icb indirectRenderCommandAtIndex:0];
        [cmd setRenderPipelineState:zcpPso];
        [cmd setFragmentBuffer:tintBuf offset:0 atIndex:0];
        [cmd drawPatches:0 patchStart:0 patchCount:1 patchIndexBuffer:nil patchIndexBufferOffset:0 instanceCount:1 baseInstance:0
            tessellationFactorBuffer:triFB8 tessellationFactorBufferOffset:0 tessellationFactorBufferInstanceStride:0];
        cb = nil;
        enc = beginRender(queue, it, &cb, @"cpu-patch-0cp-i");
        [enc setViewport:vp]; [enc setScissorRect:sc];
        [enc executeCommandsInBuffer:icb withRange:NSMakeRange(0, 1)];
        [enc endEncoding];
        finish(cb, @"cpu-patch-0cp-i");
        expectSamePixels(readTexture(device, queue, dt, @"0cp-d"), readTexture(device, queue, it, @"0cp-i"), @"cpu-patch-0cp");
    }

    // 3) drawIndexedPatches: UInt16 & UInt32 with 4B patchIndexBuffer + baseInstance + instanceStride
    for (int is32 = 0; is32 < 2; is32++) {
        id<MTLRenderPipelineState> pso = is32 ? quadIdx32Pso : quadIdx16Pso;
        id<MTLBuffer> cpBuf = is32 ? cp32Buf : cp16Buf;
        id<MTLTexture> dt = newTarget(device), it = newTarget(device);
        id<MTLCommandBuffer> cb = nil;
        id<MTLRenderCommandEncoder> enc = beginRender(queue, dt, &cb, @"cpu-idx-patch-d");
        [enc setViewport:vp]; [enc setScissorRect:sc];
        [enc setRenderPipelineState:pso];
        [enc setVertexBuffer:quadVB offset:0 atIndex:0];
        [enc setFragmentBuffer:tintBuf offset:0 atIndex:0];
        [enc setTessellationFactorBuffer:quadFB offset:0 instanceStride:16];
        [enc drawIndexedPatches:4 patchStart:0 patchCount:1 patchIndexBuffer:pIdxBuf patchIndexBufferOffset:4
            controlPointIndexBuffer:cpBuf controlPointIndexBufferOffset:0 instanceCount:1 baseInstance:1];
        [enc endEncoding];
        finish(cb, @"cpu-idx-patch-d");

        id<MTLIndirectCommandBuffer> icb = newICB(device, MTLIndirectCommandTypeDrawIndexedPatches, NO, NO, 1);
        id<MTLIndirectRenderCommand> cmd = [icb indirectRenderCommandAtIndex:0];
        [cmd setRenderPipelineState:pso];
        [cmd setVertexBuffer:quadVB offset:0 atIndex:0];
        [cmd setFragmentBuffer:tintBuf offset:0 atIndex:0];
        [cmd drawIndexedPatches:4 patchStart:0 patchCount:1 patchIndexBuffer:pIdxBuf patchIndexBufferOffset:4
            controlPointIndexBuffer:cpBuf controlPointIndexBufferOffset:0 instanceCount:1 baseInstance:1
            tessellationFactorBuffer:quadFB tessellationFactorBufferOffset:0 tessellationFactorBufferInstanceStride:16];
        cb = nil;
        enc = beginRender(queue, it, &cb, @"cpu-idx-patch-i");
        [enc setViewport:vp]; [enc setScissorRect:sc];
        [enc executeCommandsInBuffer:icb withRange:NSMakeRange(0, 1)];
        [enc endEncoding];
        finish(cb, @"cpu-idx-patch-i");
        expectSamePixels(readTexture(device, queue, dt, @"idx-patch-d"),
                         readTexture(device, queue, it, @"idx-patch-i"),
                         is32 ? @"cpu-indexed-patch-u32" : @"cpu-indexed-patch-u16");
    }
    passCase(@"cpu-patch-and-indexed-patch-equivalence");
}

static void testCPUPatchAndComputeRefusals(id<MTLDevice> device, id<MTLCommandQueue> queue,
                                            id<MTLRenderPipelineState> nonTessPipeline) {
    NSString *src =
        @"#include <metal_stdlib>\n"
         "using namespace metal;\n"
         "struct POut { float4 pos [[position]]; };\n"
         "[[patch(triangle, 0)]]\n"
         "vertex POut neg_zcp_tes() { POut o; o.pos = float4(0, 0, 0, 1); return o; }\n"
         "fragment float4 neg_fs() { return float4(1); }\n"
         "kernel void icb_compute_noop(device uint *out [[buffer(0)]]) { out[0] = 1; }\n";
    NSError *err = nil;
    id<MTLLibrary> lib = [device newLibraryWithSource:src options:nil error:&err];
    if (!lib) fail([NSString stringWithFormat:@"neg lib: %@", err]);
    id<MTLRenderPipelineState> zcpPso = newTessPipeline(device, lib, @"neg_zcp_tes", @"neg_fs", NO, MTLTessellationControlPointIndexTypeNone, MTLTessellationFactorStepFunctionConstant);
    const uint16_t tf[4] = { f32_to_f16(2), f32_to_f16(2), f32_to_f16(2), f32_to_f16(2) };
    const uint32_t pi[2] = { 0, 0 };
    id<MTLBuffer> fb8 = newBuffer(device, tf, sizeof tf);
    id<MTLBuffer> fb4 = newBuffer(device, tf, 4);
    id<MTLBuffer> pib = newBuffer(device, pi, sizeof pi);

    // 1) control points > 32 rejected
    {
        id<MTLIndirectCommandBuffer> icb = newICB(device, MTLIndirectCommandTypeDrawPatches, NO, NO, 1);
        id<MTLIndirectRenderCommand> cmd = [icb indirectRenderCommandAtIndex:0];
        [cmd setRenderPipelineState:zcpPso];
        [cmd drawPatches:33 patchStart:0 patchCount:1 patchIndexBuffer:nil patchIndexBufferOffset:0 instanceCount:1 baseInstance:0
            tessellationFactorBuffer:fb8 tessellationFactorBufferOffset:0 tessellationFactorBufferInstanceStride:0];
        id<MTLCommandBuffer> cb = nil;
        id<MTLRenderCommandEncoder> enc = beginRender(queue, newTarget(device), &cb, @"patch-cp-33");
        [enc executeCommandsInBuffer:icb withRange:NSMakeRange(0, 1)];
        [enc endEncoding];
        expectCommandError(cb, @"patch-cp-33");
    }

    // 2) patchIndexBuffer 2-byte misaligned offset (requires 4B uint32) rejected
    {
        id<MTLIndirectCommandBuffer> icb = newICB(device, MTLIndirectCommandTypeDrawPatches, NO, NO, 1);
        id<MTLIndirectRenderCommand> cmd = [icb indirectRenderCommandAtIndex:0];
        [cmd setRenderPipelineState:zcpPso];
        [cmd drawPatches:0 patchStart:0 patchCount:1 patchIndexBuffer:pib patchIndexBufferOffset:2 instanceCount:1 baseInstance:0
            tessellationFactorBuffer:fb8 tessellationFactorBufferOffset:0 tessellationFactorBufferInstanceStride:0];
        id<MTLCommandBuffer> cb = nil;
        id<MTLRenderCommandEncoder> enc = beginRender(queue, newTarget(device), &cb, @"patch-idx-misaligned-2");
        [enc executeCommandsInBuffer:icb withRange:NSMakeRange(0, 1)];
        [enc endEncoding];
        expectCommandError(cb, @"patch-idx-misaligned-2");
    }

    // 3) tessellationFactorBuffer misaligned offset / stride and undersized factor buffer at execution rejected
    {
        id<MTLIndirectCommandBuffer> icb = newICB(device, MTLIndirectCommandTypeDrawPatches, NO, NO, 1);
        id<MTLIndirectRenderCommand> cmd = [icb indirectRenderCommandAtIndex:0];
        [cmd setRenderPipelineState:zcpPso];
        [cmd drawPatches:0 patchStart:0 patchCount:1 patchIndexBuffer:nil patchIndexBufferOffset:0 instanceCount:1 baseInstance:0
            tessellationFactorBuffer:fb8 tessellationFactorBufferOffset:2 tessellationFactorBufferInstanceStride:0];
        id<MTLCommandBuffer> cb = nil;
        id<MTLRenderCommandEncoder> enc = beginRender(queue, newTarget(device), &cb, @"patch-factor-misaligned");
        [enc executeCommandsInBuffer:icb withRange:NSMakeRange(0, 1)];
        [enc endEncoding];
        expectCommandError(cb, @"patch-factor-misaligned");

        [icb resetWithRange:NSMakeRange(0, 1)];
        [cmd setRenderPipelineState:zcpPso];
        [cmd drawPatches:0 patchStart:0 patchCount:1 patchIndexBuffer:nil patchIndexBufferOffset:0 instanceCount:1 baseInstance:0
            tessellationFactorBuffer:fb4 tessellationFactorBufferOffset:0 tessellationFactorBufferInstanceStride:0];
        cb = nil;
        enc = beginRender(queue, newTarget(device), &cb, @"patch-factor-undersized");
        [enc executeCommandsInBuffer:icb withRange:NSMakeRange(0, 1)];
        [enc endEncoding];
        expectCommandError(cb, @"patch-factor-undersized");
    }

    // 4) Non-tessellation pipeline with patch command rejected
    {
        id<MTLIndirectCommandBuffer> icb = newICB(device, MTLIndirectCommandTypeDrawPatches, NO, YES, 1);
        id<MTLIndirectRenderCommand> cmd = [icb indirectRenderCommandAtIndex:0];
        [cmd setRenderPipelineState:nonTessPipeline];
        [cmd drawPatches:0 patchStart:0 patchCount:1 patchIndexBuffer:nil patchIndexBufferOffset:0 instanceCount:1 baseInstance:0
            tessellationFactorBuffer:fb8 tessellationFactorBufferOffset:0 tessellationFactorBufferInstanceStride:0];
        id<MTLCommandBuffer> cb = nil;
        id<MTLRenderCommandEncoder> enc = beginRender(queue, newTarget(device), &cb, @"patch-nontess-pso");
        [enc executeCommandsInBuffer:icb withRange:NSMakeRange(0, 1)];
        [enc endEncoding];
        expectCommandError(cb, @"patch-nontess-pso");
    }

    // 5) Compute ICB descriptor and compute pipeline supportIndirectCommandBuffers = YES refused
    {
        MTLIndirectCommandBufferDescriptor *cd = [MTLIndirectCommandBufferDescriptor new];
        cd.commandTypes = MTLIndirectCommandTypeConcurrentDispatch;
        id<MTLIndirectCommandBuffer> cicb = [device newIndirectCommandBufferWithDescriptor:cd maxCommandCount:2 options:MTLResourceStorageModePrivate];
        if (cicb) fail(@"compute indirect command buffer descriptor was accepted unexpectedly");

        MTLComputePipelineDescriptor *cpd = [MTLComputePipelineDescriptor new];
        cpd.computeFunction = [lib newFunctionWithName:@"icb_compute_noop"];
        cpd.supportIndirectCommandBuffers = YES;
        NSError *cerr = nil;
        id<MTLComputePipelineState> cpso = [device newComputePipelineStateWithDescriptor:cpd options:0 reflection:nil error:&cerr];
        if (cpso || !cerr) fail(@"compute pipeline with supportIndirectCommandBuffers=YES was accepted unexpectedly");

        id<MTLComputePipelineState> normalCpso = [device newComputePipelineStateWithFunction:cpd.computeFunction error:&cerr];
        if (!normalCpso || normalCpso.supportIndirectCommandBuffers)
            fail(@"normal compute pipeline should advertise supportIndirectCommandBuffers == NO");
    }
    passCase(@"cpu-patch-validation-and-compute-icb-refusal");
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
                testCPUPatchEquivalence(device, queue);
                testCPUPatchAndComputeRefusals(device, queue, otherPipeline);
                testRangeRejection(device, queue);
            } else {
                testCPUPatchEquivalence(device, queue);
            }
            printf("ICB_SUITE_PASS reference=%u production_bundle=%u\n",
                   referenceMode ? 1u : 0u, referenceMode ? 0u : 1u);
            return 0;
        } @catch (NSException *exception) {
            fail(exception.description);
        }
    }
}
