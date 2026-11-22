// Host test for rdna4_hotswap.h:  cc -O1 -Wall -Wextra -Werror -pthread -o /tmp/test-hotswap test-hotswap.c && /tmp/test-hotswap
#include <stdio.h>
#include <pthread.h>
#include "rdna4_hotswap.h"
static int fails, checks;
#define CK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)
#define S 1000000000LL
static rdna4hs_key_obs mk(int spv, uint64_t sz, int64_t mt, int meta) {
    rdna4hs_key_obs k = { { spv, sz, mt }, { meta, meta ? 100 : 0, meta ? mt : 0 } }; return k; }

static void t_basic(void) {
    rdna4hs_entry e; rdna4hs_init(&e, 2, 1);
    rdna4hs_key_obs none[2] = { mk(0, 0, 0, 0), mk(0, 0, 0, 0) };
    CK(rdna4hs_tick(&e, 10 * S, none) == RDNA4HS_NONE);
    rdna4hs_key_obs one[2] = { mk(1, 400, 10 * S, 1), mk(0, 0, 0, 0) };      // only the vertex file is there
    CK(rdna4hs_tick(&e, 11 * S, one) == RDNA4HS_NONE); CK(rdna4hs_tick(&e, 12 * S, one) == RDNA4HS_NONE);
    rdna4hs_key_obs both[2] = { mk(1, 400, 10 * S, 1), mk(1, 800, 13 * S, 1) };
    CK(rdna4hs_tick(&e, 13 * S + 100, both) == RDNA4HS_NONE);                   // first sighting: not yet stable
    CK(rdna4hs_tick(&e, 13 * S + 200000000, both) == RDNA4HS_NONE);             // stable but newest file younger than MIN_AGE
    CK(rdna4hs_tick(&e, 14 * S, both) == RDNA4HS_BUILD); CK(e.st == RDNA4HS_BUILDING); CK(e.attempts == 1);
    CK(rdna4hs_tick(&e, 15 * S, both) == RDNA4HS_NONE);                          // one BUILD per attempt
    CK(rdna4hs_build_done(&e, 1, 15 * S) == 1); CK(e.st == RDNA4HS_SWAPPED);
    CK(rdna4hs_tick(&e, 16 * S, both) == RDNA4HS_NONE);
    CK(rdna4hs_build_done(&e, 1, 16 * S) == 0);                                // a second completion is ignored
}
static void t_cold_pair_poll_delay(void) {
    const int64_t registered = 20 * S;
    const int64_t published = registered + 101000000LL;
    rdna4hs_entry entry;
    rdna4hs_init(&entry, 2, 1);
    rdna4hs_key_obs modules[2] = { mk(1, 2008, 19 * S, 1), mk(1, 16944, published, 1) };
    int64_t eligible = 0;
    for (int64_t time = registered + RDNA4HS_POLL_NS; time <= registered + 2 * S; time += RDNA4HS_POLL_NS) {
        rdna4hs_key_obs current[2] = { modules[0], time >= published ? modules[1] : mk(0, 0, 0, 0) };
        if (rdna4hs_tick(&entry, time, current) == RDNA4HS_BUILD) { eligible = time; break; }
    }
    CK(eligible - published >= RDNA4HS_MIN_AGE_NS);
    CK(eligible - published < RDNA4HS_MIN_AGE_NS + 100000000LL);
    printf("cold shader pair ready-to-build latency: %lld ms\n", (long long)((eligible - published) / 1000000LL));
}
static void t_partial_file(void) {
    rdna4hs_entry e; rdna4hs_init(&e, 1, 1);
    rdna4hs_key_obs a = mk(1, 400, 10 * S, 1), b = mk(1, 800, 11 * S, 1), odd = mk(1, 402, 10 * S, 1);
    CK(rdna4hs_tick(&e, 20 * S, &a) == RDNA4HS_NONE);
    CK(rdna4hs_tick(&e, 21 * S, &b) == RDNA4HS_NONE);                            // grew between ticks: never stable on the first sighting
    CK(rdna4hs_tick(&e, 22 * S, &b) == RDNA4HS_BUILD);
    rdna4hs_entry f; rdna4hs_init(&f, 1, 1);
    CK(rdna4hs_tick(&f, 20 * S, &odd) == RDNA4HS_NONE); CK(rdna4hs_tick(&f, 21 * S, &odd) == RDNA4HS_NONE); CK(f.st == RDNA4HS_WAIT);   // size % 4 != 0
    rdna4hs_key_obs tiny = mk(1, 16, 10 * S, 1);
    CK(rdna4hs_tick(&f, 22 * S, &tiny) == RDNA4HS_NONE); CK(rdna4hs_tick(&f, 23 * S, &tiny) == RDNA4HS_NONE);                         // < 20 bytes
}
static void t_meta(void) {
    rdna4hs_entry c; rdna4hs_init(&c, 1, 1);                                     // compute: meta required forever
    rdna4hs_key_obs nm = mk(1, 400, 10 * S, 0);
    for (int i = 0; i < 20; i++) CK(rdna4hs_tick(&c, (20 + i) * S, &nm) == RDNA4HS_NONE);
    rdna4hs_key_obs wm = mk(1, 400, 10 * S, 1); wm.meta.mtime_ns = 10 * S;
    CK(rdna4hs_tick(&c, 50 * S, &wm) == RDNA4HS_NONE); CK(rdna4hs_tick(&c, 51 * S, &wm) == RDNA4HS_BUILD);
    rdna4hs_entry r; rdna4hs_init(&r, 1, 0);                                     // render: proceeds without meta only after the grace period
    CK(rdna4hs_tick(&r, 11 * S, &nm) == RDNA4HS_NONE); CK(rdna4hs_tick(&r, 12 * S, &nm) == RDNA4HS_NONE);
    CK(rdna4hs_tick(&r, 14 * S, &nm) == RDNA4HS_NONE);                           // 4 s old < 5 s grace
    CK(rdna4hs_tick(&r, 16 * S, &nm) == RDNA4HS_BUILD);
}
static void t_failure_backoff(void) {
    rdna4hs_entry e; rdna4hs_init(&e, 1, 1); rdna4hs_key_obs a = mk(1, 400, 10 * S, 1);
    int64_t t = 20 * S; rdna4hs_tick(&e, t, &a);
    CK(rdna4hs_tick(&e, t += S, &a) == RDNA4HS_BUILD);
    CK(rdna4hs_build_done(&e, 0, t) == 0); CK(e.st == RDNA4HS_WAIT); CK(e.attempts == 1); CK(e.next_try_ns == t + 2 * S);
    CK(rdna4hs_tick(&e, t += S, &a) == RDNA4HS_NONE);                            // backoff (2 s) not over yet
    CK(rdna4hs_tick(&e, t += S, &a) == RDNA4HS_BUILD);                           // backoff elapsed (>= 2 s)
    CK(rdna4hs_build_done(&e, 0, t) == 0);
    for (int i = 0; i < 20 && e.st == RDNA4HS_WAIT; i++) { if (rdna4hs_tick(&e, t += S, &a) == RDNA4HS_BUILD) rdna4hs_build_done(&e, 0, t); }
    CK(e.st == RDNA4HS_GAVEUP); CK(e.attempts == RDNA4HS_MAX_ATTEMPTS);
    CK(rdna4hs_tick(&e, t += 100 * S, &a) == RDNA4HS_NONE);                      // terminal
    // changed files after a failure reset the budget
    rdna4hs_entry g; rdna4hs_init(&g, 1, 1); int64_t u = 20 * S; rdna4hs_tick(&g, u, &a);
    CK(rdna4hs_tick(&g, u += S, &a) == RDNA4HS_BUILD); rdna4hs_build_done(&g, 0, u); CK(g.attempts == 1);
    rdna4hs_key_obs b = mk(1, 800, 30 * S, 1);
    rdna4hs_tick(&g, u += 5 * S, &b); CK(g.attempts == 0 && g.next_try_ns == 0);
}
static void t_deferred_destroy(void) {
    rdna4hs_entry e; rdna4hs_init(&e, 1, 1); rdna4hs_key_obs a = mk(1, 400, 10 * S, 1);
    int g1 = rdna4hs_acquire(&e), g2 = rdna4hs_acquire(&e); CK(g1 == 0 && g2 == 0 && e.users[0] == 2);   // two command buffers encode with the fallback
    CK(rdna4hs_destroy_ok(&e) == 0);                                                                   // not swapped: never destroy
    rdna4hs_tick(&e, 20 * S, &a); CK(rdna4hs_tick(&e, 21 * S, &a) == RDNA4HS_BUILD); CK(rdna4hs_build_done(&e, 1, 21 * S) == 1);
    CK(rdna4hs_destroy_ok(&e) == 0);                                                                   // swapped, but two cbs in flight
    int g3 = rdna4hs_acquire(&e); CK(g3 == 1 && e.users[0] == 2 && e.users[1] == 1);                   // new work gets the real generation
    rdna4hs_release(&e, g1); CK(rdna4hs_destroy_ok(&e) == 0);
    rdna4hs_release(&e, g2); CK(e.users[0] == 0);
    CK(rdna4hs_destroy_ok(&e) == 1); CK(rdna4hs_destroy_ok(&e) == 0);                                    // exactly once
    rdna4hs_release(&e, g3); rdna4hs_release(&e, g3); CK(e.users[1] == 0);                               // over-release is clamped
    rdna4hs_release(&e, 7); rdna4hs_release(&e, -1);
    // swapped with no users at all: destroy at once
    rdna4hs_entry f; rdna4hs_init(&f, 1, 1); rdna4hs_tick(&f, 20 * S, &a); rdna4hs_tick(&f, 21 * S, &a); rdna4hs_build_done(&f, 1, 21 * S); CK(rdna4hs_destroy_ok(&f) == 1);
    // a failed build never retires the fallback
    rdna4hs_entry h; rdna4hs_init(&h, 1, 1); rdna4hs_tick(&h, 20 * S, &a); rdna4hs_tick(&h, 21 * S, &a); rdna4hs_build_done(&h, 0, 21 * S); CK(rdna4hs_destroy_ok(&h) == 0);
}
static _Atomic uintptr_t slot; static _Atomic int winners, bad;
static void *racer(void *arg) { uintptr_t v = (uintptr_t)arg; if (rdna4hs_install(&slot, v)) atomic_fetch_add(&winners, 1); return NULL; }
static void *reader(void *arg) {
    (void)arg; uintptr_t seen = 0;
    for (int i = 0; i < 2000000; i++) { uintptr_t v = rdna4hs_load(&slot); if (v && !seen) seen = v; if (seen && v != seen) atomic_fetch_add(&bad, 1); if (!v && seen) atomic_fetch_add(&bad, 1); }
    return NULL;
}
static void t_publish_race(void) {
    for (int round = 0; round < 50; round++) {
        atomic_store(&slot, 0); atomic_store(&winners, 0); atomic_store(&bad, 0);
        pthread_t r[4], w[4]; for (int i = 0; i < 4; i++) pthread_create(&r[i], NULL, reader, NULL);
        for (int i = 0; i < 4; i++) pthread_create(&w[i], NULL, racer, (void *)(uintptr_t)(0x1000 + 0x10 * (i + 1)));
        for (int i = 0; i < 4; i++) { pthread_join(w[i], NULL); pthread_join(r[i], NULL); }
        CK(atomic_load(&winners) == 1); CK(atomic_load(&bad) == 0); CK(rdna4hs_load(&slot) >= 0x1010 && rdna4hs_load(&slot) <= 0x1040);
    }
}
int main(void) {
    t_basic(); t_cold_pair_poll_delay(); t_partial_file(); t_meta(); t_failure_backoff(); t_deferred_destroy(); t_publish_race();
    printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails != 0;
}
