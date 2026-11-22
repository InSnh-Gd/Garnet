#include <stdint.h>
#include <stddef.h>
#include "rdna4_query_caps.h"
unsigned seen;
uint32_t garnetx_abi_version(void) {
#ifdef UNKNOWN_ABI
    return 99;
#else
    return 3;
#endif
}
// Old entry points must never be called by the ABI 3 native path.
int garnetx_translate(void) { return 99; }
int garnetx_translate_linked(void) { return 99; }
void garnetx_free(uint8_t *p, size_t n) { (void)p; (void)n; }
#define OUT uint8_t **s, size_t *sl, uint8_t **m, size_t *ml, char *e, size_t en
#define IGNORE_OUT (void)s; (void)sl; (void)m; (void)ml; (void)e; (void)en
int garnetx_translate_with_capabilities(const uint8_t *l, size_t n, const uint32_t *local, OUT, const rdna4x_backend_capabilities *c) {
    (void)l; (void)n; (void)local; IGNORE_OUT;
    if (!rdna4x_query_supported(c)) return 3;
    seen |= rdna4x_query_uses_quad(c) ? 1 : 8; return 0;
}
int garnetx_translate_linked_with_capabilities(const uint8_t *l, size_t n, const uint32_t *local, const void *deps, size_t nd, OUT, const rdna4x_backend_capabilities *c) {
    (void)l; (void)n; (void)local; (void)deps; (void)nd; IGNORE_OUT;
    if (!rdna4x_query_supported(c)) return 3;
    seen |= rdna4x_query_uses_quad(c) ? 2 : 16; return 0;
}
#ifndef MISSING_EXPORT
int garnetx_lower_argument_resources_with_capabilities(const uint8_t *l, size_t n, const uint8_t *meta, size_t metas, OUT, const rdna4x_backend_capabilities *c) {
    (void)l; (void)n; (void)meta; (void)metas; IGNORE_OUT;
    if (!rdna4x_query_supported(c)) return 3;
    seen |= rdna4x_query_uses_quad(c) ? 4 : 32; return 0;
}
#endif
