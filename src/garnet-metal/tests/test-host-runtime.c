// test-host-runtime.c: consolidated host C11 tests for GarnetMetal runtime policies,
// capabilities, T1 telemetry/cache header, async compute, CB ordering, draw cache,
// queue pipelining, and shader pipeline hot-swap.
#include <assert.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "garnet_query_caps.h"
#include "garnet_t1.h"
#include "garnet_async_compute.h"
#include "garnet_cblog.h"
#include "garnet_drawcache.h"
#include "garnet_queue_pipe.h"
#include "garnet_hotswap.h"
#include "garnet_devcaps.h"

static unsigned g_checks, g_fails;

#define CHECK(name, cond) do { \
    g_checks++; \
    if (!(cond)) { \
        g_fails++; \
        fprintf(stderr, "FAIL [%s:%d] %s\n", __FILE__, __LINE__, (name)); \
    } \
} while (0)

static void suite_query_caps(void) {
    rdna4x_backend_capabilities c = rdna4x_query_caps(1, 1, 32, 0x10, 0x80);
    CHECK("qcaps supported+quad", rdna4x_query_supported(&c) && rdna4x_query_uses_quad(&c));
    c.subgroup_size = 3; CHECK("qcaps subgroup<4 no quad", rdna4x_query_supported(&c) && !rdna4x_query_uses_quad(&c));
    c.subgroup_size = 4; c.subgroup_supported_stages = 0x20; CHECK("qcaps wrong stage", !rdna4x_query_uses_quad(&c));
    c.subgroup_supported_stages = 0x10; c.subgroup_supported_operations = 0; CHECK("qcaps zero ops", !rdna4x_query_uses_quad(&c));
    c = rdna4x_query_caps(0, 1, 32, 0x10, 0x80); CHECK("qcaps missing language", !rdna4x_query_supported(&c));
    c = rdna4x_query_caps(1, 0, 32, 0x10, 0x80); CHECK("qcaps missing device", !rdna4x_query_supported(&c));
    c = rdna4x_query_caps(1, 1, 0, 0, 0); CHECK("qcaps zero subgroup", rdna4x_query_supported(&c) && !rdna4x_query_uses_quad(&c));
    c.flags |= 4; CHECK("qcaps unknown flags", !rdna4x_query_caps_valid(&c));
    c = rdna4x_query_caps(1, 1, 32, 0x10, 0x80); c.version++; CHECK("qcaps bad version", !rdna4x_query_caps_valid(&c));
    c.version--; c.struct_size--; CHECK("qcaps bad struct_size", !rdna4x_query_caps_valid(&c));
    CHECK("qcaps NULL", !rdna4x_query_supported(NULL));
}

static rdna4t_hist g_t1_hist;
static void *t1_worker(void *a) {
    (void)a;
    for (int i = 0; i < 100000; i++) rdna4t_add(&g_t1_hist, 1000 + (uint64_t)(i % 7));
    return NULL;
}

static void suite_t1(void) {
    CHECK("t1 bucket edges",
          rdna4t_bucket(0) == 0 && rdna4t_bucket(99999) == 0 && rdna4t_bucket(100000) == 1 &&
          rdna4t_bucket(399999) == 1 && rdna4t_bucket(400000) == 2 &&
          rdna4t_bucket(409599999ULL) == 6 && rdna4t_bucket(409600000ULL) == 7 && rdna4t_bucket(~0ULL) == 7);
    rdna4t_snap s; rdna4t_take(&g_t1_hist, &s);
    CHECK("t1 empty", s.n == 0 && s.min == 0 && s.max == 0);
    char buf[512];
    CHECK("t1 empty fmt", rdna4t_fmt("x", &s, buf, sizeof buf) == 0);
    rdna4t_add(&g_t1_hist, 2000000); rdna4t_add(&g_t1_hist, 500000); rdna4t_add(&g_t1_hist, 30000000);
    rdna4t_take(&g_t1_hist, &s);
    CHECK("t1 three samples",
          s.n == 3 && s.sum == 32500000 && s.min == 500000 && s.max == 30000000 &&
          s.b[2] == 1 && s.b[3] == 1 && s.b[5] == 1 && s.b[0] + s.b[1] + s.b[4] + s.b[6] + s.b[7] == 0);
    rdna4t_fmt("pipe", &s, buf, sizeof buf);
    CHECK("t1 fmt content", strstr(buf, "T1 pipe n=3 avg=10.833ms min=0.500ms max=30.000ms") != NULL);
    rdna4t_take(&g_t1_hist, &s); CHECK("t1 drained", s.n == 0);
    rdna4t_add(&g_t1_hist, 0); rdna4t_take(&g_t1_hist, &s); CHECK("t1 zero-ns min 0", s.n == 1 && s.min == 0 && s.max == 0);
    pthread_t t[4];
    for (int i = 0; i < 4; i++) pthread_create(&t[i], NULL, t1_worker, NULL);
    for (int i = 0; i < 4; i++) pthread_join(t[i], NULL);
    rdna4t_take(&g_t1_hist, &s);
    CHECK("t1 4 threads", s.n == 400000 && s.min == 1000 && s.max == 1006 && s.b[0] == 400000);

    uint8_t key[32], uuid[16], key2[32], uuid2[16];
    for (int i = 0; i < 32; i++) { key[i] = (uint8_t)i; key2[i] = (uint8_t)(i ^ 0x55); }
    for (int i = 0; i < 16; i++) { uuid[i] = (uint8_t)(0xA0 + i); uuid2[i] = (uint8_t)i; }
    size_t plen = 1000; uint8_t *pay = malloc(plen);
    assert(pay);
    for (size_t i = 0; i < plen; i++) pay[i] = (uint8_t)(i * 7);
    uint32_t vk[4] = { 32, 1, 0x1002, 0x7550 }; memcpy(pay, vk, 16); memcpy(pay + 16, uuid, 16);
    size_t flen = sizeof(rdna4pc_hdr) + plen; uint8_t *f = malloc(flen);
    assert(f);
    rdna4pc_hdr h; rdna4pc_fill(&h, key, uuid, 0x1002, 0x7550, pay, plen);
    memcpy(f, &h, sizeof h); memcpy(f + sizeof h, pay, plen);
    const void *pp = NULL; size_t pl = 0;
    CHECK("t1 pc hdr size 88", sizeof(rdna4pc_hdr) == 88);
    CHECK("t1 pc valid", rdna4pc_check(f, flen, key, uuid, 0x1002, 0x7550, &pp, &pl) == RDNA4PC_OK && pl == plen && pp == f + sizeof h);
    CHECK("t1 pc truncated", rdna4pc_check(f, flen - 1, key, uuid, 0x1002, 0x7550, &pp, &pl) == RDNA4PC_LEN);
    CHECK("t1 pc short", rdna4pc_check(f, sizeof h - 1, key, uuid, 0x1002, 0x7550, &pp, &pl) == RDNA4PC_SHORT);
    CHECK("t1 pc empty", rdna4pc_check(f, 0, key, uuid, 0x1002, 0x7550, &pp, &pl) == RDNA4PC_SHORT);
    CHECK("t1 pc key mismatch", rdna4pc_check(f, flen, key2, uuid, 0x1002, 0x7550, &pp, &pl) == RDNA4PC_KEY);
    CHECK("t1 pc uuid mismatch", rdna4pc_check(f, flen, key, uuid2, 0x1002, 0x7550, &pp, &pl) == RDNA4PC_UUID);
    CHECK("t1 pc devid mismatch", rdna4pc_check(f, flen, key, uuid, 0x1002, 0x7551, &pp, &pl) == RDNA4PC_UUID);
    f[sizeof h + 500] ^= 1; CHECK("t1 pc payload bitflip", rdna4pc_check(f, flen, key, uuid, 0x1002, 0x7550, &pp, &pl) == RDNA4PC_HASH); f[sizeof h + 500] ^= 1;
    f[0] = 'X'; CHECK("t1 pc bad magic", rdna4pc_check(f, flen, key, uuid, 0x1002, 0x7550, &pp, &pl) == RDNA4PC_MAGIC); f[0] = 'N';
    f[8] = 2; CHECK("t1 pc bad version", rdna4pc_check(f, flen, key, uuid, 0x1002, 0x7550, &pp, &pl) == RDNA4PC_VERSION_BAD); f[8] = 1;
    pay[8] = 9; rdna4pc_fill(&h, key, uuid, 0x1002, 0x7550, pay, plen); memcpy(f, &h, sizeof h); memcpy(f + sizeof h, pay, plen);
    CHECK("t1 pc inner vk vendor", rdna4pc_check(f, flen, key, uuid, 0x1002, 0x7550, &pp, &pl) == RDNA4PC_VKHDR);
    pay[8] = 0x02; pay[9] = 0x10; pay[10] = 0; pay[11] = 0; memcpy(pay + 16, uuid2, 16);
    rdna4pc_fill(&h, key, uuid, 0x1002, 0x7550, pay, plen); memcpy(f, &h, sizeof h); memcpy(f + sizeof h, pay, plen);
    CHECK("t1 pc inner vk uuid", rdna4pc_check(f, flen, key, uuid, 0x1002, 0x7550, &pp, &pl) == RDNA4PC_VKHDR);
    free(f); free(pay);

    CHECK("t1 max payload bounds", rdna4pc_max_payload(1) == (32ULL << 20) && rdna4pc_max_payload(0) == (256ULL << 20));
    {
        uint8_t pay2[128]; memset(pay2, 0x3C, sizeof pay2);
        uint32_t vk2[4] = { 32, 1, 0x1002, 0x7590 }; memcpy(pay2, vk2, 16); memcpy(pay2 + 16, uuid, 16);
        size_t fl2 = sizeof(rdna4pc_hdr) + sizeof pay2; uint8_t f2[sizeof(rdna4pc_hdr) + sizeof pay2];
        rdna4pc_hdr h2; rdna4pc_fill(&h2, key, uuid, 0x1002, 0x7590, pay2, sizeof pay2);
        memcpy(f2, &h2, sizeof h2); memcpy(f2 + sizeof h2, pay2, sizeof pay2);
        CHECK("t1 bounded accepts <= max", rdna4pc_check_bounded(f2, fl2, key, uuid, 0x1002, 0x7590, 128, &pp, &pl) == RDNA4PC_OK);
        CHECK("t1 bounded rejects > max", rdna4pc_check_bounded(f2, fl2, key, uuid, 0x1002, 0x7590, 64, &pp, &pl) == RDNA4PC_LEN);
    }
    const uint64_t sec = 1000000000ULL;
    CHECK("t1 inc save no new pipes", rdna4pc_should_incremental_save(900 * 1000000ULL, 4, 4, 20 * sec, 10 * sec) == 0);
    CHECK("t1 inc save slow first", rdna4pc_should_incremental_save(500 * 1000000ULL, 1, 0, 1 * sec, 0) == 1);
    CHECK("t1 inc save debounce <5s", rdna4pc_should_incremental_save(900 * 1000000ULL, 20, 5, 14 * sec, 10 * sec) == 0);
    CHECK("t1 inc save debounce >=5s", rdna4pc_should_incremental_save(500 * 1000000ULL, 6, 5, 15 * sec, 10 * sec) == 1);
    CHECK("t1 inc save fast <8", rdna4pc_should_incremental_save(10 * 1000000ULL, 12, 5, 20 * sec, 10 * sec) == 0);
    CHECK("t1 inc save batch >=8", rdna4pc_should_incremental_save(10 * 1000000ULL, 13, 5, 20 * sec, 10 * sec) == 1);
}

static void suite_async_compute(void) {
    CHECK("async device enable", garnet_async_compute_device_enable(1, 1, 0, 0) == 1);
    CHECK("async no dedicated qf", garnet_async_compute_device_enable(0, 1, 0, 0) == 0);
    CHECK("async no timeline", garnet_async_compute_device_enable(1, 0, 0, 0) == 0);
    CHECK("async ws pinned gfx", garnet_async_compute_device_enable(1, 1, 1, 0) == 0);
    CHECK("async kill switch", garnet_async_compute_device_enable(1, 1, 0, 1) == 0);

    CHECK("async switch pristine", garnet_async_compute_can_switch_segment(1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0) == 1);
    CHECK("async switch disabled", garnet_async_compute_can_switch_segment(0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0) == 0);
    CHECK("async switch already compute", garnet_async_compute_can_switch_segment(1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0) == 0);
    CHECK("async switch seg!=1", garnet_async_compute_can_switch_segment(1, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0) == 0);
    CHECK("async switch blocked flags 1..8",
          garnet_async_compute_can_switch_segment(1, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0) == 0 &&
          garnet_async_compute_can_switch_segment(1, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0) == 0 &&
          garnet_async_compute_can_switch_segment(1, 0, 1, 0, 0, 1, 0, 0, 0, 0, 0) == 0 &&
          garnet_async_compute_can_switch_segment(1, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0) == 0 &&
          garnet_async_compute_can_switch_segment(1, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0) == 0 &&
          garnet_async_compute_can_switch_segment(1, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0) == 0 &&
          garnet_async_compute_can_switch_segment(1, 0, 1, 0, 0, 0, 0, 0, 0, 1, 0) == 0 &&
          garnet_async_compute_can_switch_segment(1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1) == 0);

    CHECK("async texture ok",
          garnet_async_compute_texture_ok(0, 0, 0) == 1 &&
          garnet_async_compute_texture_ok(1, 0, 0) == 0 &&
          garnet_async_compute_texture_ok(0, 1, 0) == 0 &&
          garnet_async_compute_texture_ok(0, 0, 1) == 0);
    CHECK("async gfx fallback",
          garnet_async_compute_needs_gfx_fallback(0, 1, 1, 1) == 0 &&
          garnet_async_compute_needs_gfx_fallback(1, 0, 0, 0) == 0 &&
          garnet_async_compute_needs_gfx_fallback(1, 1, 0, 0) == 1 &&
          garnet_async_compute_needs_gfx_fallback(1, 0, 1, 0) == 1 &&
          garnet_async_compute_needs_gfx_fallback(1, 0, 0, 1) == 1);

    garnet_async_qorder_t q = { 0, 0 };
    garnet_async_submit_order_t o = garnet_async_qorder_next(&q, 0);
    CHECK("async qorder gfx1", o.wait_value == 0u && o.signal_value == 1u &&
          garnet_async_qorder_cross_wait(&q, 0) == 0u && garnet_async_qorder_cross_wait(&q, 1) == 1u);
    o = garnet_async_qorder_next(&q, 0);
    CHECK("async qorder gfx2", o.wait_value == 0u && o.signal_value == 2u && garnet_async_qorder_cross_wait(&q, 1) == 2u);
    o = garnet_async_qorder_next(&q, 1);
    CHECK("async qorder comp1", o.wait_value == 2u && o.signal_value == 3u &&
          garnet_async_qorder_cross_wait(&q, 1) == 0u && garnet_async_qorder_cross_wait(&q, 0) == 3u);
    o = garnet_async_qorder_next(&q, 1); CHECK("async qorder comp2", o.wait_value == 0u && o.signal_value == 4u);
    o = garnet_async_qorder_next(&q, 0); CHECK("async qorder gfx3", o.wait_value == 4u && o.signal_value == 5u);
    o = garnet_async_qorder_next(&q, 1); CHECK("async qorder comp3", o.wait_value == 5u && o.signal_value == 6u);

    garnet_async_qorder_t q2 = { 0, 0 };
    o = garnet_async_qorder_next(&q2, 1); CHECK("async q2 comp1", o.wait_value == 0u && o.signal_value == 1u);
    o = garnet_async_qorder_next(&q2, 1); CHECK("async q2 comp2", o.wait_value == 0u && o.signal_value == 2u);
    o = garnet_async_qorder_next(&q2, 0); CHECK("async q2 gfx1", o.wait_value == 2u && o.signal_value == 3u);

    CHECK("async concurrent buffer",
          garnet_async_compute_use_concurrent_buffer(0, 0, 1) == 0 &&
          garnet_async_compute_use_concurrent_buffer(1, 0, 0) == 0 &&
          garnet_async_compute_use_concurrent_buffer(1, 0, 1) == 1);
    CHECK("async concurrent image",
          garnet_async_compute_use_concurrent_image(0, 0, 1, 0) == 0 &&
          garnet_async_compute_use_concurrent_image(1, 0, 0, 0) == 0 &&
          garnet_async_compute_use_concurrent_image(1, 0, 1, 0) == 1 &&
          garnet_async_compute_use_concurrent_image(1, 0, 1, 1) == 0);

    const char *res_paths[2][2] = {
        { "src/garnet-metal/garnet_resources.m", "garnet_resources.m" },
        { "src/garnet-metal/garnet_textures.m", "garnet_textures.m" },
    };
    size_t imgCreates = 0, bufCreates = 0, concAssign = 0;
    int sawDrmSharing = 0;
    for (size_t fi = 0; fi < 2; fi++) {
        FILE *rf = fopen(res_paths[fi][0], "rb");
        if (!rf) rf = fopen(res_paths[fi][1], "rb");
        CHECK("async resources file readable", rf != NULL);
        if (rf) {
            fseek(rf, 0, SEEK_END);
            long sz = ftell(rf);
            fseek(rf, 0, SEEK_SET);
            char *buf = malloc((size_t)sz + 1);
            CHECK("async resources alloc", buf != NULL);
            if (buf) {
                CHECK("async resources read", fread(buf, 1, (size_t)sz, rf) == (size_t)sz);
                buf[sz] = 0;
                for (const char *p = buf; (p = strstr(p, "VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO")) != NULL; p += 35) imgCreates++;
                for (const char *p = buf; (p = strstr(p, "VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO")) != NULL; p += 36) bufCreates++;
                for (const char *p = buf; (p = strstr(p, "VK_SHARING_MODE_CONCURRENT")) != NULL; p += 26) concAssign++;
                if (strstr(buf, ".sharingMode = ic.sharingMode") != NULL) sawDrmSharing = 1;
                free(buf);
            }
            fclose(rf);
        }
    }
    CHECK("async resources concurrent count", imgCreates == 5u && bufCreates == 3u && concAssign == imgCreates + bufCreates);
    CHECK("async resources drm sharingMode", sawDrmSharing);
}

static rdna4cb_t g_cb_tab;
static uint32_t cb_sub(uint64_t id, uint64_t q, uint64_t now, uint32_t nr, const uint32_t *r,
                       rdna4cb_inv_t *inv, uint32_t mx, uint64_t *sq) {
    return rdna4cb_submit(&g_cb_tab, id, q, now, NULL, 0, r, nr, inv, mx, sq);
}

static void suite_cblog(void) {
    rdna4cb_inv_t inv[RDNA4CB_MAXINV]; uint64_t sq; uint32_t w1[] = { 5 }, r1[] = { 5 }, none[] = { 0 };
    rdna4cb_init(&g_cb_tab);
    rdna4cb_commit(&g_cb_tab, 1, 0xA1, 10, w1, 1, none, 0); rdna4cb_commit(&g_cb_tab, 2, 0xB2, 20, none, 0, r1, 1);
    uint32_t n = cb_sub(2, 0xB2, 30, 1, r1, inv, 32, &sq);
    CHECK("cblog B before A", n == 1 && inv[0].sid == 5 && inv[0].writer_id == 1 && inv[0].reader_id == 2 &&
          inv[0].writer_queue == 0xA1 && inv[0].reader_queue == 0xB2 && inv[0].reader_sub == 1 && sq == 1);
    n = cb_sub(1, 0xA1, 31, 0, none, inv, 32, &sq);
    CHECK("cblog A afterwards", n == 0 && sq == 2 && g_cb_tab.inv_total == 1 && g_cb_tab.inv_cbs == 1);
    rdna4cb_init(&g_cb_tab);
    rdna4cb_commit(&g_cb_tab, 1, 1, 10, w1, 1, none, 0); rdna4cb_commit(&g_cb_tab, 2, 2, 20, none, 0, r1, 1);
    cb_sub(1, 1, 25, 0, none, inv, 32, &sq); n = cb_sub(2, 2, 26, 1, r1, inv, 32, &sq);
    CHECK("cblog in order", n == 0 && g_cb_tab.inv_total == 0);
    rdna4cb_init(&g_cb_tab);
    rdna4cb_commit(&g_cb_tab, 2, 2, 20, none, 0, r1, 1); rdna4cb_commit(&g_cb_tab, 1, 1, 30, w1, 1, none, 0);
    CHECK("cblog later writer ignored", cb_sub(2, 2, 40, 1, r1, inv, 32, &sq) == 0);
    rdna4cb_init(&g_cb_tab);
    rdna4cb_commit(&g_cb_tab, 3, 1, 10, w1, 1, r1, 1);
    CHECK("cblog self read-write", cb_sub(3, 1, 11, 1, r1, inv, 32, &sq) == 0);
    rdna4cb_init(&g_cb_tab);
    uint32_t wa[] = { 7, 8 }, rr[] = { 7, 8 };
    rdna4cb_commit(&g_cb_tab, 1, 1, 1, wa, 2, none, 0);
    rdna4cb_commit(&g_cb_tab, 2, 2, 2, wa, 2, none, 0);
    rdna4cb_commit(&g_cb_tab, 9, 3, 5, none, 0, rr, 2);
    n = cb_sub(9, 3, 6, 2, rr, inv, 3, &sq);
    CHECK("cblog capped list exact count", n == 4 && inv[2].sid != 0 && g_cb_tab.inv_total == 4 && g_cb_tab.iv_inv == 4);
    uint64_t s_, i_; rdna4cb_take_interval(&g_cb_tab, &s_, &i_);
    CHECK("cblog interval", s_ == 1 && i_ == 4 && g_cb_tab.iv_submits == 0 && g_cb_tab.iv_inv == 0);

    rdna4cba_t activity; int idle;
    rdna4cba_init(&activity);
    rdna4cba_t interval = rdna4cba_take_interval(&activity, &idle);
    CHECK("cblog idle interval", interval.submits == 0 && interval.writebacks == 0 && interval.presents == 0 && interval.skips == 0 && idle);
    rdna4cba_submit(&activity); rdna4cba_writeback(&activity); rdna4cba_present(&activity, 0); rdna4cba_present(&activity, 1); rdna4cba_skip(&activity);
    interval = rdna4cba_take_interval(&activity, &idle);
    CHECK("cblog active interval", interval.submits == 1 && interval.writebacks == 1 && interval.presents == 1 && interval.present_calls == 2 && interval.skips == 1 && !idle);
    rdna4cba_result(&activity, 0);
    interval = rdna4cba_take_interval(&activity, &idle);
    CHECK("cblog failure-only active", interval.submits == 0 && interval.completed == 0 && interval.failed == 1 && !idle);
    rdna4cba_result(&activity, 1);
    interval = rdna4cba_take_interval(&activity, &idle);
    CHECK("cblog completion-only active", interval.submits == 0 && interval.completed == 1 && interval.failed == 0 && !idle);
    interval = rdna4cba_take_interval(&activity, &idle);
    CHECK("cblog terminal reset", interval.completed == 0 && interval.failed == 0 && idle);

    uint32_t a[4]; uint32_t c = 0;
    c = rdna4cb_addsid(a, c, 4, 3); c = rdna4cb_addsid(a, c, 4, 3); c = rdna4cb_addsid(a, c, 4, 0); c = rdna4cb_addsid(a, c, 4, 4);
    CHECK("cblog addsid dedupe", c == 2 && a[0] == 3 && a[1] == 4);
    c = rdna4cb_addsid(a, c, 4, 5); c = rdna4cb_addsid(a, c, 4, 6); c = rdna4cb_addsid(a, c, 4, 7);
    CHECK("cblog addsid cap", c == 4);

    rdna4cb_init(&g_cb_tab);
    rdna4cb_commit(&g_cb_tab, 1, 1, 1, w1, 1, none, 0); rdna4cb_commit(&g_cb_tab, 2, 2, 2, none, 0, r1, 1); rdna4cb_drop(&g_cb_tab, 1);
    CHECK("cblog drop", cb_sub(2, 2, 3, 1, r1, inv, 32, &sq) == 0 && g_cb_tab.drops == 1);
    rdna4cb_init(&g_cb_tab);
    rdna4cb_commit(&g_cb_tab, 1, 1, 1, w1, 1, none, 0);
    CHECK("cblog unregistered commit", cb_sub(2, 2, 3, 1, r1, inv, 32, &sq) == 1 && g_cb_tab.no_commit == 1);
    rdna4cb_init(&g_cb_tab);
    rdna4cb_commit(&g_cb_tab, 1000, 1, 1, w1, 1, none, 0);
    for (int i = 0; i < RDNA4CB_TAB; i++) rdna4cb_commit(&g_cb_tab, 1 + i, 1, 100 + i, none, 0, none, 0);
    CHECK("cblog evicted oldest", g_cb_tab.evicted == 1 && rdna4cb_find(&g_cb_tab, 1000) == NULL);
    rdna4cb_init(&g_cb_tab);
    rdna4cb_commit(&g_cb_tab, 1, 1, 1, w1, 1, none, 0); rdna4cb_commit(&g_cb_tab, 1, 1, 2, none, 0, none, 0); rdna4cb_commit(&g_cb_tab, 2, 2, 3, none, 0, r1, 1);
    CHECK("cblog re-commit replaces", cb_sub(2, 2, 4, 1, r1, inv, 32, &sq) == 0);

    srand(12345); int bad = 0;
    for (int round = 0; round < 2000 && !bad; round++) {
        rdna4cb_init(&g_cb_tab); struct { int live; uint64_t c; uint32_t w[3], r[3]; } m[24]; memset(m, 0, sizeof m);
        int cnt = 2 + rand() % 22;
        for (int i = 0; i < cnt; i++) {
            uint32_t wl[3], rl[3], nw = rand() % 3, nr = rand() % 3;
            for (uint32_t k = 0; k < nw; k++) wl[k] = 1 + rand() % 5;
            for (uint32_t k = 0; k < nr; k++) rl[k] = 1 + rand() % 5;
            uint32_t dw[3], dr[3], cw = 0, cr = 0;
            for (uint32_t k = 0; k < nw; k++) cw = rdna4cb_addsid(dw, cw, 3, wl[k]);
            for (uint32_t k = 0; k < nr; k++) cr = rdna4cb_addsid(dr, cr, 3, rl[k]);
            m[i].live = 1; m[i].c = 100 + i; memcpy(m[i].w, dw, sizeof dw); memcpy(m[i].r, dr, sizeof dr);
            m[i].w[2] = cw; m[i].r[2] = cr;
            if (cw > 2) cw = m[i].w[2] = 2;
            if (cr > 2) cr = m[i].r[2] = 2;
            rdna4cb_commit(&g_cb_tab, 1 + i, 1, 100 + i, dw, cw, dr, cr);
        }
        for (int step = 0; step < cnt; step++) {
            int k; do { k = rand() % cnt; } while (!m[k].live);
            uint32_t exp = 0;
            for (uint32_t j = 0; j < m[k].r[2]; j++)
                for (int e = 0; e < cnt; e++)
                    if (e != k && m[e].live && m[e].c < m[k].c)
                        for (uint32_t q = 0; q < m[e].w[2]; q++)
                            if (m[e].w[q] == m[k].r[j]) exp++;
            uint32_t got = rdna4cb_submit(&g_cb_tab, 1 + k, 1, 1000 + step, NULL, 0, m[k].r, m[k].r[2], NULL, 0, &sq);
            m[k].live = 0;
            if (got != exp) bad = 1;
        }
    }
    CHECK("cblog model check 2000 rounds", !bad);
}

static void dc_mk(rdna4dc_sig *s, uint64_t layout, uint64_t dsl, uint64_t buf, uint64_t off, uint64_t tex, uint64_t view, uint64_t smp) {
    rdna4dc_sig_begin(s, layout, dsl); rdna4dc_sig_push(s, buf); rdna4dc_sig_push(s, off);
    rdna4dc_sig_push(s, tex); rdna4dc_sig_push(s, view); rdna4dc_sig_push(s, smp);
}

static rdna4dc_rpkey dc_rk(uint32_t na, uint32_t fetch) {
    rdna4dc_rpkey k; memset(&k, 0, sizeof k); k.na = na; k.fetch = fetch;
    for (uint32_t i = 0; i < na; i++) k.a[i] = (rdna4dc_att){ 44, 1, 2, 0, 2, 1, 0, 2 };
    return k;
}

static void suite_drawcache(void) {
    rdna4dc_enc e; rdna4dc_sig a, b; uint64_t set;
    rdna4dc_enc_init(&e);
    dc_mk(&a, 1, 10, 0xB0, 0, 0xE0, 0xF0, 0xA0);
    CHECK("dc empty stage", !rdna4dc_stage_match(&e.st[0], &a, &set));
    rdna4dc_stage_store(&e.st[0], &a, 0x5E7);
    dc_mk(&b, 1, 10, 0xB0, 0, 0xE0, 0xF0, 0xA0); CHECK("dc identical bindings", rdna4dc_stage_match(&e.st[0], &b, &set) && set == 0x5E7);
    dc_mk(&b, 1, 10, 0xB0, 256, 0xE0, 0xF0, 0xA0); CHECK("dc offset change", !rdna4dc_stage_match(&e.st[0], &b, &set));
    dc_mk(&b, 1, 11, 0xB0, 0, 0xE0, 0xF0, 0xA0); CHECK("dc dsl change", !rdna4dc_stage_match(&e.st[0], &b, &set));
    dc_mk(&b, 2, 10, 0xB0, 0, 0xE0, 0xF0, 0xA0); CHECK("dc layout change", !rdna4dc_stage_match(&e.st[0], &b, &set));
    dc_mk(&b, 1, 10, 0xB0, 0, 0xE1, 0xF0, 0xA0); CHECK("dc tex change", !rdna4dc_stage_match(&e.st[0], &b, &set));
    dc_mk(&b, 1, 10, 0xB0, 0, 0xE0, 0xF1, 0xA0); CHECK("dc view change", !rdna4dc_stage_match(&e.st[0], &b, &set));
    dc_mk(&b, 1, 10, 0xB0, 0, 0xE0, 0xF0, 0xA1); CHECK("dc sampler change", !rdna4dc_stage_match(&e.st[0], &b, &set));
    dc_mk(&b, 1, 10, 0xB1, 0, 0xE0, 0xF0, 0xA0); CHECK("dc buffer change", !rdna4dc_stage_match(&e.st[0], &b, &set));
    rdna4dc_sig_begin(&b, 1, 10); rdna4dc_sig_push(&b, 0xB0);
    CHECK("dc shorter sig", !rdna4dc_stage_match(&e.st[0], &b, &set));
    CHECK("dc independent stage", !rdna4dc_stage_match(&e.st[1], &a, &set));
    { rdna4dc_sig big; rdna4dc_sig_begin(&big, 1, 10); for (int i = 0; i < RDNA4DC_MAXW + 5; i++) rdna4dc_sig_push(&big, 7);
      rdna4dc_stage_store(&e.st[1], &big, 0x99);
      CHECK("dc over-long sig", big.over && !e.st[1].valid && !rdna4dc_stage_match(&e.st[1], &big, &set)); }

    dc_mk(&a, 1, 10, 0xB0, 0, 0xE0, 0xF0, 0xA0);
    rdna4dc_enc_init(&e); rdna4dc_enc_layout(&e, 1); rdna4dc_stage_store(&e.st[0], &a, 0x5E7);
    CHECK("dc same layout keeps set", (rdna4dc_enc_layout(&e, 1), rdna4dc_stage_match(&e.st[0], &a, &set)));
    rdna4dc_enc_layout(&e, 2);
    CHECK("dc new layout drops sets", !e.st[0].valid && !e.st[1].valid);
    rdna4dc_stage_store(&e.st[0], &a, 0x5E7); e.lastPipe = 5;
    rdna4dc_enc_reset_bound(&e);
    CHECK("dc reset bound", !e.st[0].valid && e.lastPipe == 0 && e.layout == 0);
    { rdna4dc_enc f; rdna4dc_enc_init(&f); CHECK("dc fresh enc", !f.st[0].valid && !f.st[1].valid && f.lastPipe == 0 && !f.pcValid); }

    rdna4dc_enc_init(&e);
    CHECK("dc first pipe bind", !rdna4dc_enc_pipe_same(&e, 0x77));
    CHECK("dc same pipe skip", rdna4dc_enc_pipe_same(&e, 0x77));
    CHECK("dc other pipe bind", !rdna4dc_enc_pipe_same(&e, 0x78));
    CHECK("dc back to first bind", !rdna4dc_enc_pipe_same(&e, 0x77));
    rdna4dc_enc_reset_bound(&e);
    CHECK("dc after reset bind", !rdna4dc_enc_pipe_same(&e, 0x77));
    CHECK("dc pipe counters", e.pipesSkipped == 1 && e.pipesBound == 4);

    { uint64_t p; int dummy1 = 1, dummy2 = 2;
      rdna4dc_enc_init(&e);
      CHECK("dc pc miss", !rdna4dc_enc_pc_get(&e, 3, 0, 1, &dummy1, &p));
      rdna4dc_enc_pc_put(&e, 3, 0, 1, &dummy1, 0xAA);
      CHECK("dc pc hit", rdna4dc_enc_pc_get(&e, 3, 0, 1, &dummy1, &p) && p == 0xAA);
      CHECK("dc pc diffs", !rdna4dc_enc_pc_get(&e, 4, 0, 1, &dummy1, &p) &&
            !rdna4dc_enc_pc_get(&e, 3, 1, 1, &dummy1, &p) &&
            !rdna4dc_enc_pc_get(&e, 3, 0, 0, &dummy1, &p) &&
            !rdna4dc_enc_pc_get(&e, 3, 0, 1, &dummy2, &p));
      rdna4dc_enc_pc_put(&e, 3, 0, 1, &dummy1, 0);
      CHECK("dc pc null not cached", !rdna4dc_enc_pc_get(&e, 3, 0, 1, &dummy1, &p));
      int psos[10]; rdna4dc_enc_init(&e);
      for (int i = 0; i < (int)RDNA4DC_PC_CAP; i++) rdna4dc_enc_pc_put(&e, 3, 0, 1, &psos[i], 0x1000u + (uint64_t)i);
      int allHit = 1;
      for (int i = 0; i < (int)RDNA4DC_PC_CAP; i++)
          if (!rdna4dc_enc_pc_get(&e, 3, 0, 1, &psos[i], &p) || p != 0x1000u + (uint64_t)i) allHit = 0;
      CHECK("dc pc 8-slot all hit", allHit && e.pcValid == (int)RDNA4DC_PC_CAP);
      rdna4dc_enc_pc_put(&e, 3, 0, 1, &psos[8], 0x2000u);
      CHECK("dc pc 9th evicts slot 0",
            !rdna4dc_enc_pc_get(&e, 3, 0, 1, &psos[0], &p) &&
            rdna4dc_enc_pc_get(&e, 3, 0, 1, &psos[8], &p) && p == 0x2000u &&
            rdna4dc_enc_pc_get(&e, 3, 0, 1, &psos[7], &p) && p == 0x1007u); }

    { rdna4dc_cbcache cb; rdna4dc_enc e1, e2; rdna4dc_sig sa, sb, slong; uint64_t s, p; int pso1 = 1, pso2 = 2;
      rdna4dc_cb_init(&cb); rdna4dc_enc_init(&e1); rdna4dc_enc_init(&e2);
      rdna4dc_cb_pc_put(&cb, 3, 0, 1, &pso1, 0xA1); rdna4dc_cb_pc_put(&cb, 3, 0, 1, &pso2, 0xA2);
      CHECK("dc cb pc cross-enc",
            rdna4dc_cb_pc_get(&cb, 3, 0, 1, &pso1, &p) && p == 0xA1 &&
            rdna4dc_cb_pc_get(&cb, 3, 0, 1, &pso2, &p) && p == 0xA2);
      rdna4dc_cb_pc_put(&cb, 3, 0, 1, &pso1, 0);
      CHECK("dc cb pc null invalidate",
            !rdna4dc_cb_pc_get(&cb, 3, 0, 1, &pso1, &p) && rdna4dc_cb_pc_get(&cb, 3, 0, 1, &pso2, &p) && p == 0xA2);
      dc_mk(&sa, 1, 10, 0xB0, 0, 0xE0, 0xF0, 0xA0); dc_mk(&sb, 2, 20, 0xB1, 64, 0xE1, 0xF1, 0xA1);
      rdna4dc_enc_layout(&e1, 1); rdna4dc_stage_store(&e1.st[0], &sa, 0x501); rdna4dc_cb_lru_put(&cb, 0, &sa, 0x501);
      rdna4dc_enc_layout(&e1, 2); rdna4dc_stage_store(&e1.st[0], &sb, 0x502); rdna4dc_cb_lru_put(&cb, 0, &sb, 0x502);
      CHECK("dc cb lru cross-enc",
            !rdna4dc_stage_match(&e2.st[0], &sa, &s) && rdna4dc_cb_lru_find(&cb, 0, &sa, &s) && s == 0x501 &&
            rdna4dc_cb_lru_find(&cb, 0, &sb, &s) && s == 0x502);
      CHECK("dc cb lru stage indep", !rdna4dc_cb_lru_find(&cb, 1, &sa, &s));
      rdna4dc_cb_reset_segment(&cb);
      CHECK("dc cb reset segment",
            !rdna4dc_cb_lru_find(&cb, 0, &sa, &s) && !rdna4dc_cb_lru_find(&cb, 0, &sb, &s) &&
            rdna4dc_cb_pc_get(&cb, 3, 0, 1, &pso2, &p) && p == 0xA2);
      for (uint32_t i = 0; i <= RDNA4DC_LRU_CAP; i++) { dc_mk(&sa, 1, 10, 0xB0 + i, 0, 0xE0, 0xF0, 0xA0); rdna4dc_cb_lru_put(&cb, 0, &sa, 0x700u + i); }
      dc_mk(&sa, 1, 10, 0xB0, 0, 0xE0, 0xF0, 0xA0); dc_mk(&sb, 1, 10, 0xB0 + RDNA4DC_LRU_CAP, 0, 0xE0, 0xF0, 0xA0);
      CHECK("dc cb lru eviction", !rdna4dc_cb_lru_find(&cb, 0, &sa, &s) && rdna4dc_cb_lru_find(&cb, 0, &sb, &s) && s == 0x700u + RDNA4DC_LRU_CAP);
      rdna4dc_sig_begin(&slong, 1, 10);
      for (uint32_t i = 0; i < RDNA4DC_LRU_MAXW; i++) rdna4dc_sig_push(&slong, 0x100u + i);
      rdna4dc_cb_lru_put(&cb, 0, &slong, 0x999);
      CHECK("dc cb lru long sig skipped", !rdna4dc_cb_lru_find(&cb, 0, &slong, &s)); }

    { rdna4dc_rpcache *c = (rdna4dc_rpcache *)calloc(1, sizeof *c); rdna4dc_rpkey k = dc_rk(2, 0), k2;
      assert(c);
      CHECK("dc rp miss", rdna4dc_rp_find(c, &k) == 0);
      CHECK("dc rp add", rdna4dc_rp_add(c, &k, 0x100) && c->created == 1);
      k2 = dc_rk(2, 0); CHECK("dc rp hit", rdna4dc_rp_find(c, &k2) == 0x100 && c->hits == 1);
      k2 = dc_rk(2, 1); CHECK("dc rp fetch diff", rdna4dc_rp_find(c, &k2) == 0);
      k2 = dc_rk(3, 0); CHECK("dc rp na diff", rdna4dc_rp_find(c, &k2) == 0);
      k2 = dc_rk(2, 0); k2.a[1].format++; CHECK("dc rp fmt diff", rdna4dc_rp_find(c, &k2) == 0);
      k2 = dc_rk(2, 0); k2.a[0].samples++; CHECK("dc rp samples diff", rdna4dc_rp_find(c, &k2) == 0);
      k2 = dc_rk(2, 0); k2.a[1].loadOp++; CHECK("dc rp loadOp diff", rdna4dc_rp_find(c, &k2) == 0);
      k2 = dc_rk(2, 0); k2.a[0].storeOp++; CHECK("dc rp storeOp diff", rdna4dc_rp_find(c, &k2) == 0);
      k2 = dc_rk(2, 0); k2.a[0].stencilLoadOp++; CHECK("dc rp stLoad diff", rdna4dc_rp_find(c, &k2) == 0);
      k2 = dc_rk(2, 0); k2.a[0].stencilStoreOp++; CHECK("dc rp stStore diff", rdna4dc_rp_find(c, &k2) == 0);
      k2 = dc_rk(2, 0); k2.a[1].initialLayout++; CHECK("dc rp initLayout diff", rdna4dc_rp_find(c, &k2) == 0);
      k2 = dc_rk(2, 0); k2.a[1].finalLayout++; CHECK("dc rp finalLayout diff", rdna4dc_rp_find(c, &k2) == 0);
      k2 = dc_rk(2, 0); k2.a[5].format = 99; CHECK("dc rp tail ignored", rdna4dc_rp_find(c, &k2) == 0x100);
      for (uint32_t i = 1; i < RDNA4DC_RP_CAP; i++) { k2 = dc_rk(1, 0); k2.a[0].format = 1000 + i; rdna4dc_rp_add(c, &k2, 0x200 + i); }
      k2 = dc_rk(1, 1); CHECK("dc rp cap", c->n == RDNA4DC_RP_CAP && !rdna4dc_rp_add(c, &k2, 0x999) && c->uncached == 1);
      k2 = dc_rk(1, 0); k2.a[0].format = 1000 + 7; CHECK("dc rp full lookup", rdna4dc_rp_find(c, &k2) == 0x207);
      free(c); }

    CHECK("dc stage skip allowed",
          rdna4dc_stage_can_skip(1, GARNET_DRAW_DIRTY_FSTAGE, GARNET_DRAW_DIRTY_VSTAGE, 0) == 1 &&
          rdna4dc_stage_can_skip(1, GARNET_DRAW_DIRTY_VSTAGE, GARNET_DRAW_DIRTY_FSTAGE, 0) == 1);
    CHECK("dc stage skip blocked",
          rdna4dc_stage_can_skip(0, 0u, GARNET_DRAW_DIRTY_VSTAGE, 0) == 0 &&
          rdna4dc_stage_can_skip(1, GARNET_DRAW_DIRTY_VSTAGE, GARNET_DRAW_DIRTY_VSTAGE, 0) == 0 &&
          rdna4dc_stage_can_skip(1, GARNET_DRAW_DIRTY_PASS, GARNET_DRAW_DIRTY_VSTAGE, 0) == 0 &&
          rdna4dc_stage_can_skip(1, 0u, GARNET_DRAW_DIRTY_VSTAGE, 1) == 0);
    CHECK("dc clean draw ready",
          rdna4dc_clean_draw_ready(1, 0u, 0u, 1, 1, 0, 0, 0, 1) == 1 &&
          rdna4dc_clean_draw_ready(0, 0u, 0u, 1, 1, 0, 0, 0, 1) == 0 &&
          rdna4dc_clean_draw_ready(1, GARNET_DRAW_DIRTY_PIPE, 0u, 1, 1, 0, 0, 0, 1) == 0 &&
          rdna4dc_clean_draw_ready(1, 0u, 1u, 1, 1, 0, 0, 0, 1) == 0 &&
          rdna4dc_clean_draw_ready(1, 0u, 0u, 0, 1, 0, 0, 0, 1) == 0 &&
          rdna4dc_clean_draw_ready(1, 0u, 0u, 1, 0, 0, 0, 0, 1) == 0 &&
          rdna4dc_clean_draw_ready(1, 0u, 0u, 1, 1, 1, 0, 0, 1) == 0 &&
          rdna4dc_clean_draw_ready(1, 0u, 0u, 1, 1, 0, 1, 0, 1) == 0 &&
          rdna4dc_clean_draw_ready(1, 0u, 0u, 1, 1, 0, 0, 1, 1) == 0 &&
          rdna4dc_clean_draw_ready(1, 0u, 0u, 1, 1, 0, 0, 0, 0) == 0);
    CHECK("dc tex walk gated",
          rdna4dc_stage_needs_tex(GARNET_DRAW_DIRTY_FSTAGE, GARNET_DRAW_DIRTY_FTEX, GARNET_DRAW_DIRTY_FSTAGE, 0) == 0 &&
          rdna4dc_stage_needs_tex(GARNET_DRAW_DIRTY_FSTAGE, GARNET_DRAW_DIRTY_FTEX, GARNET_DRAW_DIRTY_FSTAGE, 1) == 1 &&
          rdna4dc_stage_needs_tex(GARNET_DRAW_DIRTY_FTEX, GARNET_DRAW_DIRTY_FTEX, GARNET_DRAW_DIRTY_FSTAGE, 0) == 1 &&
          rdna4dc_stage_needs_tex(GARNET_DRAW_DIRTY_PASS, GARNET_DRAW_DIRTY_VTEX, GARNET_DRAW_DIRTY_VSTAGE, 0) == 1);
}

static void suite_queue_pipe(void) {
    CHECK("qp eligible simple", garnet_qp_eligible(0, 1, 0, 0, 1, 0) == 1);
    CHECK("qp ineligible matrix",
          garnet_qp_eligible(1, 1, 0, 0, 1, 0) == 0 &&
          garnet_qp_eligible(0, 2, 0, 0, 1, 0) == 0 &&
          garnet_qp_eligible(0, 0, 0, 0, 1, 0) == 0 &&
          garnet_qp_eligible(0, 1, 1, 0, 1, 0) == 0 &&
          garnet_qp_eligible(0, 1, 0, 1, 1, 0) == 0 &&
          garnet_qp_eligible(0, 1, 0, 0, 0, 0) == 0 &&
          garnet_qp_eligible(0, 1, 0, 0, 1, 1) == 0);

    garnet_qp_t qp; garnet_qp_init(&qp, GARNET_QP_DEFAULT_MAX_INFLIGHT);
    CHECK("qp init default 4", qp.max_inflight == 4u && qp.inflight_count == 0u);
    for (unsigned i = 0; i < 4; i++) { CHECK("qp launch up to 4", garnet_qp_can_launch(&qp, 1) == 1); garnet_qp_on_launch(&qp, 1); }
    CHECK("qp 5th blocked", garnet_qp_can_launch(&qp, 1) == 0 && garnet_qp_can_launch(&qp, 0) == 0);
    CHECK("qp stats", qp.max_observed_depth == 4u && qp.pipelined_submits == 3u);
    garnet_qp_on_complete(&qp, 0, 0); garnet_qp_on_complete(&qp, 0, 0); garnet_qp_on_complete(&qp, 0, 0);
    CHECK("qp 1 in flight", qp.inflight_count == 1u && garnet_qp_can_launch(&qp, 1) == 1 && garnet_qp_can_launch(&qp, 0) == 0);
    garnet_qp_on_complete(&qp, 0, 0);
    CHECK("qp 0 in flight barrier ok", qp.inflight_count == 0u && garnet_qp_can_launch(&qp, 0) == 1);
    garnet_qp_on_launch(&qp, 0);
    CHECK("qp barrier active", qp.barrier_active == 1 && garnet_qp_can_launch(&qp, 1) == 0 && garnet_qp_can_launch(&qp, 0) == 0);
    garnet_qp_on_complete(&qp, 1, 0);
    CHECK("qp barrier done", qp.barrier_active == 0 && qp.inflight_count == 0u && garnet_qp_can_launch(&qp, 1) == 1);

    garnet_qp_t sq; garnet_qp_init(&sq, GARNET_QP_SERIAL_MAX_INFLIGHT);
    CHECK("qp serial", garnet_qp_can_launch(&sq, 1) == 1);
    garnet_qp_on_launch(&sq, 1);
    CHECK("qp serial blocks 2nd", garnet_qp_can_launch(&sq, 1) == 0);
    garnet_qp_on_complete(&sq, 0, 0);
    CHECK("qp serial reopens", garnet_qp_can_launch(&sq, 1) == 1);

    garnet_qp_t fq; garnet_qp_init(&fq, GARNET_QP_DEFAULT_MAX_INFLIGHT);
    garnet_qp_on_launch(&fq, 1); garnet_qp_on_launch(&fq, 1);
    garnet_qp_on_complete(&fq, 0, 1);
    CHECK("qp faulted blocks", fq.faulted == 1 && garnet_qp_can_launch(&fq, 1) == 0);
    garnet_qp_on_complete(&fq, 0, 0);
    CHECK("qp faulted drained", fq.inflight_count == 0u && garnet_qp_can_launch(&fq, 1) == 1);

    CHECK("qp elide dcopy",
          garnet_qp_should_elide_dcopy(1, 0, 0, 1, 0) == 1 &&
          garnet_qp_should_elide_dcopy(1, 2, 0, 0, 1) == 1 &&
          garnet_qp_should_elide_dcopy(1, 1, 0, 0, 0) == 0 &&
          garnet_qp_should_elide_dcopy(0, 1, 0, 1, 1) == 0 &&
          garnet_qp_should_elide_dcopy(1, -1, 0, 1, 1) == 0 &&
          garnet_qp_should_elide_dcopy(1, 1, 1, 1, 1) == 0);

    srand(42);
    garnet_qp_t sim; garnet_qp_init(&sim, GARNET_QP_DEFAULT_MAX_INFLIGHT);
    unsigned next_submit = 0, next_deliver = 0, inflight_ids[4] = {0}, inflight_n = 0;
    int inflight_ready[4] = {0}, order_ok = 1;
    while (next_deliver < 1000) {
        while (next_submit < 1000 && garnet_qp_can_launch(&sim, 1)) {
            garnet_qp_on_launch(&sim, 1);
            inflight_ids[inflight_n] = next_submit++;
            inflight_ready[inflight_n] = 0;
            inflight_n++;
        }
        if (inflight_n > 0) {
            unsigned pick = (unsigned)(rand() % (int)inflight_n);
            inflight_ready[pick] = 1;
            while (inflight_n > 0 && inflight_ready[0]) {
                if (inflight_ids[0] != next_deliver) order_ok = 0;
                next_deliver++;
                garnet_qp_on_complete(&sim, 0, 0);
                for (unsigned k = 1; k < inflight_n; k++) {
                    inflight_ids[k - 1] = inflight_ids[k];
                    inflight_ready[k - 1] = inflight_ready[k];
                }
                inflight_n--;
            }
        }
    }
    CHECK("qp 1000 FIFO completions", order_ok && next_deliver == 1000 && sim.inflight_count == 0u && sim.max_observed_depth == 4u);

    CHECK("qp final defer default", garnet_qp_final_defer_ns(1, 1, 1, 0, 1, 0, 0, 0, 0, 1000000ULL, 1050000ULL) == 200000ULL);
    CHECK("qp final defer adaptive",
          garnet_qp_final_defer_ns(1, 1, 1, 0, 1, 0, 0, 0, 60000ULL, 1000000ULL, 1010000ULL) == 200000ULL &&
          garnet_qp_final_defer_ns(1, 1, 1, 0, 1, 0, 0, 0, 400000ULL, 1000000ULL, 1050000ULL) == 500000ULL &&
          garnet_qp_final_defer_ns(1, 1, 1, 0, 1, 0, 0, 0, 2000000ULL, 1000000ULL, 1100000ULL) == 700000ULL);
    CHECK("qp final defer elapsed", garnet_qp_final_defer_ns(1, 1, 1, 0, 1, 0, 0, 0, 60000ULL, 1000000ULL, 1210000ULL) == 0ULL);
    CHECK("qp final defer zero cases",
          garnet_qp_final_defer_ns(0, 1, 1, 0, 1, 0, 0, 0, 60000ULL, 1000000ULL, 1010000ULL) == 0ULL &&
          garnet_qp_final_defer_ns(1, 0, 1, 0, 1, 0, 0, 0, 60000ULL, 1000000ULL, 1010000ULL) == 0ULL &&
          garnet_qp_final_defer_ns(1, 1, 0, 0, 1, 0, 0, 0, 60000ULL, 1000000ULL, 1010000ULL) == 0ULL &&
          garnet_qp_final_defer_ns(1, 1, 1, 1, 1, 0, 0, 0, 60000ULL, 1000000ULL, 1010000ULL) == 0ULL &&
          garnet_qp_final_defer_ns(1, 1, 1, 0, 0, 0, 0, 0, 60000ULL, 1000000ULL, 1010000ULL) == 0ULL &&
          garnet_qp_final_defer_ns(1, 1, 1, 0, 1, 1, 0, 0, 60000ULL, 1000000ULL, 1010000ULL) == 0ULL &&
          garnet_qp_final_defer_ns(1, 1, 1, 0, 1, 0, 1, 0, 60000ULL, 1000000ULL, 1010000ULL) == 0ULL &&
          garnet_qp_final_defer_ns(1, 1, 1, 0, 1, 0, 0, 1, 60000ULL, 1000000ULL, 1010000ULL) == 0ULL);

    CHECK("qp ws wait policy",
          garnet_qp_ws_needs_completion_wait(0, 0, 1, 1, 0, 1) == 0 &&
          garnet_qp_ws_needs_completion_wait(1, 1, 1, 1, 0, 1) == 0 &&
          garnet_qp_ws_needs_completion_wait(1, 0, 0, 1, 0, 0) == 1 &&
          garnet_qp_ws_needs_completion_wait(1, 0, 0, 0, 0, 0) == 0 &&
          garnet_qp_ws_needs_completion_wait(1, 0, 0, 1, 1, 0) == 0 &&
          garnet_qp_ws_needs_completion_wait(1, 0, 0, 1, 1, 1) == 1 &&
          garnet_qp_ws_needs_completion_wait(1, 0, 1, 0, 1, 0) == 1);

    CHECK("qp comp barrier policy",
          garnet_comp_norm_dispatch_type(0) == GARNET_COMP_DISPATCH_SERIAL &&
          garnet_comp_norm_dispatch_type(1) == GARNET_COMP_DISPATCH_CONCURRENT &&
          garnet_comp_norm_dispatch_type(2) == GARNET_COMP_DISPATCH_SERIAL &&
          garnet_comp_norm_dispatch_type(99) == GARNET_COMP_DISPATCH_SERIAL &&
          garnet_comp_needs_pre_barrier(GARNET_COMP_DISPATCH_SERIAL, 0, 0) == 0 &&
          garnet_comp_needs_pre_barrier(GARNET_COMP_DISPATCH_SERIAL, 1, 0) == 1 &&
          garnet_comp_needs_pre_barrier(GARNET_COMP_DISPATCH_SERIAL, 1, 1) == 0 &&
          garnet_comp_needs_pre_barrier(GARNET_COMP_DISPATCH_CONCURRENT, 1, 0) == 0 &&
          garnet_comp_needs_pre_barrier(GARNET_COMP_DISPATCH_CONCURRENT, 0, 0) == 0 &&
          garnet_comp_scope_needs_full_barrier(0) == 0 &&
          garnet_comp_scope_needs_full_barrier(1) == 0 &&
          garnet_comp_scope_needs_full_barrier(2) == 0 &&
          garnet_comp_scope_needs_full_barrier(3) == 0 &&
          garnet_comp_scope_needs_full_barrier(4) == 1 &&
          garnet_comp_scope_needs_full_barrier(5) == 1 &&
          garnet_comp_scope_needs_full_barrier(8) == 1);
}

#define HS_SEC 1000000000LL
static rdna4hs_key_obs hs_mk(int spv, uint64_t sz, int64_t mt, int meta) {
    rdna4hs_key_obs k = { { spv, sz, mt }, { meta, meta ? 100 : 0, meta ? mt : 0 } };
    return k;
}
static _Atomic uintptr_t g_hs_slot;
static _Atomic int g_hs_winners, g_hs_bad;
static void *hs_racer(void *arg) {
    uintptr_t v = (uintptr_t)arg;
    if (rdna4hs_install(&g_hs_slot, v)) atomic_fetch_add(&g_hs_winners, 1);
    return NULL;
}
static void *hs_reader(void *arg) {
    (void)arg; uintptr_t seen = 0;
    for (int i = 0; i < 2000000; i++) {
        uintptr_t v = rdna4hs_load(&g_hs_slot);
        if (v && !seen) seen = v;
        if (seen && v != seen) atomic_fetch_add(&g_hs_bad, 1);
        if (!v && seen) atomic_fetch_add(&g_hs_bad, 1);
    }
    return NULL;
}

static void suite_hotswap(void) {
    { rdna4hs_entry e; rdna4hs_init(&e, 2, 1);
      rdna4hs_key_obs none[2] = { hs_mk(0, 0, 0, 0), hs_mk(0, 0, 0, 0) };
      rdna4hs_key_obs one[2] = { hs_mk(1, 400, 10 * HS_SEC, 1), hs_mk(0, 0, 0, 0) };
      rdna4hs_key_obs both[2] = { hs_mk(1, 400, 10 * HS_SEC, 1), hs_mk(1, 800, 13 * HS_SEC, 1) };
      CHECK("hs basic wait",
            rdna4hs_tick(&e, 10 * HS_SEC, none) == RDNA4HS_NONE &&
            rdna4hs_tick(&e, 11 * HS_SEC, one) == RDNA4HS_NONE &&
            rdna4hs_tick(&e, 12 * HS_SEC, one) == RDNA4HS_NONE &&
            rdna4hs_tick(&e, 13 * HS_SEC + 100, both) == RDNA4HS_NONE &&
            rdna4hs_tick(&e, 13 * HS_SEC + 10000000, both) == RDNA4HS_NONE);
      CHECK("hs basic build+swap",
            rdna4hs_tick(&e, 14 * HS_SEC, both) == RDNA4HS_BUILD && e.st == RDNA4HS_BUILDING && e.attempts == 1 &&
            rdna4hs_tick(&e, 15 * HS_SEC, both) == RDNA4HS_NONE &&
            rdna4hs_build_done(&e, 1, 15 * HS_SEC) == 1 && e.st == RDNA4HS_SWAPPED &&
            rdna4hs_tick(&e, 16 * HS_SEC, both) == RDNA4HS_NONE &&
            rdna4hs_build_done(&e, 1, 16 * HS_SEC) == 0); }

    { const int64_t registered = 20 * HS_SEC, published = registered + 101000000LL;
      rdna4hs_entry entry; rdna4hs_init(&entry, 2, 1);
      rdna4hs_key_obs modules[2] = { hs_mk(1, 2008, 19 * HS_SEC, 1), hs_mk(1, 16944, published, 1) };
      int64_t eligible = 0;
      for (int64_t time = registered + RDNA4HS_POLL_NS; time <= registered + 2 * HS_SEC; time += RDNA4HS_POLL_NS) {
          rdna4hs_key_obs current[2] = { modules[0], time >= published ? modules[1] : hs_mk(0, 0, 0, 0) };
          if (rdna4hs_tick(&entry, time, current) == RDNA4HS_BUILD) { eligible = time; break; }
      }
      CHECK("hs cold pair poll delay", eligible - published >= RDNA4HS_MIN_AGE_NS && eligible - published <= 45000000LL); }

    { rdna4hs_entry e; rdna4hs_init(&e, 1, 1);
      rdna4hs_key_obs a = hs_mk(1, 400, 10 * HS_SEC, 1), b = hs_mk(1, 800, 11 * HS_SEC, 1), odd = hs_mk(1, 402, 10 * HS_SEC, 1), tiny = hs_mk(1, 16, 10 * HS_SEC, 1);
      CHECK("hs partial/odd/tiny",
            rdna4hs_tick(&e, 20 * HS_SEC, &a) == RDNA4HS_NONE &&
            rdna4hs_tick(&e, 21 * HS_SEC, &b) == RDNA4HS_NONE &&
            rdna4hs_tick(&e, 22 * HS_SEC, &b) == RDNA4HS_BUILD);
      rdna4hs_entry f; rdna4hs_init(&f, 1, 1);
      CHECK("hs malformed sizes",
            rdna4hs_tick(&f, 20 * HS_SEC, &odd) == RDNA4HS_NONE && rdna4hs_tick(&f, 21 * HS_SEC, &odd) == RDNA4HS_NONE && f.st == RDNA4HS_WAIT &&
            rdna4hs_tick(&f, 22 * HS_SEC, &tiny) == RDNA4HS_NONE && rdna4hs_tick(&f, 23 * HS_SEC, &tiny) == RDNA4HS_NONE); }

    { rdna4hs_entry c; rdna4hs_init(&c, 1, 1);
      rdna4hs_key_obs nm = hs_mk(1, 400, 10 * HS_SEC, 0), wm = hs_mk(1, 400, 10 * HS_SEC, 1);
      int ok = 1; for (int i = 0; i < 20; i++) if (rdna4hs_tick(&c, (20 + i) * HS_SEC, &nm) != RDNA4HS_NONE) ok = 0;
      wm.meta.mtime_ns = 10 * HS_SEC;
      CHECK("hs compute meta required", ok && rdna4hs_tick(&c, 50 * HS_SEC, &wm) == RDNA4HS_NONE && rdna4hs_tick(&c, 51 * HS_SEC, &wm) == RDNA4HS_BUILD);
      rdna4hs_entry r; rdna4hs_init(&r, 1, 0);
      CHECK("hs render meta grace",
            rdna4hs_tick(&r, 11 * HS_SEC, &nm) == RDNA4HS_NONE && rdna4hs_tick(&r, 12 * HS_SEC, &nm) == RDNA4HS_NONE &&
            rdna4hs_tick(&r, 14 * HS_SEC, &nm) == RDNA4HS_NONE && rdna4hs_tick(&r, 16 * HS_SEC, &nm) == RDNA4HS_BUILD); }

    { rdna4hs_entry e; rdna4hs_init(&e, 1, 1); rdna4hs_key_obs a = hs_mk(1, 400, 10 * HS_SEC, 1);
      int64_t t = 20 * HS_SEC; rdna4hs_tick(&e, t, &a);
      CHECK("hs backoff",
            rdna4hs_tick(&e, t += HS_SEC, &a) == RDNA4HS_BUILD &&
            rdna4hs_build_done(&e, 0, t) == 0 && e.st == RDNA4HS_WAIT && e.attempts == 1 && e.next_try_ns == t + 2 * HS_SEC &&
            rdna4hs_tick(&e, t += HS_SEC, &a) == RDNA4HS_NONE &&
            rdna4hs_tick(&e, t += HS_SEC, &a) == RDNA4HS_BUILD && rdna4hs_build_done(&e, 0, t) == 0);
      for (int i = 0; i < 20 && e.st == RDNA4HS_WAIT; i++)
          if (rdna4hs_tick(&e, t += HS_SEC, &a) == RDNA4HS_BUILD) rdna4hs_build_done(&e, 0, t);
      CHECK("hs gaveup", e.st == RDNA4HS_GAVEUP && e.attempts == RDNA4HS_MAX_ATTEMPTS && rdna4hs_tick(&e, t += 100 * HS_SEC, &a) == RDNA4HS_NONE);
      rdna4hs_entry g; rdna4hs_init(&g, 1, 1); int64_t u = 20 * HS_SEC; rdna4hs_tick(&g, u, &a);
      CHECK("hs file change resets budget",
            rdna4hs_tick(&g, u += HS_SEC, &a) == RDNA4HS_BUILD && (rdna4hs_build_done(&g, 0, u), g.attempts == 1));
      rdna4hs_key_obs b = hs_mk(1, 800, 30 * HS_SEC, 1);
      rdna4hs_tick(&g, u += 5 * HS_SEC, &b);
      CHECK("hs budget reset", g.attempts == 0 && g.next_try_ns == 0); }

    { rdna4hs_entry e; rdna4hs_init(&e, 1, 1); rdna4hs_key_obs a = hs_mk(1, 400, 10 * HS_SEC, 1);
      int g1 = rdna4hs_acquire(&e), g2 = rdna4hs_acquire(&e);
      CHECK("hs acquire fallback", g1 == 0 && g2 == 0 && e.users[0] == 2 && rdna4hs_destroy_ok(&e) == 0);
      rdna4hs_tick(&e, 20 * HS_SEC, &a);
      CHECK("hs swap in-flight", rdna4hs_tick(&e, 21 * HS_SEC, &a) == RDNA4HS_BUILD && rdna4hs_build_done(&e, 1, 21 * HS_SEC) == 1 && rdna4hs_destroy_ok(&e) == 0);
      int g3 = rdna4hs_acquire(&e);
      CHECK("hs new gen", g3 == 1 && e.users[0] == 2 && e.users[1] == 1);
      rdna4hs_release(&e, g1); CHECK("hs still busy", rdna4hs_destroy_ok(&e) == 0);
      rdna4hs_release(&e, g2); CHECK("hs destroy once", e.users[0] == 0 && rdna4hs_destroy_ok(&e) == 1 && rdna4hs_destroy_ok(&e) == 0);
      rdna4hs_release(&e, g3); rdna4hs_release(&e, g3); CHECK("hs over-release clamp", e.users[1] == 0);
      rdna4hs_release(&e, 7); rdna4hs_release(&e, -1);
      rdna4hs_entry f; rdna4hs_init(&f, 1, 1); rdna4hs_tick(&f, 20 * HS_SEC, &a); rdna4hs_tick(&f, 21 * HS_SEC, &a); rdna4hs_build_done(&f, 1, 21 * HS_SEC);
      CHECK("hs empty fallback destroys immediately", rdna4hs_destroy_ok(&f) == 1);
      rdna4hs_entry h; rdna4hs_init(&h, 1, 1); rdna4hs_tick(&h, 20 * HS_SEC, &a); rdna4hs_tick(&h, 21 * HS_SEC, &a); rdna4hs_build_done(&h, 0, 21 * HS_SEC);
      CHECK("hs failed build keeps fallback", rdna4hs_destroy_ok(&h) == 0); }

    { int race_ok = 1;
      for (int round = 0; round < 50; round++) {
          atomic_store(&g_hs_slot, 0); atomic_store(&g_hs_winners, 0); atomic_store(&g_hs_bad, 0);
          pthread_t r[4], w[4];
          for (int i = 0; i < 4; i++) pthread_create(&r[i], NULL, hs_reader, NULL);
          for (int i = 0; i < 4; i++) pthread_create(&w[i], NULL, hs_racer, (void *)(uintptr_t)(0x1000 + 0x10 * (i + 1)));
          for (int i = 0; i < 4; i++) { pthread_join(w[i], NULL); pthread_join(r[i], NULL); }
          uintptr_t val = rdna4hs_load(&g_hs_slot);
          if (atomic_load(&g_hs_winners) != 1 || atomic_load(&g_hs_bad) != 0 || val < 0x1010 || val > 0x1040) race_ok = 0;
      }
      CHECK("hs publish race 50 rounds", race_ok); }
}

static void suite_devcaps(void) {
    const uint64_t MIB = UINT64_C(1) << 20, GIB = UINT64_C(1) << 30;
    rdna4dc_caps fallback = rdna4dc_fallback();
    CHECK("devcaps pre-open identity", !strstr(fallback.name, "9070") && !strstr(fallback.name, "9060"));
    CHECK("devcaps pre-open buffer limit", fallback.buffer_length == 128 * MIB && fallback.working_set == 256 * MIB);
    CHECK("devcaps pre-open compute", fallback.workgroup_size[0] == 128 && fallback.workgroup_size[1] == 128 &&
          fallback.workgroup_size[2] == 64 && fallback.invocations == 128 && fallback.shared_memory == 16384 &&
          rdna4dc_workgroup_ok(&fallback, 128, 1, 1) && !rdna4dc_workgroup_ok(&fallback, 128, 2, 1));

    rdna4dc_native_memory native = { .version = RDNA4DC_MEMORY_CAPS_VERSION,
        .physical = 16 * GIB, .pool = 15 * GIB, .visible = 217 * MIB,
        .application_budget = 14 * GIB - 64 * MIB, .max_buffer = UINT64_C(0xfffffffc) };
    rdna4dc_caps cold = rdna4dc_from_native_memory(&native, 0);
    CHECK("devcaps cold native",
          cold.buffer_length == UINT64_C(0xfffffffc) && cold.allocation_budget == 14 * GIB - 64 * MIB &&
          cold.working_set == 12 * GIB && rdna4dc_heap_available(&cold, 12 * GIB) == 2 * GIB - 64 * MIB &&
          rdna4dc_from_native_memory(&native, 1).allocation_budget == 15 * GIB &&
          cold.invocations == fallback.invocations && cold.shared_memory == fallback.shared_memory);
    const rdna4dc_native_memory malformed[] = {
        {0}, {2, 16 * GIB, 15 * GIB, 217 * MIB, 14 * GIB, GIB, 0},
        {1, GIB, 2 * GIB, MIB, GIB, GIB, 0}, {1, GIB, GIB, 2 * GIB, GIB, GIB, 0},
        {1, GIB, GIB, MIB, 2 * GIB, GIB, 0}, {1, GIB, GIB, MIB, GIB, 0, 0},
        {1, GIB, GIB, MIB, GIB, 2 * GIB, 0}, {1, UINT64_MAX, 0, 0, 0, GIB, 0}
    };
    for (size_t i = 0; i < sizeof malformed / sizeof malformed[0]; i++)
        CHECK("devcaps malformed native", !rdna4dc_native_memory_valid(&malformed[i]) &&
              rdna4dc_from_native_memory(&malformed[i], 0).buffer_length == fallback.buffer_length);

    native.application_budget = 0; cold = rdna4dc_from_native_memory(&native, 0);
    CHECK("devcaps exhausted native", cold.buffer_length == native.max_buffer && !cold.allocation_budget && !cold.working_set);
    native.max_buffer = 8 * GIB; native.application_budget = 2 * GIB; cold = rdna4dc_from_native_memory(&native, 0);
    CHECK("devcaps independent logical length", cold.buffer_length == 8 * GIB && cold.allocation_budget == 2 * GIB);
    native.max_buffer = UINT64_C(0xfffffffc); native.composite_max_buffer = 8 * GIB; cold = rdna4dc_from_native_memory(&native, 0);
    CHECK("devcaps composite discovery", cold.buffer_length == 8 * GIB && native.max_buffer == UINT64_C(0xfffffffc));
    native.application_budget = 0;
    CHECK("devcaps composite zero budget", rdna4dc_from_native_memory(&native, 0).buffer_length == 8 * GIB && !rdna4dc_from_native_memory(&native, 0).allocation_budget);
    const uint64_t invalid_composite[] = {UINT64_MAX, 16 * GIB, 9 * GIB, GIB};
    for (size_t i = 0; i < sizeof invalid_composite / sizeof invalid_composite[0]; i++) {
        native.composite_max_buffer = invalid_composite[i];
        CHECK("devcaps invalid composite", rdna4dc_from_native_memory(&native, 0).buffer_length == native.max_buffer);
    }
    native.composite_max_buffer = 0; native.max_buffer = 8 * GIB; native.application_budget = 2 * GIB;
    cold = rdna4dc_from_native_memory(&native, 0);

    rdna4dc_heap heaps[] = {{32 * GIB, 0}, {8 * GIB, 1}, {256 * MIB, 1}};
    char name[] = "AMD Radeon RX 9060 XT (RADV GFX1200)";
    rdna4dc_properties p = { .name = name, .storage_buffer_range = UINT32_MAX,
        .allocation_size = 8 * GIB, .heaps = heaps, .heap_count = 3,
        .workgroup_size = {1024, 1024, 1024}, .invocations = 1024, .shared_memory = 65536, .subgroup_size = 64 };
    rdna4dc_caps c = rdna4dc_from_properties(&p);
    CHECK("devcaps name copy", !strcmp(c.name, name));
    name[0] = 'X'; CHECK("devcaps owned name", c.name[0] == 'A');
    CHECK("devcaps heaps & buffer bounds",
          c.working_set == 8 * GIB + 256 * MIB && c.allocation_budget == 8 * GIB + 256 * MIB &&
          c.buffer_length == UINT32_MAX && rdna4dc_buffer_ok(&c, UINT32_MAX) &&
          !rdna4dc_buffer_ok(&c, UINT64_C(1) << 32) && !rdna4dc_buffer_ok(&c, 0));
    CHECK("devcaps lds/subgroup/workgroup",
          c.shared_memory == 32768 && c.subgroup_size == 64 &&
          rdna4dc_thread_execution_width(&fallback) == 32 && rdna4dc_thread_execution_width(&c) == 32 &&
          rdna4dc_thread_execution_width(&(rdna4dc_caps){ .subgroup_size = 32 }) == 32 &&
          rdna4dc_thread_execution_width(&(rdna4dc_caps){ .subgroup_size = 16 }) == 16 &&
          rdna4dc_workgroup_ok(&c, 32, 32, 1) && !rdna4dc_workgroup_ok(&c, 1, 0, 1) &&
          !rdna4dc_workgroup_ok(&c, 1025, 1, 1) && !rdna4dc_workgroup_ok(&c, 32, 32, 2));

    p.allocation_size = 64 * MIB; c = rdna4dc_from_properties(&p);
    CHECK("devcaps m3 cap", c.buffer_length == 64 * MIB && rdna4dc_buffer_ok(&c, 64 * MIB) && !rdna4dc_buffer_ok(&c, 64 * MIB + 1));
    p.allocation_size = 0; p.storage_buffer_range = 16 * MIB; c = rdna4dc_from_properties(&p);
    CHECK("devcaps storage range cap", c.buffer_length == 16 * MIB);
    p.storage_buffer_range = UINT64_MAX; heaps[1].size = 16 * GIB; c = rdna4dc_from_properties(&p);
    CHECK("devcaps 12G working set", c.working_set == 12 * GIB && c.allocation_budget == 16 * GIB + 256 * MIB && c.buffer_length == UINT32_MAX);
    p.budget_valid = 1; p.allocation_budget = 9 * GIB; c = rdna4dc_from_properties(&p);
    CHECK("devcaps budget clamp", c.allocation_budget == 9 * GIB && c.working_set == 9 * GIB && heaps[1].size == 16 * GIB);
    p.allocation_budget = UINT64_MAX; CHECK("devcaps oversized budget", rdna4dc_from_properties(&p).allocation_budget == 16 * GIB + 256 * MIB);
    p.allocation_budget = 0; c = rdna4dc_from_properties(&p); CHECK("devcaps zero budget", c.allocation_budget == 0 && c.working_set == 0);
    p.budget_valid = 0;

    p.logical_buffer_size = 8 * GIB; p.allocation_size = 8 * GIB; p.storage_buffer_range = 128 * MIB;
    CHECK("devcaps needs bda64", rdna4dc_from_properties(&p).buffer_length == 128 * MIB);
    p.addressing64 = 1; c = rdna4dc_from_properties(&p);
    CHECK("devcaps bda64 8G", c.buffer_length == 8 * GIB && rdna4dc_buffer_ok(&c, 8 * GIB));
    p.budget_valid = 1; p.allocation_budget = 2 * GIB; c = rdna4dc_from_properties(&p);
    CHECK("devcaps matches cold", c.buffer_length == cold.buffer_length && c.allocation_budget == cold.allocation_budget);
    p.budget_valid = 0; p.logical_buffer_size = 6 * GIB; CHECK("devcaps m4 cap", rdna4dc_from_properties(&p).buffer_length == 6 * GIB);
    p.allocation_size = 5 * GIB; CHECK("devcaps m3+m4 cap", rdna4dc_from_properties(&p).buffer_length == 5 * GIB);
    p.logical_buffer_size = 0; CHECK("devcaps missing m4", rdna4dc_from_properties(&p).buffer_length == 128 * MIB);
    p.addressing64 = 0; p.logical_buffer_size = 0; p.allocation_size = 0; p.storage_buffer_range = UINT64_MAX;

    heaps[1].size = 128 * MIB; heaps[2].size = 0;
    p.workgroup_size[0] = 512; p.workgroup_size[1] = 128; p.workgroup_size[2] = 64;
    p.invocations = 256; p.shared_memory = 8192; p.subgroup_size = 32;
    c = rdna4dc_from_properties(&p);
    CHECK("devcaps small heap & heap rounding",
          c.buffer_length == 128 * MIB && c.working_set == 128 * MIB &&
          c.workgroup_size[0] == 256 && c.shared_memory == 8192 && c.subgroup_size == 32 &&
          rdna4dc_heap_available(&c, 127 * MIB) == MIB && rdna4dc_heap_available(&c, 128 * MIB) == 0 &&
          rdna4dc_heap_available(&c, fallback.working_set) == 0 && rdna4dc_heap_available(&c, UINT64_MAX) == 0 &&
          rdna4dc_heap_size(128 * MIB, c.allocation_budget) == 128 * MIB &&
          rdna4dc_heap_size(128 * MIB - 1, c.allocation_budget) == 128 * MIB &&
          rdna4dc_heap_size(128 * MIB + 1, c.allocation_budget) == 0 &&
          !rdna4dc_heap_size(UINT64_MAX, UINT64_MAX) && !rdna4dc_heap_size(UINT64_MAX - RDNA4DC_HEAP_PAGE + 2, UINT64_MAX) &&
          !rdna4dc_heap_size(0, UINT64_MAX) && rdna4dc_heap_size(1, RDNA4DC_HEAP_PAGE) == RDNA4DC_HEAP_PAGE);

    p.workgroup_size[0] = p.workgroup_size[1] = p.workgroup_size[2] = UINT32_MAX; p.invocations = UINT32_MAX;
    c = rdna4dc_from_properties(&p);
    CHECK("devcaps metal ceiling & overflow",
          c.invocations == 1024 && c.workgroup_size[0] == 1024 &&
          !rdna4dc_workgroup_ok(&c, UINT64_MAX, UINT64_MAX, UINT64_MAX) && !rdna4dc_workgroup_ok(&c, 1024, 1024, 1024));

    heaps[0].device_local = heaps[1].device_local = heaps[2].device_local = 0; c = rdna4dc_from_properties(&p);
    CHECK("devcaps missing local", c.buffer_length == 0 && c.working_set == 0 && !rdna4dc_buffer_ok(&c, 1));
    p.heaps = NULL; p.heap_count = 0; p.invocations = 0; p.shared_memory = 0; c = rdna4dc_from_properties(&p);
    CHECK("devcaps fail closed", !rdna4dc_workgroup_ok(&c, 1, 1, 1) && !rdna4dc_heap_size(1, c.working_set) && c.shared_memory == 0);
    char long_name[300]; memset(long_name, 'Q', sizeof long_name); long_name[299] = 0;
    p.name = long_name; c = rdna4dc_from_properties(&p); CHECK("devcaps long name", strlen(c.name) == 255 && c.name[255] == 0);
    p.name = ""; c = rdna4dc_from_properties(&p); CHECK("devcaps empty name", !strcmp(c.name, fallback.name));

    unsigned violations = 0;
    for (uint32_t inv = 1; inv <= 1024; inv *= 2) {
        p.invocations = inv; p.workgroup_size[0] = 127; p.workgroup_size[1] = 63; p.workgroup_size[2] = 31;
        c = rdna4dc_from_properties(&p);
        for (uint64_t x = 1; x <= 128; x++)
            for (uint64_t y = 1; y <= 64; y++)
                for (uint64_t z = 1; z <= 32; z++)
                    if (rdna4dc_workgroup_ok(&c, x, y, z) != (x <= 127 && y <= 63 && z <= 31 && x * y * z <= inv)) violations++;
    }
    CHECK("devcaps 2.88M workgroup sweep", violations == 0);

    { rdna4dc_metal3_caps zero = rdna4dc_metal3_fallback();
      CHECK("devcaps m3 fallback off",
            memcmp(&fallback.metal3, &zero, sizeof zero) == 0 && memcmp(&cold.metal3, &zero, sizeof zero) == 0 &&
            !rdna4dc_metal3_barycentric_ok(&fallback.metal3) && !rdna4dc_metal3_pull_model_ok(&fallback.metal3) &&
            !rdna4dc_metal3_atomic_float_ok(&fallback.metal3) && !rdna4dc_metal3_atomic_int64_ok(&fallback.metal3) &&
            !rdna4dc_metal3_mesh_supported(&fallback.metal3) && !rdna4dc_metal3_mesh_payload_ok(&fallback.metal3, 0) &&
            !rdna4dc_metal3_mesh_grid_ok(&fallback.metal3, 1, 1, 1) && !rdna4dc_metal3_object_grid_ok(&fallback.metal3, 1, 1, 1) &&
            !rdna4dc_metal3_raytracing_supported(&fallback.metal3) && !rdna4dc_metal3_timestamps_supported(&fallback.metal3) &&
            !rdna4dc_metal3_sparse_buffer_ok(&fallback.metal3) && !rdna4dc_metal3_sparse_texture_ok(&fallback.metal3) &&
            !rdna4dc_metal3_mesh_supported(NULL) && !rdna4dc_metal3_raytracing_supported(NULL) &&
            !rdna4dc_metal3_timestamps_supported(NULL) && !rdna4dc_metal3_sparse_texture_ok(NULL) &&
            memcmp(&c.metal3, &zero, sizeof zero) == 0);

      rdna4dc_metal3_probe mp = {
          .api_1_2 = 1, .shader_int64 = 1, .buffer_device_address = 1, .sample_rate_shading = 1,
          .ext_barycentric = 1, .feat_barycentric = 1,
          .ext_atomic_float = 1, .feat_atomic_float_buffer = 1, .feat_atomic_float_buffer_add = 1,
          .feat_atomic_float_shared = 1, .feat_atomic_float_shared_add = 1,
          .feat_atomic_float_image = 1, .feat_atomic_float_image_add = 1,
          .ext_atomic_float2 = 1, .feat_atomic_float_buffer_minmax = 1, .feat_atomic_float_shared_minmax = 1,
          .ext_atomic_int64 = 1, .feat_atomic_int64_buffer = 1, .feat_atomic_int64_shared = 1,
          .ext_mesh_shader = 1, .feat_task_shader = 1, .feat_mesh_shader = 1,
          .mesh_max_payload_bytes = 65536u,
          .object_max_workgroup_count = { 65535u, 65535u, 65535u },
          .object_max_workgroup_total_count = 4194304u, .object_max_workgroup_invocations = 2048u,
          .mesh_max_workgroup_count = { 65535u, 65535u, 65535u },
          .mesh_max_workgroup_total_count = 4194304u, .mesh_max_workgroup_invocations = 2048u,
          .mesh_max_output_vertices = 512u, .mesh_max_output_primitives = 1024u,
          .ext_deferred_host_ops = 1, .ext_accel_struct = 1, .feat_accel_struct = 1,
          .feat_accel_indirect_build = 1, .feat_accel_uab = 1,
          .rt_min_scratch_alignment = 128u, .rt_max_geometries = 1u << 24,
          .rt_max_instances = 1u << 24, .rt_max_primitives = 1u << 29,
          .ext_ray_query = 1, .feat_ray_query = 1,
          .ext_ray_tracing_position_fetch = 1, .feat_ray_tracing_position_fetch = 1,
          .timestamp_query_funcs = 1, .timestamp_compute_and_graphics = 1,
          .timestamp_period_ns = 10.0f, .timestamp_valid_bits = 64u,
          .sparse_queue = 1, .sparse_bind_func = 1,
          .sparse_binding = 1, .sparse_residency_buffer = 1,
          .sparse_residency_image_2d = 1, .sparse_residency_image_3d = 1,
          .sparse_residency_2_samples = 1, .sparse_residency_4_samples = 1,
          .sparse_residency_8_samples = 1, .sparse_residency_aliased = 1
      };
      p.metal3 = &mp;
      rdna4dc_caps m3c = rdna4dc_from_properties(&p);
      const rdna4dc_metal3_caps *m = &m3c.metal3;
      CHECK("devcaps m3 probed on",
            rdna4dc_metal3_barycentric_ok(m) && rdna4dc_metal3_pull_model_ok(m) &&
            rdna4dc_metal3_atomic_float_ok(m) && m->atomic_float_image && m->atomic_float_minmax &&
            rdna4dc_metal3_atomic_int64_ok(m) && rdna4dc_metal3_mesh_supported(m) &&
            rdna4dc_metal3_raytracing_supported(m) && m->accel_indirect_build && m->accel_uab &&
            m->rt_min_scratch_alignment == 128u && rdna4dc_metal3_timestamps_supported(m) &&
            m->timestamp_compute_and_graphics && m->timestamp_valid_bits == 64u &&
            m->timestamp_period_ns == 10.0f && rdna4dc_metal3_sparse_buffer_ok(m) &&
            rdna4dc_metal3_sparse_texture_ok(m) && m->sparse_residency_image_3d && m->sparse_residency_aliased &&
            m->ray_tracing_position_fetch && rdna4dc_metal3_rt_position_fetch_supported(m));
      CHECK("devcaps m3 mesh clamp & edges",
            m->mesh_max_payload_bytes == RDNA4DC_MESH_MAX_PAYLOAD_BYTES &&
            m->object_max_workgroup_invocations == RDNA4DC_MESH_MAX_INVOCATIONS &&
            m->mesh_max_workgroup_invocations == RDNA4DC_MESH_MAX_INVOCATIONS &&
            m->mesh_max_output_vertices == RDNA4DC_MESH_MAX_VERTICES &&
            m->mesh_max_output_primitives == RDNA4DC_MESH_MAX_PRIMITIVES &&
            rdna4dc_metal3_mesh_payload_ok(m, 0) &&
            rdna4dc_metal3_mesh_payload_ok(m, RDNA4DC_MESH_MAX_PAYLOAD_BYTES) &&
            !rdna4dc_metal3_mesh_payload_ok(m, RDNA4DC_MESH_MAX_PAYLOAD_BYTES + 1u) &&
            rdna4dc_metal3_mesh_grid_ok(m, 1024, 1, 1) && rdna4dc_metal3_object_grid_ok(m, 64, 64, 1024) &&
            !rdna4dc_metal3_mesh_grid_ok(m, 0, 1, 1) && !rdna4dc_metal3_mesh_grid_ok(m, 65536, 1, 1) &&
            !rdna4dc_metal3_mesh_grid_ok(m, UINT64_MAX, UINT64_MAX, UINT64_MAX));

      rdna4dc_metal3_probe sub = mp; sub.mesh_max_payload_bytes = 8192u; rdna4dc_metal3_caps mc = rdna4dc_metal3_from_probe(&sub);
      CHECK("devcaps m3 undersized payload", mc.mesh_max_payload_bytes == 8192u && !rdna4dc_metal3_mesh_supported(&mc) &&
            rdna4dc_metal3_mesh_payload_ok(&mc, 8192u) && !rdna4dc_metal3_mesh_payload_ok(&mc, 8193u));
      sub = mp; sub.mesh_max_workgroup_total_count = 512u; mc = rdna4dc_metal3_from_probe(&sub);
      CHECK("devcaps m3 mesh axis clamp", mc.mesh_max_workgroup_count[0] == 512u && !rdna4dc_metal3_mesh_supported(&mc) &&
            rdna4dc_metal3_mesh_grid_ok(&mc, 512, 1, 1) && !rdna4dc_metal3_mesh_grid_ok(&mc, 513, 1, 1));
      sub = mp; sub.feat_task_shader = 0; mc = rdna4dc_metal3_from_probe(&sub);
      CHECK("devcaps m3 missing taskShader", mc.mesh_shader && !mc.object_shader && !mc.mesh_max_payload_bytes &&
            !mc.object_max_workgroup_total_count && !rdna4dc_metal3_mesh_supported(&mc) && rdna4dc_metal3_mesh_grid_ok(&mc, 1024, 1, 1));
      sub = mp; sub.ext_mesh_shader = 0; mc = rdna4dc_metal3_from_probe(&sub);
      CHECK("devcaps m3 missing mesh ext", !mc.mesh_shader && !mc.object_shader && !mc.mesh_max_payload_bytes && !mc.mesh_max_output_vertices);

      sub = mp; sub.ext_deferred_host_ops = 0; mc = rdna4dc_metal3_from_probe(&sub);
      CHECK("devcaps m3 rt no deferred_host", !mc.deferred_host_ops && !mc.acceleration_structure && !mc.ray_query && !rdna4dc_metal3_raytracing_supported(&mc));
      sub = mp; sub.buffer_device_address = 0; mc = rdna4dc_metal3_from_probe(&sub);
      CHECK("devcaps m3 rt no bda", !mc.acceleration_structure && !mc.ray_query && !rdna4dc_metal3_raytracing_supported(&mc));
      sub = mp; sub.feat_accel_struct = 0; mc = rdna4dc_metal3_from_probe(&sub);
      CHECK("devcaps m3 rt no accel", !mc.acceleration_structure && !mc.ray_query && !mc.ray_tracing_position_fetch);
      sub = mp; sub.feat_ray_query = 0; mc = rdna4dc_metal3_from_probe(&sub); CHECK("devcaps m3 pos_fetch no rq", !rdna4dc_metal3_rt_position_fetch_supported(&mc));
      sub = mp; sub.ext_ray_tracing_position_fetch = 0; mc = rdna4dc_metal3_from_probe(&sub); CHECK("devcaps m3 pos_fetch no ext", !rdna4dc_metal3_rt_position_fetch_supported(&mc));
      sub = mp; sub.feat_ray_tracing_position_fetch = 0; mc = rdna4dc_metal3_from_probe(&sub); CHECK("devcaps m3 pos_fetch no feat", !rdna4dc_metal3_rt_position_fetch_supported(&mc));

      sub = mp; sub.ext_barycentric = 0; mc = rdna4dc_metal3_from_probe(&sub); CHECK("devcaps m3 no bary", !rdna4dc_metal3_barycentric_ok(&mc));
      sub = mp; sub.feat_atomic_float_buffer_add = 0; mc = rdna4dc_metal3_from_probe(&sub); CHECK("devcaps m3 no atomic_f", !mc.atomic_float_buffer && !rdna4dc_metal3_atomic_float_ok(&mc));
      sub = mp; sub.ext_atomic_float = 0; mc = rdna4dc_metal3_from_probe(&sub); CHECK("devcaps m3 no atomic_f2", !mc.atomic_float_minmax && !rdna4dc_metal3_atomic_float_ok(&mc));
      sub = mp; sub.shader_int64 = 0; mc = rdna4dc_metal3_from_probe(&sub); CHECK("devcaps m3 no int64", !rdna4dc_metal3_atomic_int64_ok(&mc));

      const float bad_periods[] = { 0.0f, -1.0f, 1e9f, (float)NAN, (float)INFINITY };
      int bad_ts_ok = 1;
      for (size_t i = 0; i < sizeof bad_periods / sizeof bad_periods[0]; i++) {
          sub = mp; sub.timestamp_period_ns = bad_periods[i]; mc = rdna4dc_metal3_from_probe(&sub);
          if (rdna4dc_metal3_timestamps_supported(&mc) || mc.timestamp_valid_bits) bad_ts_ok = 0;
      }
      sub = mp; sub.timestamp_valid_bits = 0u; mc = rdna4dc_metal3_from_probe(&sub); bad_ts_ok &= !rdna4dc_metal3_timestamps_supported(&mc);
      sub = mp; sub.timestamp_valid_bits = 65u; mc = rdna4dc_metal3_from_probe(&sub); bad_ts_ok &= !rdna4dc_metal3_timestamps_supported(&mc);
      sub = mp; sub.timestamp_query_funcs = 0; mc = rdna4dc_metal3_from_probe(&sub); bad_ts_ok &= !rdna4dc_metal3_timestamps_supported(&mc);
      CHECK("devcaps m3 bad timestamps fail closed", bad_ts_ok);

      sub = mp; sub.sparse_binding = 0; mc = rdna4dc_metal3_from_probe(&sub);
      CHECK("devcaps m3 sparse no binding", !mc.sparse_binding && !mc.sparse_residency_buffer && !mc.sparse_residency_image_2d && !mc.sparse_residency_image_3d && !mc.sparse_residency_aliased);
      sub = mp; sub.sparse_queue = 0; mc = rdna4dc_metal3_from_probe(&sub); CHECK("devcaps m3 sparse no queue", !rdna4dc_metal3_sparse_texture_ok(&mc));
      sub = mp; sub.sparse_bind_func = 0; mc = rdna4dc_metal3_from_probe(&sub); CHECK("devcaps m3 sparse no func", !rdna4dc_metal3_sparse_buffer_ok(&mc));
      p.metal3 = NULL; }
}

int main(void) {
    suite_query_caps();
    suite_t1();
    suite_async_compute();
    suite_cblog();
    suite_drawcache();
    suite_queue_pipe();
    suite_hotswap();
    suite_devcaps();
    printf("test-host-runtime: %s (%u checks, %u failed)\n", g_fails ? "FAIL" : "PASS", g_checks, g_fails);
    return g_fails != 0;
}
