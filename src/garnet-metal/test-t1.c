// test-t1.c: host test of rdna4_t1.h.  cc -O1 -Wall -Wextra -Werror -o /tmp/test-t1 test-t1.c && /tmp/test-t1
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include "rdna4_t1.h"
static int fails;
#define CHECK(name, cond, ...) do { int ok_ = (cond); printf("%s %s: ", ok_ ? "ok  " : "FAIL", name); printf(__VA_ARGS__); printf("\n"); if (!ok_) fails++; } while (0)
static rdna4t_hist H;
static void *worker(void *a) { (void)a; for (int i = 0; i < 100000; i++) rdna4t_add(&H, 1000 + (uint64_t)(i % 7)); return NULL; }
int main(void) {
    CHECK("bucket edges", rdna4t_bucket(0) == 0 && rdna4t_bucket(99999) == 0 && rdna4t_bucket(100000) == 1 && rdna4t_bucket(399999) == 1 && rdna4t_bucket(400000) == 2 &&
          rdna4t_bucket(409599999ULL) == 6 && rdna4t_bucket(409600000ULL) == 7 && rdna4t_bucket(~0ULL) == 7, "0.1ms x4 steps, last bucket open");
    rdna4t_snap s; rdna4t_take(&H, &s); CHECK("empty", s.n == 0 && s.min == 0 && s.max == 0, "n=%llu", (unsigned long long)s.n);
    char buf[512]; CHECK("empty fmt", rdna4t_fmt("x", &s, buf, sizeof buf) == 0, "no line for an empty window");
    rdna4t_add(&H, 2000000); rdna4t_add(&H, 500000); rdna4t_add(&H, 30000000);
    rdna4t_take(&H, &s);
    CHECK("three samples", s.n == 3 && s.sum == 32500000 && s.min == 500000 && s.max == 30000000 && s.b[2] == 1 && s.b[3] == 1 && s.b[5] == 1 && s.b[0] + s.b[1] + s.b[4] + s.b[6] + s.b[7] == 0,
          "n=%llu sum=%llu min=%llu max=%llu (0.5ms->b2, 2ms->b3, 30ms->b5)", (unsigned long long)s.n, (unsigned long long)s.sum, (unsigned long long)s.min, (unsigned long long)s.max);
    rdna4t_fmt("pipe", &s, buf, sizeof buf); printf("     %s\n", buf);
    CHECK("fmt content", strstr(buf, "T1 pipe n=3 avg=10.833ms min=0.500ms max=30.000ms") != NULL, "avg 10.833");
    rdna4t_take(&H, &s); CHECK("drained", s.n == 0, "second take empty");
    rdna4t_add(&H, 0); rdna4t_take(&H, &s); CHECK("zero-ns sample keeps min 0", s.n == 1 && s.min == 0 && s.max == 0, "min=%llu", (unsigned long long)s.min);
    pthread_t t[4]; for (int i = 0; i < 4; i++) pthread_create(&t[i], NULL, worker, NULL);
    for (int i = 0; i < 4; i++) pthread_join(t[i], NULL);
    rdna4t_take(&H, &s); CHECK("4 threads", s.n == 400000 && s.min == 1000 && s.max == 1006 && s.b[0] == 400000, "n=%llu min=%llu max=%llu", (unsigned long long)s.n, (unsigned long long)s.min, (unsigned long long)s.max);

    // ---- pipeline cache file ----
    uint8_t key[32], uuid[16], key2[32], uuid2[16]; for (int i = 0; i < 32; i++) { key[i] = (uint8_t)i; key2[i] = (uint8_t)(i ^ 0x55); } for (int i = 0; i < 16; i++) { uuid[i] = (uint8_t)(0xA0 + i); uuid2[i] = (uint8_t)i; }
    size_t plen = 1000; uint8_t *pay = malloc(plen); for (size_t i = 0; i < plen; i++) pay[i] = (uint8_t)(i * 7);
    uint32_t vk[4] = { 32, 1, 0x1002, 0x7550 }; memcpy(pay, vk, 16); memcpy(pay + 16, uuid, 16);
    size_t flen = sizeof(rdna4pc_hdr) + plen; uint8_t *f = malloc(flen);
    rdna4pc_hdr h; rdna4pc_fill(&h, key, uuid, 0x1002, 0x7550, pay, plen); memcpy(f, &h, sizeof h); memcpy(f + sizeof h, pay, plen);
    const void *pp = NULL; size_t pl = 0;
    CHECK("header size is 88", sizeof(rdna4pc_hdr) == 88, "%zu", sizeof(rdna4pc_hdr));
    int c = rdna4pc_check(f, flen, key, uuid, 0x1002, 0x7550, &pp, &pl); CHECK("valid file", c == RDNA4PC_OK && pl == plen && pp == f + sizeof h, "%s", rdna4pc_why(c));
    c = rdna4pc_check(f, flen - 1, key, uuid, 0x1002, 0x7550, &pp, &pl); CHECK("truncated by 1", c == RDNA4PC_LEN, "%s", rdna4pc_why(c));
    c = rdna4pc_check(f, sizeof h - 1, key, uuid, 0x1002, 0x7550, &pp, &pl); CHECK("shorter than header", c == RDNA4PC_SHORT, "%s", rdna4pc_why(c));
    c = rdna4pc_check(f, 0, key, uuid, 0x1002, 0x7550, &pp, &pl); CHECK("empty file", c == RDNA4PC_SHORT, "%s", rdna4pc_why(c));
    c = rdna4pc_check(f, flen, key2, uuid, 0x1002, 0x7550, &pp, &pl); CHECK("other build key", c == RDNA4PC_KEY, "%s", rdna4pc_why(c));
    c = rdna4pc_check(f, flen, key, uuid2, 0x1002, 0x7550, &pp, &pl); CHECK("other UUID", c == RDNA4PC_UUID, "%s", rdna4pc_why(c));
    c = rdna4pc_check(f, flen, key, uuid, 0x1002, 0x7551, &pp, &pl); CHECK("other device id", c == RDNA4PC_UUID, "%s", rdna4pc_why(c));
    f[sizeof h + 500] ^= 1; c = rdna4pc_check(f, flen, key, uuid, 0x1002, 0x7550, &pp, &pl); CHECK("flipped payload bit", c == RDNA4PC_HASH, "%s", rdna4pc_why(c)); f[sizeof h + 500] ^= 1;
    f[0] = 'X'; c = rdna4pc_check(f, flen, key, uuid, 0x1002, 0x7550, &pp, &pl); CHECK("bad magic", c == RDNA4PC_MAGIC, "%s", rdna4pc_why(c)); f[0] = 'N';
    f[8] = 2; c = rdna4pc_check(f, flen, key, uuid, 0x1002, 0x7550, &pp, &pl); CHECK("version 2", c == RDNA4PC_VERSION_BAD, "%s", rdna4pc_why(c)); f[8] = 1;
    // an inner Vulkan header that disagrees (self-consistent file, so only the inner check can catch it)
    pay[8] = 9; rdna4pc_fill(&h, key, uuid, 0x1002, 0x7550, pay, plen); memcpy(f, &h, sizeof h); memcpy(f + sizeof h, pay, plen);
    c = rdna4pc_check(f, flen, key, uuid, 0x1002, 0x7550, &pp, &pl); CHECK("inner vk vendor wrong", c == RDNA4PC_VKHDR, "%s", rdna4pc_why(c));
    pay[8] = 0x02; pay[9] = 0x10; pay[10] = 0; pay[11] = 0; memcpy(pay + 16, uuid2, 16); rdna4pc_fill(&h, key, uuid, 0x1002, 0x7550, pay, plen); memcpy(f, &h, sizeof h); memcpy(f + sizeof h, pay, plen);
    c = rdna4pc_check(f, flen, key, uuid, 0x1002, 0x7550, &pp, &pl); CHECK("inner vk uuid wrong", c == RDNA4PC_VKHDR, "%s", rdna4pc_why(c));
    printf(fails ? "FAILED %d\n" : "ALL PASS\n", fails);
    return fails != 0;
}
