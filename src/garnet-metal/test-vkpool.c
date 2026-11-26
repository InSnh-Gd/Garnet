// Host ASan+UBSan unit test for garnet_vkpool.h (Stage 2B).
#include "garnet_vkpool.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fails = 0;

static void check(int cond, const char *msg) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", msg);
        g_fails++;
    }
}

static void test_lifo_and_bounds(void) {
    garnet_vkpool_t p;
    garnet_vkpool_init(&p, 4u);
    check(garnet_vkpool_pop(&p) == 0u, "empty pop returns 0");
    check(p.misses == 1u && p.hits == 0u, "empty pop counts as miss");

    check(garnet_vkpool_push(&p, 0u) == 0, "null handle rejected");
    check(garnet_vkpool_push(&p, 0x1001u) == 1, "push 1");
    check(garnet_vkpool_push(&p, 0x1002u) == 1, "push 2");
    check(garnet_vkpool_push(&p, 0x1003u) == 1, "push 3");
    check(garnet_vkpool_push(&p, 0x1004u) == 1, "push 4");
    check(garnet_vkpool_push(&p, 0x1005u) == 0, "push beyond cap rejected");
    check(p.count == 4u && p.returns == 4u && p.drops == 1u, "push/drop counters exact");

    check(garnet_vkpool_pop(&p) == 0x1004u, "LIFO order #1");
    check(garnet_vkpool_pop(&p) == 0x1003u, "LIFO order #2");
    check(p.count == 2u && p.hits == 2u, "hit counter exact");

    uint64_t drained[8] = {0};
    uint32_t n = garnet_vkpool_drain(&p, drained, 8u);
    check(n == 2u && drained[0] == 0x1002u && drained[1] == 0x1001u, "drain returns remaining in LIFO order");
    check(p.count == 0u && garnet_vkpool_pop(&p) == 0u, "empty after drain");
}

static void test_cap_clamping(void) {
    garnet_vkpool_t p;
    garnet_vkpool_init(&p, 9999u);
    check(p.cap == GARNET_VKPOOL_FENCE_CAP, "oversized cap clamped to GARNET_VKPOOL_FENCE_CAP");
    for (uint32_t i = 1u; i <= GARNET_VKPOOL_FENCE_CAP; i++) {
        check(garnet_vkpool_push(&p, 0x2000u + i) == 1, "fill to clamped cap");
    }
    check(garnet_vkpool_push(&p, 0x9999u) == 0, "overflow after clamped cap rejected");
    uint32_t n = garnet_vkpool_drain(&p, NULL, GARNET_VKPOOL_FENCE_CAP);
    check(n == GARNET_VKPOOL_FENCE_CAP && p.count == 0u, "drain with NULL out clears pool");
}

static void test_stage2_command_invariants(void) {
    check(GARNET_DYN_ALL == (GARNET_DYN_VIEWPORT | GARNET_DYN_SCISSOR | GARNET_DYN_BLEND |
                             GARNET_DYN_STENCIL_M | GARNET_DYN_STENCIL_R | GARNET_DYN_DEPTHBIAS |
                             GARNET_DYN_RAST | GARNET_DYN_TOPO | GARNET_DYN_DS_OP),
          "GARNET_DYN_ALL covers all dynamic-state dirty bits");
    FILE *f = fopen("garnet_commands.m", "rb");
    check(f != NULL, "open garnet_commands.m");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)len + 1u);
    check(buf != NULL, "alloc garnet_commands.m buffer");
    if (buf) {
        size_t nr = fread(buf, 1, (size_t)len, f);
        buf[nr] = '\0';
        const char *bind = strstr(buf, "if (!rdna4dc_enc_pipe_same(&sc->dc, (uint64_t)(uintptr_t)vkp))");
        const char *dirty = bind ? strstr(bind, "_dynDirty = GARNET_DYN_ALL;") : NULL;
        check(bind != NULL && dirty != NULL && (dirty - bind) < 200,
              "pipeline switch re-dirties all dynamic state (prevents vpCount/depthBias/mesh stale leaks)");
        const char *pd = strstr(buf, "- (void)rdna4PoolDone {");
        const char *pser_zero = pd ? strstr(pd, "rdna4_fence_close(_pser); _pser = 0;") : NULL;
        check(pd != NULL && pser_zero != NULL && (pser_zero - pd) < 160,
              "rdna4PoolDone clears _pser so dealloc does not double-close under RDNA4P.mu");
        free(buf);
    }
    fclose(f);
}

int main(void) {
    test_lifo_and_bounds();
    test_cap_clamping();
    test_stage2_command_invariants();
    if (g_fails) {
        fprintf(stderr, "test-vkpool: %d failure(s)\n", g_fails);
        return 1;
    }
    printf("test-vkpool: ALL PASS\n");
    return 0;
}
