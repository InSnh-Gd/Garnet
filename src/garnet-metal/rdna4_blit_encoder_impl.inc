@interface RDNA4BlitEncoder : _MTLCommandEncoder { RDNA4CommandBuffer *_cb; BOOL _ended; NSString *_lbl; NSMutableSet *_argumentResources, *_argumentWritable; }
- (instancetype)initWithCommandBuffer:(id)cb blit:(int)dummy;
- (void)copyFromBuffer:(id)src sourceOffset:(NSUInteger)so toBuffer:(id)dst destinationOffset:(NSUInteger)doff size:(NSUInteger)size;
@end

// m11h9: one buffer<->image copy region for a level / slice (2D, 1D arrays) or a box (3D: baseArrayLayer 0, depth in the extent, bufferImageHeight from bytesPerImage).
// Vulkan specifies bufferRowLength and bufferImageHeight in TEXELS (for compressed formats, multiply block counts by the 4x4 block dimension).
static VkBufferImageCopy rdna4_bic(RDNA4Texture *t, NSUInteger off, NSUInteger bpr, NSUInteger bpi, NSUInteger level, NSUInteger slice, MTLOrigin o, MTLSize sz) {
    BOOL is3 = [t textureType] == MTLTextureType3D; uint32_t bpp = [t bytesPerPixel];
    uint32_t rowTexels = bpr && bpp ? (uint32_t)((bpr / bpp) * ([t isCompressed] ? 4u : 1u)) : 0u;
    uint32_t imgHeight = (is3 && sz.depth > 1 && bpr) ? (uint32_t)((bpi / bpr) * ([t isCompressed] ? 4u : 1u)) : 0u;
    return (VkBufferImageCopy){ .bufferOffset = off, .bufferRowLength = rowTexels, .bufferImageHeight = imgHeight,
        .imageSubresource = { [t rdna4CopyAspect], [t rdna4MipLevel:level], [t rdna4Slice:slice], 1 },   // bundle 10: colour, or the one depth / stencil aspect
        .imageOffset = { (int32_t)o.x, (int32_t)o.y, is3 ? (int32_t)o.z : 0 }, .imageExtent = { (uint32_t)sz.width, (uint32_t)sz.height, is3 ? (uint32_t)sz.depth : 1 } };
}
static BOOL rdna4_blit_buffer_layout(RDNA4Texture *texture, RDNA4Buffer *buffer,
    MTLSize size, NSUInteger offset, NSUInteger *rowStride, NSUInteger *imageStride,
    VkDeviceSize *vkOffset) {
    NSUInteger rowBytes, minimumImage;
    NSUInteger bpp = [texture bytesPerPixel];
    BOOL bc = [texture isCompressed];
    NSUInteger bw = bc ? (size.width + 3) / 4 : size.width;
    NSUInteger bh = bc ? (size.height + 3) / 4 : size.height;
    if (!bpp || __builtin_mul_overflow(bw, bpp, &rowBytes)) return NO;
    NSUInteger row = *rowStride ?: rowBytes;
    NSUInteger depth = [texture textureType] == MTLTextureType3D ? size.depth : 1;
    NSUInteger image = 0;
    if (depth > 1) {
        if (!row || __builtin_mul_overflow(row, bh, &minimumImage)) return NO;
        image = *imageStride ?: minimumImage;
        if (image % row || (image / row) * (bc ? 4 : 1) > UINT32_MAX) return NO;
    }
    rdna4_texture_transfer transfer;
    if (row % bpp || (row / bpp) * (bc ? 4 : 1) > UINT32_MAX ||
        !rdna4_texture_transfer_layout(bw, bh, depth, bpp, row, image, &transfer) ||
        !rdna4_buffer_range(buffer.length, offset, transfer.span)) return NO;
    VkDeviceSize relocated;
    if (__builtin_add_overflow([buffer rdna4VkOffset], (VkDeviceSize)offset, &relocated) ||
        !rdna4nc_vk_offset_ok(relocated, [texture rdna4CopyAspect] == VK_IMAGE_ASPECT_COLOR_BIT ? bpp : 4,
            RDNA4NC_ALIGN_IMAGE)) return NO;
    *rowStride = row; *imageStride = image; *vkOffset = relocated;
    return YES;
}
// Exact buffer rows touched by VkCmdCopyImageToBuffer. Keeping rows separate avoids
// retiring sampler owners that live in destinationBytesPerRow gaps.
static NSArray<NSValue *> *rdna4_blit_texture_buffer_ranges(RDNA4Texture *texture, MTLSize size,
        NSUInteger offset, NSUInteger bytesPerRow, NSUInteger bytesPerImage,
        NSUInteger bufferLength, NSRange *envelope) {
    if (!envelope || !texture || !size.width || !size.height) return @[];
    BOOL is3 = [texture textureType] == MTLTextureType3D;
    NSUInteger depth = is3 ? size.depth : 1;
    if (!depth) return @[];
    if (offset > bufferLength) return nil;
    BOOL bc = [texture isCompressed];
    NSUInteger bw = bc ? (size.width + 3) / 4 : size.width;
    NSUInteger bh = bc ? (size.height + 3) / 4 : size.height;
    NSUInteger pixelBytes = [texture bytesPerPixel], rowBytes = 0, minimumImageBytes = 0;
    if (!pixelBytes || __builtin_mul_overflow(bw, pixelBytes, &rowBytes)) return nil;
    NSUInteger rowStride = bytesPerRow ? bytesPerRow : rowBytes;
    if (rowStride < rowBytes || __builtin_mul_overflow(rowStride, bh,
                                                       &minimumImageBytes)) return nil;
    NSUInteger imageStride = depth > 1 && bytesPerImage ? bytesPerImage : minimumImageBytes;
    if (imageStride < minimumImageBytes) return nil;

    NSMutableArray<NSValue *> *ranges = [NSMutableArray array];
    NSUInteger first = NSUIntegerMax, last = 0;
    for (NSUInteger z = 0; z < depth; ++z) {
        NSUInteger imageOffset = 0;
        if (__builtin_mul_overflow(z, imageStride, &imageOffset) ||
            imageOffset > NSUIntegerMax - offset) return nil;
        for (NSUInteger y = 0; y < bh; ++y) {
            NSUInteger rowOffset = 0, start = 0, end = 0;
            if (__builtin_mul_overflow(y, rowStride, &rowOffset) ||
                rowOffset > NSUIntegerMax - imageOffset - offset) return nil;
            start = offset + imageOffset + rowOffset;
            if (start > bufferLength || rowBytes > bufferLength - start ||
                __builtin_add_overflow(start, rowBytes, &end)) return nil;
            if (first == NSUIntegerMax) first = start;
            if (ranges.count && NSMaxRange(ranges.lastObject.rangeValue) == start) {
                NSRange merged = NSMakeRange(ranges.lastObject.rangeValue.location,
                    start + rowBytes - ranges.lastObject.rangeValue.location);
                ranges[ranges.count - 1] = [NSValue valueWithRange:merged];
            } else {
                [ranges addObject:[NSValue valueWithRange:NSMakeRange(start, rowBytes)]];
            }
            last = end;
        }
    }
    if (!ranges.count) return ranges;
    *envelope = NSMakeRange(first, last - first);
    return ranges;
}
static BOOL rdna4_blit_region_ok(RDNA4Texture *t, MTLOrigin o, MTLSize sz, NSUInteger level, NSUInteger slice, const char *what) {
    return rdna4_region_ok(t, (MTLRegion){ o, sz }, level, slice, what);
}
static void rdna4_blit_note_overwrite(RDNA4CommandBuffer *cb, RDNA4Texture *texture,
        MTLOrigin origin, MTLSize size, NSUInteger level, NSUInteger slice) {
    if (level || slice || origin.x || origin.y || origin.z || size.depth != 1 ||
        [texture textureType] != MTLTextureType2D || [texture mipmapLevelCount] != 1 ||
        [texture rdna4Aspects] != VK_IMAGE_ASPECT_COLOR_BIT) return;
    [cb rdna4NoteSurfaceOverwrite:texture width:(uint32_t)size.width height:(uint32_t)size.height level:0];
}
@implementation RDNA4BlitEncoder
- (instancetype)initWithCommandBuffer:(id)cb blit:(int)dummy {
    (void)dummy;
    self = [super initWithCommandBuffer:cb];
    if (!self) return nil;
    _cb = cb;
    RDNA4LOGR("RDNA4BlitEncoder %p", (__bridge void *)self);
    return self;
}
RDNA4_DNR(RDNA4BlitEncoder)
- (NSString *)label { return _lbl; }
- (void)setLabel:(NSString *)l { _lbl = [l copy]; }
- (NSUInteger)getType { return 3; }
- (void)copyFromTexture:(id)src sourceSlice:(NSUInteger)ss sourceLevel:(NSUInteger)sl sourceOrigin:(MTLOrigin)so sourceSize:(MTLSize)sz
               toBuffer:(id)dst destinationOffset:(NSUInteger)doff destinationBytesPerRow:(NSUInteger)bpr destinationBytesPerImage:(NSUInteger)bpi {
    if (_ended) { [_cb rdna4Fail:@"texture-to-buffer copy after endEncoding"]; return; }
    if ([_cb rdna4Error]) return;
    RDNA4LOGR("blit copyFromTexture toBuffer: slice %lu level %lu origin %lu,%lu,%lu size %lux%lux%lu offset %lu bpr %lu bpi %lu", (unsigned long)ss, (unsigned long)sl,
           (unsigned long)so.x, (unsigned long)so.y, (unsigned long)so.z, (unsigned long)sz.width, (unsigned long)sz.height, (unsigned long)sz.depth, (unsigned long)doff, (unsigned long)bpr, (unsigned long)bpi);
    if (![src isKindOfClass:[RDNA4Texture class]] || ![dst isKindOfClass:[RDNA4Buffer class]]) { [_cb rdna4Fail:@"blit copyFromTexture:toBuffer: bad classes"]; return; }
    if (rdna4nc_drop_write(dst, RDNA4NC_TEXTURE_BUFFER, "copyFromTexture:toBuffer:", "BlitCommandEncoder")) { [_cb rdna4Fail:@"copyFromTexture:toBuffer: destination is read-only"]; return; }
    RDNA4Texture *t = src; RDNA4Buffer *b = dst;
    [_cb rdna4RetainArgument:t]; [_cb rdna4RetainArgument:b];
    if (![t rdna4CopyAspect] || [t rdna4Samples] > 1) { [_cb rdna4Fail:@"blit copyFromTexture:toBuffer: a combined depth/stencil texture or a multisample texture cannot be copied to a buffer here"]; return; }   // bundle 10
    if (!rdna4_blit_region_ok(t, so, sz, sl, ss, "blit copyFromTexture:toBuffer:")) { [_cb rdna4Fail:@"blit copyFromTexture:toBuffer: region outside the texture level"]; return; }
    VkDeviceSize vkoff;
    if (!rdna4_blit_buffer_layout(t, b, sz, doff, &bpr, &bpi, &vkoff)) {
        [_cb rdna4Fail:@"blit copyFromTexture:toBuffer: invalid buffer range, stride or Vulkan offset"]; return;
    }
    if ([t rdna4IsTexel]) {
        [self copyFromBuffer:[t rdna4TexelBuffer] sourceOffset:[t bufferOffset] + so.x * [t bytesPerPixel]
            toBuffer:b destinationOffset:doff size:sz.width * [t bytesPerPixel]];
        return;
    }
    NSRange argumentRange = NSMakeRange(0, 0);
    NSArray<NSValue *> *argumentRanges = rdna4_blit_texture_buffer_ranges(t, sz, doff, bpr, bpi,
                                                                           [b length], &argumentRange);
    if (!argumentRanges) {
        [_cb rdna4Fail:@"blit copyFromTexture:toBuffer: buffer layout overflows or exceeds the destination"];
        return;
    }
    if (argumentRanges.count) {
        // Texture contents are opaque to CPU argument scanning. A mapped
        // destination is refined from its final bytes; an unmapped destination
        // must conservatively retain the owners it had before this copy.
        RDNA4ArgumentWriteIntent *intent =
            rdna4_argument_sampler_intent_ranges(b, argumentRange, argumentRanges, nil, NO);
        if (!intent) {
            [_cb rdna4Fail:@"blit copyFromTexture:toBuffer: argument-write intent is invalid"];
            return;
        }
        [_cb rdna4ArgumentIntent:intent];
    }
    rdna4_ios_use(_cb, t, YES, NO);
    VkCommandBuffer cmd = [_cb vk];
    rdna4_tex_to(cmd, t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy bic = rdna4_bic(t, vkoff, bpr, bpi, sl, ss, so, sz);
    vkCmdCopyImageToBuffer(cmd, [t vkImage], rdna4_use_layout(t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL), [b vkBuffer], 1, &bic);
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                           .dstAccessMask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}
- (void)copyFromBuffer:(id)src sourceOffset:(NSUInteger)so sourceBytesPerRow:(NSUInteger)bpr sourceBytesPerImage:(NSUInteger)bpi sourceSize:(MTLSize)sz
             toTexture:(id)dst destinationSlice:(NSUInteger)ds destinationLevel:(NSUInteger)dl destinationOrigin:(MTLOrigin)dorg {
    if (_ended) { [_cb rdna4Fail:@"buffer-to-texture copy after endEncoding"]; return; }
    if ([_cb rdna4Error]) return;
    if (![src isKindOfClass:[RDNA4Buffer class]] || ![dst isKindOfClass:[RDNA4Texture class]]) { [_cb rdna4Fail:@"blit copyFromBuffer:toTexture: bad classes"]; return; }
    if (rdna4nc_drop_write(dst, RDNA4NC_BUFFER_TEXTURE, "copyFromBuffer:toTexture:", "BlitCommandEncoder")) { [_cb rdna4Fail:@"copyFromBuffer:toTexture: destination is read-only"]; return; }
    RDNA4Texture *t = dst; [_cb rdna4RetainArgument:src]; [_cb rdna4RetainArgument:t];
    if (![t rdna4CopyAspect] || [t rdna4Samples] > 1) { [_cb rdna4Fail:@"blit copyFromBuffer:toTexture: a combined depth/stencil texture or a multisample texture cannot be filled from a buffer here"]; return; }   // bundle 10
    if (!rdna4_blit_region_ok(t, dorg, sz, dl, ds, "blit copyFromBuffer:toTexture:")) { [_cb rdna4Fail:@"blit copyFromBuffer:toTexture: region outside the texture level"]; return; }
    VkDeviceSize vkoff;
    if (!rdna4_blit_buffer_layout(t, src, sz, so, &bpr, &bpi, &vkoff)) {
        [_cb rdna4Fail:@"blit copyFromBuffer:toTexture: invalid buffer range, stride or Vulkan offset"]; return;
    }
    if ([t rdna4IsTexel]) {
        [self copyFromBuffer:src sourceOffset:so toBuffer:[t rdna4TexelBuffer]
            destinationOffset:[t bufferOffset] + dorg.x * [t bytesPerPixel] size:sz.width * [t bytesPerPixel]];
        return;
    }
    if (![(RDNA4Buffer *)src rdna4Imported]) rdna4_blit_note_overwrite(_cb, t, dorg, sz, dl, ds);
    rdna4_ios_usek(_cb, t, YES, YES, RDNA4DF_W_BLIT);
    VkCommandBuffer cmd = [_cb vk];
    rdna4_tex_to(cmd, t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy bic = rdna4_bic(t, vkoff, bpr, bpi, dl, ds, dorg, sz);
    vkCmdCopyBufferToImage(cmd, [(RDNA4Buffer *)src vkBuffer], [t vkImage], rdna4_use_layout(t, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL), 1, &bic);
    rdna4_full_barrier(cmd);
}
- (void)copyFromTexture:(id)src sourceSlice:(NSUInteger)ss sourceLevel:(NSUInteger)sl sourceOrigin:(MTLOrigin)so sourceSize:(MTLSize)sz
              toTexture:(id)dst destinationSlice:(NSUInteger)ds destinationLevel:(NSUInteger)dl destinationOrigin:(MTLOrigin)dorg {
    if (_ended) { [_cb rdna4Fail:@"texture copy after endEncoding"]; return; }
    if ([_cb rdna4Error]) return;
    if (![src isKindOfClass:[RDNA4Texture class]] || ![dst isKindOfClass:[RDNA4Texture class]]) { [_cb rdna4Fail:@"blit copyFromTexture:toTexture: bad classes"]; return; }
    if (rdna4nc_drop_write(dst, RDNA4NC_COPY_TEXTURE, "copyFromTexture:toTexture:", "BlitCommandEncoder")) { [_cb rdna4Fail:@"copyFromTexture:toTexture: destination is read-only"]; return; }
    RDNA4Texture *a = src, *b = dst; [_cb rdna4RetainArgument:a]; [_cb rdna4RetainArgument:b];
    if ([a rdna4Aspects] != [b rdna4Aspects] || [a rdna4Samples] != [b rdna4Samples]) { [_cb rdna4Fail:@"blit copyFromTexture:toTexture: the textures differ in aspects (colour / depth / stencil) or sample count"]; return; }   // bundle 10
    if (!rdna4_blit_region_ok(a, so, sz, sl, ss, "blit copyFromTexture:toTexture: (source)") || !rdna4_blit_region_ok(b, dorg, sz, dl, ds, "blit copyFromTexture:toTexture: (destination)")) {
        [_cb rdna4Fail:@"blit copyFromTexture:toTexture: region outside a texture level"]; return; }
    if ([a rdna4IsTexel] || [b rdna4IsTexel]) {
        if (a.pixelFormat != b.pixelFormat) { [_cb rdna4Fail:@"texture-buffer copy requires identical pixel formats"]; return; }
        NSUInteger row = sz.width * [a bytesPerPixel];
        if ([a rdna4IsTexel]) {
            [self copyFromBuffer:[a rdna4TexelBuffer] sourceOffset:[a bufferOffset] + so.x * [a bytesPerPixel]
                sourceBytesPerRow:row sourceBytesPerImage:0 sourceSize:sz toTexture:b
                destinationSlice:ds destinationLevel:dl destinationOrigin:dorg];
        } else {
            [self copyFromTexture:a sourceSlice:ss sourceLevel:sl sourceOrigin:so sourceSize:sz
                toBuffer:[b rdna4TexelBuffer] destinationOffset:[b bufferOffset] + dorg.x * [b bytesPerPixel]
                destinationBytesPerRow:row destinationBytesPerImage:0];
        }
        return;
    }
    BOOL sameImage = [a vkImage] == [b vkImage];
    uint32_t sourceMip = [a rdna4MipLevel:sl], destinationMip = [b rdna4MipLevel:dl];
    uint32_t sourceSlice = [a rdna4Slice:ss], destinationSlice = [b rdna4Slice:ds];
    if (sameImage && sourceMip == destinationMip && sourceSlice == destinationSlice && so.x < dorg.x + sz.width && dorg.x < so.x + sz.width &&
        so.y < dorg.y + sz.height && dorg.y < so.y + sz.height && so.z < dorg.z + sz.depth && dorg.z < so.z + sz.depth) {
        [_cb rdna4Fail:@"blit copyFromTexture:toTexture: source and destination overlap"]; return;
    }
    rdna4_ios_use(_cb, a, YES, NO);
    IOSurfaceRef sourceSurface = [a rdna4UCSurface], destinationSurface = [b rdna4UCSurface];
    if (!sameImage && destinationSurface && (sourceSurface ? IOSurfaceGetID(sourceSurface) != IOSurfaceGetID(destinationSurface)
                                                          : ![a rdna4IsIOS]))
        rdna4_blit_note_overwrite(_cb, b, dorg, sz, dl, ds);
    rdna4_ios_usek(_cb, b, YES, YES, RDNA4DF_W_BLIT);
    VkCommandBuffer cmd = [_cb vk];
    BOOL a3 = [a textureType] == MTLTextureType3D, b3 = [b textureType] == MTLTextureType3D;
    VkImageLayout sourceLayout = rdna4_use_layout(a, sameImage ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkImageLayout destinationLayout = rdna4_use_layout(b, sameImage ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    rdna4_tex_to(cmd, a, sourceLayout);
    rdna4_tex_to(cmd, b, destinationLayout);
    VkImageCopy ic = { .srcSubresource = { [a rdna4Aspects], sourceMip, sourceSlice, 1 }, .srcOffset = { (int32_t)so.x, (int32_t)so.y, a3 ? (int32_t)so.z : 0 },
        .dstSubresource = { [b rdna4Aspects], destinationMip, destinationSlice, 1 }, .dstOffset = { (int32_t)dorg.x, (int32_t)dorg.y, b3 ? (int32_t)dorg.z : 0 },
        .extent = { (uint32_t)sz.width, (uint32_t)sz.height, (a3 || b3) ? (uint32_t)sz.depth : 1 } };
    vkCmdCopyImage(cmd, [a vkImage], sourceLayout, [b vkImage], destinationLayout, 1, &ic);
    rdna4_full_barrier(cmd);
}
- (void)copyFromBuffer:(id)src sourceOffset:(NSUInteger)so toBuffer:(id)dst destinationOffset:(NSUInteger)doff size:(NSUInteger)size {
    if (_ended) { [_cb rdna4Fail:@"buffer copy after endEncoding"]; return; }
    if ([_cb rdna4Error]) return;
    if (![src isKindOfClass:[RDNA4Buffer class]] || ![dst isKindOfClass:[RDNA4Buffer class]]) { [_cb rdna4Fail:@"blit copyFromBuffer:toBuffer: bad classes"]; return; }
    if (rdna4nc_drop_write(dst, RDNA4NC_COPY_BUFFER, "copyFromBuffer:toBuffer:", "BlitCommandEncoder")) { [_cb rdna4Fail:@"copyFromBuffer:toBuffer: destination is read-only"]; return; }
    RDNA4Buffer *a = src, *b = dst;
    if (so > a.length || size > a.length - so || doff > b.length || size > b.length - doff || ((so | doff | size) & 3)) {
        [_cb rdna4Fail:@"blit buffer copy range or macOS alignment is invalid"]; return;
    }
    if (!size) return;
    VkDeviceSize srcOffset = [a rdna4VkOffset] + so, dstOffset = [b rdna4VkOffset] + doff;
    if ([a vkBuffer] == [b vkBuffer] && srcOffset < dstOffset + size && dstOffset < srcOffset + size) {
        [_cb rdna4Fail:@"blit buffer copy source and destination overlap"]; return;
    }
    if ([a rdna4Imported] && [b rdna4Imported]) {
        uintptr_t ap = (uintptr_t)a.contents + so, bp = (uintptr_t)b.contents + doff;
        if (ap < bp ? bp - ap < size : ap - bp < size) {
            [_cb rdna4Fail:@"blit buffer copy imported source and destination overlap"]; return;
        }
    }
    [_cb rdna4RetainArgument:src]; [_cb rdna4RetainArgument:dst];
    RDNA4ArgumentWriteIntent *copyIntent = rdna4_argument_sampler_copy(a, so, b, doff, size);
    if (!copyIntent) {
        [_cb rdna4Fail:@"blit buffer copy argument-write intent is invalid"];
        return;
    }
    [_cb rdna4ArgumentIntent:copyIntent];
    uint64_t ao = 0, bo = 0;
    VkDeviceMemory am = [a rdna4NativeMemory:&ao], bm = [b rdna4NativeMemory:&bo];
    [_cb rdna4NativeRecord:(RDNA4NativeOp){ .src = am, .dst = bm,
        .srcOffset = ao <= UINT64_MAX - so ? ao + so : UINT64_MAX,
        .dstOffset = bo <= UINT64_MAX - doff ? bo + doff : UINT64_MAX, .size = size }];
    // Vulkan buffer copies allow the byte offsets introduced by an unaligned import.
    VkBufferCopy bc = { srcOffset, dstOffset, size };
    vkCmdCopyBuffer([_cb rdna4BlitVK], [(RDNA4Buffer *)src vkBuffer], [(RDNA4Buffer *)dst vkBuffer], 1, &bc);
    rdna4_full_barrier([_cb rdna4BlitVK]);
}
- (void)fillBuffer:(id)buf range:(NSRange)r value:(uint8_t)v {
    if (_ended) { [_cb rdna4Fail:@"buffer fill after endEncoding"]; return; }
    if ([_cb rdna4Error]) return;
    if (![buf isKindOfClass:[RDNA4Buffer class]]) { [_cb rdna4Fail:@"blit fillBuffer: bad class"]; return; }
    if (rdna4nc_drop_write(buf, RDNA4NC_FILL, "fillBuffer:", "BlitCommandEncoder")) { [_cb rdna4Fail:@"fillBuffer: destination is read-only"]; return; }
    if (r.location > [buf length] || r.length > [buf length] - r.location) {
        [_cb rdna4Fail:@"blit fillBuffer: range outside the buffer"]; return;
    }
    if (!r.length) return;
    [_cb rdna4RetainArgument:buf];
    [_cb rdna4ArgumentWrite:buf range:r candidates:nil fillOnly:YES];
    uint32_t w = v * 0x01010101u;
    uint64_t bo = 0; VkDeviceMemory bm = [(RDNA4Buffer *)buf rdna4NativeMemory:&bo];
    [_cb rdna4NativeRecord:(RDNA4NativeOp){ .dst = bm,
        .dstOffset = bo <= UINT64_MAX - r.location ? bo + r.location : UINT64_MAX,
        .size = r.length, .pattern = w, .fill = true }];
    VkDeviceSize dstOffset = [(RDNA4Buffer *)buf rdna4VkOffset] + r.location;
    NSUInteger head = MIN(r.length, (4 - (dstOffset & 3)) & 3);
    NSUInteger tail = (r.length - head) & 3, middle = r.length - head - tail;
    RDNA4Buffer *edge = nil; NSUInteger offset = 0;
    // Metal accepts byte ranges; Vulkan fill only accepts whole aligned words.
    if ((head || tail) && ![_cb rdna4Bytes:&w length:sizeof w buffer:&edge offset:&offset]) return;
    if (middle) vkCmdFillBuffer([_cb rdna4BlitVK], [(RDNA4Buffer *)buf vkBuffer], dstOffset + head, middle, w);
    VkBufferCopy edges[2]; uint32_t count = 0;
    if (head) edges[count++] = (VkBufferCopy){ [edge rdna4VkOffset] + offset, dstOffset, head };
    if (tail) edges[count++] = (VkBufferCopy){ [edge rdna4VkOffset] + offset, dstOffset + head + middle, tail };
    if (count) vkCmdCopyBuffer([_cb vk], [edge vkBuffer], [(RDNA4Buffer *)buf vkBuffer], count, edges);
    rdna4_full_barrier([_cb rdna4BlitVK]);
}
- (void)updateFence:(id)f {
    if (_ended) { [_cb rdna4Fail:@"blit updateFence after endEncoding"]; return; }
    if ([_cb rdna4Error]) return;
    [_cb rdna4UpdateFence:f];
}
- (void)waitForFence:(id)f {
    if (_ended) { [_cb rdna4Fail:@"blit waitForFence after endEncoding"]; return; }
    if ([_cb rdna4Error]) return;
    [_cb rdna4WaitForFence:f];
}
RDNA4_ENCODER_SHARED
// m11h9: a box-filtered chain, level L-1 -> L with vkCmdBlitImage (linear filter when RADV can filter the format, else nearest) inside one GENERAL-layout image (source and
// destination are different levels, so the regions never overlap); each blit is ordered after the previous by a full memory barrier.
- (void)generateMipmapsForTexture:(id)tex {
    if (![tex isKindOfClass:[RDNA4Texture class]]) { [_cb rdna4Fail:@"blit generateMipmapsForTexture: bad class"]; return; }
    if (rdna4nc_drop_write(tex, RDNA4NC_MIPMAPS, "generateMipmapsForTexture:", "BlitCommandEncoder")) { [_cb rdna4Fail:@"generateMipmapsForTexture: destination is read-only"]; return; }
    RDNA4Texture *t = tex; uint32_t nl = (uint32_t)[t mipmapLevelCount];
    if ([t rdna4IsDS] || [t rdna4Samples] > 1 || [t isCompressed]) { [_cb rdna4Fail:@"blit generateMipmapsForTexture: depth/stencil, multisample and compressed textures cannot be mip-mapped here"]; return; }   // bundle 10
    if (nl <= 1) { RDNA4_ONCE("generateMipmapsForTexture: the texture has a single level; nothing to generate"); return; }
    VkFormatProperties fp = {0}; vkGetPhysicalDeviceFormatProperties(RDNA4R.pd, [t vkFormat], &fp);
    if ((fp.optimalTilingFeatures & (VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT)) != (VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT)) {
        [_cb rdna4Fail:[NSString stringWithFormat:@"generateMipmapsForTexture: vk format %d cannot be blitted on RADV (features 0x%x)", [t vkFormat], fp.optimalTilingFeatures]]; return; }
    VkFilter fl = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    [_cb rdna4RetainArgument:t];
    VkCommandBuffer cmd = [_cb vk];
    rdna4_tex_to(cmd, t, VK_IMAGE_LAYOUT_GENERAL);
    for (uint32_t L = 1; L < nl; L++) {
        rdna4_full_barrier(cmd);
        int32_t sw = (int32_t)MAX((NSUInteger)1, [t width] >> (L - 1)), sh = (int32_t)MAX((NSUInteger)1, [t height] >> (L - 1)), sd = (int32_t)MAX((NSUInteger)1, [t depth] >> (L - 1));
        int32_t dw = (int32_t)MAX((NSUInteger)1, [t width] >> L), dh = (int32_t)MAX((NSUInteger)1, [t height] >> L), dd = (int32_t)MAX((NSUInteger)1, [t depth] >> L);
        VkImageBlit bl = { .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, [t rdna4MipLevel:L - 1], [t rdna4Slice:0], [t rdna4Layers] }, .srcOffsets = { { 0, 0, 0 }, { sw, sh, sd } },
                           .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, [t rdna4MipLevel:L], [t rdna4Slice:0], [t rdna4Layers] }, .dstOffsets = { { 0, 0, 0 }, { dw, dh, dd } } };
        vkCmdBlitImage(cmd, [t vkImage], VK_IMAGE_LAYOUT_GENERAL, [t vkImage], VK_IMAGE_LAYOUT_GENERAL, 1, &bl, fl);
    }
    rdna4_full_barrier(cmd);
    RDNA4LOG("blit generateMipmapsForTexture: %u levels of %lux%lux%lu (vk format %d, %s filter)", nl, (unsigned long)[t width], (unsigned long)[t height], (unsigned long)[t depth], [t vkFormat], fl == VK_FILTER_LINEAR ? "linear" : "nearest");
}
- (void)copyFromTexture:(id)src sourceSlice:(NSUInteger)ss sourceLevel:(NSUInteger)sl toTexture:(id)dst
      destinationSlice:(NSUInteger)ds destinationLevel:(NSUInteger)dl sliceCount:(NSUInteger)sc levelCount:(NSUInteger)lc {
    if (_ended) { [_cb rdna4Fail:@"texture copy after endEncoding"]; return; }
    if ([_cb rdna4Error]) return;
    if (![src isKindOfClass:[RDNA4Texture class]] || ![dst isKindOfClass:[RDNA4Texture class]]) {
        [_cb rdna4Fail:@"blit texture range copy: bad classes"]; return;
    }
    RDNA4Texture *a = src, *b = dst;
    NSUInteger al = [a mipmapLevelCount], bl = [b mipmapLevelCount], as = [a rdna4Layers], bs = [b rdna4Layers];
    if (a.pixelFormat != b.pixelFormat || [a rdna4Samples] != [b rdna4Samples]) {
        [_cb rdna4Fail:@"blit texture range copy: pixel format or sample count differs"]; return;
    }
    if (sl > al || lc > al - sl || dl > bl || lc > bl - dl || ss > as || sc > as - ss || ds > bs || sc > bs - ds) {
        [_cb rdna4Fail:@"blit texture range copy: mip or slice range outside a texture"]; return;
    }
    if (!sc || !lc) return;
    if (MAX((NSUInteger)1, a.width >> sl) != MAX((NSUInteger)1, b.width >> dl) ||
        MAX((NSUInteger)1, a.height >> sl) != MAX((NSUInteger)1, b.height >> dl) ||
        MAX((NSUInteger)1, a.depth >> sl) != MAX((NSUInteger)1, b.depth >> dl)) {
        [_cb rdna4Fail:@"blit texture range copy: starting mip dimensions differ"]; return;
    }
    uint32_t sourceMip = [a rdna4MipLevel:sl], destinationMip = [b rdna4MipLevel:dl];
    uint32_t sourceSlice = [a rdna4Slice:ss], destinationSlice = [b rdna4Slice:ds];
    if ([a vkImage] == [b vkImage] && sourceSlice < destinationSlice + sc && destinationSlice < sourceSlice + sc &&
        sourceMip < destinationMip + lc && destinationMip < sourceMip + lc) {
        [_cb rdna4Fail:@"blit texture range copy: source and destination overlap"]; return;
    }
    for (NSUInteger level = 0; level < lc; ++level) {
        MTLSize size = MTLSizeMake(MAX((NSUInteger)1, a.width >> (sl + level)),
            MAX((NSUInteger)1, a.height >> (sl + level)), MAX((NSUInteger)1, a.depth >> (sl + level)));
        for (NSUInteger slice = 0; slice < sc; ++slice)
            [self copyFromTexture:a sourceSlice:ss + slice sourceLevel:sl + level sourceOrigin:MTLOriginMake(0,0,0) sourceSize:size
                toTexture:b destinationSlice:ds + slice destinationLevel:dl + level destinationOrigin:MTLOriginMake(0,0,0)];
    }
}
- (void)copyFromTexture:(id)src toTexture:(id)dst {
    if (![src isKindOfClass:[RDNA4Texture class]] || ![dst isKindOfClass:[RDNA4Texture class]]) {
        [_cb rdna4Fail:@"blit copyFromTexture:toTexture: bad classes"]; return;
    }
    RDNA4Texture *a = src, *b = dst;
    NSUInteger sl = 0, dl = 0;
    BOOL matched = NO;
    for (NSUInteger level = 0; level < a.mipmapLevelCount; ++level) {
        if (MAX((NSUInteger)1, a.width >> level) == b.width && MAX((NSUInteger)1, a.height >> level) == b.height &&
            MAX((NSUInteger)1, a.depth >> level) == b.depth) { sl = level; matched = YES; break; }
    }
    if (!matched) for (NSUInteger level = 1; level < b.mipmapLevelCount; ++level) {
        if (MAX((NSUInteger)1, b.width >> level) == a.width && MAX((NSUInteger)1, b.height >> level) == a.height &&
            MAX((NSUInteger)1, b.depth >> level) == a.depth) { dl = level; matched = YES; break; }
    }
    if (!matched) { [_cb rdna4Fail:@"blit copyFromTexture:toTexture: no mip matches the smaller texture"]; return; }
    [self copyFromTexture:a sourceSlice:0 sourceLevel:sl toTexture:b destinationSlice:0 destinationLevel:dl
        sliceCount:MIN([a rdna4Layers], [b rdna4Layers]) levelCount:MIN(a.mipmapLevelCount - sl, b.mipmapLevelCount - dl)];
}
// the MTLBlitOption variants: option 0 (none) = the plain call; PVRTC row-linear options are not supported
- (void)copyFromTexture:(id)src sourceSlice:(NSUInteger)ss sourceLevel:(NSUInteger)sl sourceOrigin:(MTLOrigin)so sourceSize:(MTLSize)sz
               toBuffer:(id)dst destinationOffset:(NSUInteger)doff destinationBytesPerRow:(NSUInteger)bpr destinationBytesPerImage:(NSUInteger)bpi options:(NSUInteger)o {
    if (o) { [_cb rdna4Fail:@"blit copyFromTexture:toBuffer:options: blit options are not supported"]; return; }
    [self copyFromTexture:src sourceSlice:ss sourceLevel:sl sourceOrigin:so sourceSize:sz toBuffer:dst destinationOffset:doff destinationBytesPerRow:bpr destinationBytesPerImage:bpi];
}
- (void)copyFromBuffer:(id)src sourceOffset:(NSUInteger)so sourceBytesPerRow:(NSUInteger)bpr sourceBytesPerImage:(NSUInteger)bpi sourceSize:(MTLSize)sz
             toTexture:(id)dst destinationSlice:(NSUInteger)ds destinationLevel:(NSUInteger)dl destinationOrigin:(MTLOrigin)dorg options:(NSUInteger)o {
    if (o) { [_cb rdna4Fail:@"blit copyFromBuffer:toTexture:options: blit options are not supported"]; return; }
    [self copyFromBuffer:src sourceOffset:so sourceBytesPerRow:bpr sourceBytesPerImage:bpi sourceSize:sz toTexture:dst destinationSlice:ds destinationLevel:dl destinationOrigin:dorg];
}
- (void)copyFromTexture:(id)src sourceSlice:(NSUInteger)ss sourceLevel:(NSUInteger)sl sourceOrigin:(MTLOrigin)so sourceSize:(MTLSize)sz
              toTexture:(id)dst destinationSlice:(NSUInteger)ds destinationLevel:(NSUInteger)dl destinationOrigin:(MTLOrigin)dorg options:(NSUInteger)o {
    if (o) { [_cb rdna4Fail:@"blit copyFromTexture:toTexture:options: blit options are not supported"]; return; }
    [self copyFromTexture:src sourceSlice:ss sourceLevel:sl sourceOrigin:so sourceSize:sz toTexture:dst destinationSlice:ds destinationLevel:dl destinationOrigin:dorg];
}
- (void)synchronizeResource:(id)r { (void)r; }   // Managed resources: host-visible coherent memory, nothing to flush
- (void)synchronizeTexture:(id)t slice:(NSUInteger)s level:(NSUInteger)l { (void)t; (void)s; (void)l; }
- (void)pushDebugGroup:(NSString *)s { (void)s; }
- (void)popDebugGroup {}
- (void)endEncoding {
    if (_ended) return;
    _ended = YES;
    rdna4_full_barrier([_cb rdna4BlitVK]);
    [_cb rdna4ArgumentCheckpoint];
    RDNA4LOG("RDNA4BlitEncoder endEncoding");
    [super endEncoding];
}
@end

