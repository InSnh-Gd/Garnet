#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "garnet_async_compute.h"

static int passed = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        return 1; \
    } \
    passed++; \
} while (0)

static size_t count_substr(const char *hay, const char *needle) {
    size_t n = 0, nlen = strlen(needle);
    for (const char *p = hay; p && (p = strstr(p, needle)) != NULL; p += nlen) n++;
    return n;
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    // 1. Device gate: enabled only when dedicated compute QF + timelineSemaphore + non-WS + no kill switch.
    CHECK(garnet_async_compute_device_enable(1, 1, 0, 0) == 1);
    CHECK(garnet_async_compute_device_enable(0, 1, 0, 0) == 0);
    CHECK(garnet_async_compute_device_enable(1, 0, 0, 0) == 0);
    CHECK(garnet_async_compute_device_enable(1, 1, 1, 0) == 0); // WindowServer always pinned to GFX
    CHECK(garnet_async_compute_device_enable(1, 1, 0, 1) == 0); // kill switch disables

    // 2. Segment switch gate: only pristine segment 0 on non-WS async-enabled device.
    CHECK(garnet_async_compute_can_switch_segment(1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0) == 1);
    CHECK(garnet_async_compute_can_switch_segment(0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0) == 0);
    CHECK(garnet_async_compute_can_switch_segment(1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0) == 0);
    CHECK(garnet_async_compute_can_switch_segment(1, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0) == 0);
    CHECK(garnet_async_compute_can_switch_segment(1, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0) == 0);
    CHECK(garnet_async_compute_can_switch_segment(1, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0) == 0);
    CHECK(garnet_async_compute_can_switch_segment(1, 0, 1, 0, 0, 1, 0, 0, 0, 0, 0) == 0);
    CHECK(garnet_async_compute_can_switch_segment(1, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0) == 0);
    CHECK(garnet_async_compute_can_switch_segment(1, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0) == 0);
    CHECK(garnet_async_compute_can_switch_segment(1, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0) == 0);
    CHECK(garnet_async_compute_can_switch_segment(1, 0, 1, 0, 0, 0, 0, 0, 0, 1, 0) == 0);
    CHECK(garnet_async_compute_can_switch_segment(1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1) == 0);

    // 3. Texture compatibility & fallback gate.
    CHECK(garnet_async_compute_texture_ok(0, 0, 0) == 1);
    CHECK(garnet_async_compute_texture_ok(1, 0, 0) == 0); // IOSurface stays on GFX
    CHECK(garnet_async_compute_texture_ok(0, 1, 0) == 0); // Depth/stencil stays on GFX
    CHECK(garnet_async_compute_texture_ok(0, 0, 1) == 0); // Attachment layout stays on GFX

    CHECK(garnet_async_compute_needs_gfx_fallback(0, 1, 1, 1) == 0); // Already GFX
    CHECK(garnet_async_compute_needs_gfx_fallback(1, 0, 0, 0) == 0); // Pure compute stays on computeQ
    CHECK(garnet_async_compute_needs_gfx_fallback(1, 1, 0, 0) == 1); // Arg prep -> GFX
    CHECK(garnet_async_compute_needs_gfx_fallback(1, 0, 1, 0) == 1); // ICB/RT owners -> GFX
    CHECK(garnet_async_compute_needs_gfx_fallback(1, 0, 0, 1) == 1); // Incompatible texture -> GFX

    // 4. Same-queue FIFO timeline ordering: GFX -> GFX -> Compute -> Compute -> GFX -> Compute.
    garnet_async_qorder_t q = { 0, 0 };
    garnet_async_submit_order_t o;

    o = garnet_async_qorder_next(&q, 0); // GFX #1 (initial)
    CHECK(o.wait_value == 0u && o.signal_value == 1u);
    CHECK(garnet_async_qorder_cross_wait(&q, 0) == 0u);
    CHECK(garnet_async_qorder_cross_wait(&q, 1) == 1u);

    o = garnet_async_qorder_next(&q, 0); // GFX #2 (same queue -> no wait)
    CHECK(o.wait_value == 0u && o.signal_value == 2u);
    CHECK(garnet_async_qorder_cross_wait(&q, 1) == 2u);

    o = garnet_async_qorder_next(&q, 1); // Compute #1 (cross-queue -> wait on 2)
    CHECK(o.wait_value == 2u && o.signal_value == 3u);
    CHECK(garnet_async_qorder_cross_wait(&q, 1) == 0u);
    CHECK(garnet_async_qorder_cross_wait(&q, 0) == 3u);

    o = garnet_async_qorder_next(&q, 1); // Compute #2 (same queue -> no wait)
    CHECK(o.wait_value == 0u && o.signal_value == 4u);

    o = garnet_async_qorder_next(&q, 0); // GFX #3 (cross-queue -> wait on 4)
    CHECK(o.wait_value == 4u && o.signal_value == 5u);

    o = garnet_async_qorder_next(&q, 1); // Compute #3 (cross-queue -> wait on 5)
    CHECK(o.wait_value == 5u && o.signal_value == 6u);

    // 5. Pure compute queue starting with Compute -> Compute -> GFX.
    garnet_async_qorder_t q2 = { 0, 0 };
    o = garnet_async_qorder_next(&q2, 1);
    CHECK(o.wait_value == 0u && o.signal_value == 1u);
    o = garnet_async_qorder_next(&q2, 1);
    CHECK(o.wait_value == 0u && o.signal_value == 2u);
    o = garnet_async_qorder_next(&q2, 0);
    CHECK(o.wait_value == 2u && o.signal_value == 3u);

    // 6. Concurrent queue-family sharing helpers for resources touched on both GFX and computeQ.
    CHECK(garnet_async_compute_use_concurrent_buffer(0, 0, 1) == 0);
    CHECK(garnet_async_compute_use_concurrent_buffer(1, 0, 0) == 0);
    CHECK(garnet_async_compute_use_concurrent_buffer(1, 0, 1) == 1);

    CHECK(garnet_async_compute_use_concurrent_image(0, 0, 1, 0) == 0);
    CHECK(garnet_async_compute_use_concurrent_image(1, 0, 0, 0) == 0);
    CHECK(garnet_async_compute_use_concurrent_image(1, 0, 1, 0) == 1);
    CHECK(garnet_async_compute_use_concurrent_image(1, 0, 1, 1) == 0); // depth/stencil stays EXCLUSIVE

    // 7. Source pin: every VkImageCreateInfo / VkBufferCreateInfo in garnet_resources.m
    // (2 heap helpers + 6 non-heap IOSurface/vram_surf sites = 8 total) sets VK_SHARING_MODE_CONCURRENT
    // when async compute is enabled, and DRM modifier query mirrors ic.sharingMode.
    char rpath[1024];
    snprintf(rpath, sizeof rpath, "%s/garnet_resources.m", dir);
    FILE *rf = fopen(rpath, "rb");
    CHECK(rf != NULL);
    if (rf) {
        fseek(rf, 0, SEEK_END);
        long sz = ftell(rf);
        fseek(rf, 0, SEEK_SET);
        char *buf = malloc((size_t)sz + 1);
        CHECK(buf != NULL);
        if (buf) {
            CHECK(fread(buf, 1, (size_t)sz, rf) == (size_t)sz);
            buf[sz] = 0;
            const size_t imgCreates = count_substr(buf, "VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO");
            const size_t bufCreates = count_substr(buf, "VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO");
            const size_t concAssign = count_substr(buf, "VK_SHARING_MODE_CONCURRENT");
            CHECK(imgCreates == 5u && bufCreates == 3u);
            CHECK(concAssign == imgCreates + bufCreates);
            CHECK(strstr(buf, ".sharingMode = ic.sharingMode") != NULL);
            free(buf);
        }
        fclose(rf);
    }

    printf("test-async-compute: %d/%d checks passed\n", passed, passed);
    return 0;
}
