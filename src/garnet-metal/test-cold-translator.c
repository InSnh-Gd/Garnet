// Test-only translation delay/refusal around the real translator ABI.
#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <time.h>
#include "rdna4_query_caps.h"

static void *realLibrary;
static _Atomic unsigned calls;
static int slow, refuse;

void rdna4_cold_configure(const char *path, int delay, int reject) {
    realLibrary = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!realLibrary) { fprintf(stderr, "COLD_TRANSLATOR_LOAD_FAIL %s\n", dlerror()); abort(); }
    slow = delay;
    refuse = reject;
}

unsigned rdna4_cold_call_count(void) { return atomic_load(&calls); }

static int beforeTranslation(char *error, size_t length) {
    unsigned call = atomic_fetch_add(&calls, 1);
    if (slow && call == 0) {
        // The caller selects a delay below or above the current 3-second budget.
        struct timespec left = {slow, 0};
        while (nanosleep(&left, &left) && errno == EINTR) { }
    }
    if (refuse && length) snprintf(error, length, "test translator refusal");
    return refuse;
}

uint32_t garnetx_abi_version(void) {
    uint32_t (*version)(void);
    *(void **)&version = dlsym(realLibrary, "garnetx_abi_version");
    if (!version) abort();
    return version();
}

static int refuseTranslation(uint8_t **spv, size_t *spvLength, uint8_t **meta,
                             size_t *metaLength, char *error, size_t errorLength) {
    if (!beforeTranslation(error, errorLength)) return 0;
    if (spv) *spv = NULL;
    if (spvLength) *spvLength = 0;
    if (meta) *meta = NULL;
    if (metaLength) *metaLength = 0;
    return 1;
}

int garnetx_translate(const uint8_t *text, size_t textLength, const uint32_t *local,
                   uint8_t **spv, size_t *spvLength, uint8_t **meta, size_t *metaLength,
                   char *error, size_t errorLength) {
    if (refuseTranslation(spv, spvLength, meta, metaLength, error, errorLength)) return 1;
    int (*translate)(const uint8_t *, size_t, const uint32_t *, uint8_t **, size_t *, uint8_t **, size_t *, char *, size_t);
    *(void **)&translate = dlsym(realLibrary, "garnetx_translate");
    if (!translate) abort();
    return translate(text, textLength, local, spv, spvLength, meta, metaLength, error, errorLength);
}

int garnetx_translate_linked(const uint8_t *text, size_t textLength, const uint32_t *local,
                          const void *deps, size_t count, uint8_t **spv, size_t *spvLength,
                          uint8_t **meta, size_t *metaLength, char *error, size_t errorLength) {
    if (refuseTranslation(spv, spvLength, meta, metaLength, error, errorLength)) return 1;
    int (*translate)(const uint8_t *, size_t, const uint32_t *, const void *, size_t, uint8_t **, size_t *, uint8_t **, size_t *, char *, size_t);
    *(void **)&translate = dlsym(realLibrary, "garnetx_translate_linked");
    if (!translate) abort();
    return translate(text, textLength, local, deps, count, spv, spvLength, meta, metaLength, error, errorLength);
}

void garnetx_free(uint8_t *bytes, size_t length) {
    void (*release)(uint8_t *, size_t);
    *(void **)&release = dlsym(realLibrary, "garnetx_free");
    if (!release) abort();
    release(bytes, length);
}

int garnetx_lower_argument_resources(const uint8_t *spv, size_t spvLength, const uint8_t *meta, size_t metaLength,
    uint8_t **code, size_t *codeLength, uint8_t **json, size_t *jsonLength, char *error, size_t errorLength) {
    int (*lower)(const uint8_t *, size_t, const uint8_t *, size_t, uint8_t **, size_t *, uint8_t **, size_t *, char *, size_t);
    *(void **)&lower = dlsym(realLibrary, "garnetx_lower_argument_resources");
    if (!lower) { snprintf(error, errorLength, "real translator lacks argument lowering"); return 1; }
    return lower(spv, spvLength, meta, metaLength, code, codeLength, json, jsonLength, error, errorLength);
}

int garnetx_translate_with_capabilities(const uint8_t *text, size_t textLength, const uint32_t *local,
    uint8_t **spv, size_t *spvLength, uint8_t **meta, size_t *metaLength,
    char *error, size_t errorLength, const rdna4x_backend_capabilities *capabilities) {
    if (refuseTranslation(spv, spvLength, meta, metaLength, error, errorLength)) return 1;
    int (*translate)(const uint8_t *, size_t, const uint32_t *, uint8_t **, size_t *, uint8_t **,
        size_t *, char *, size_t, const rdna4x_backend_capabilities *);
    *(void **)&translate = dlsym(realLibrary, "garnetx_translate_with_capabilities");
    if (!translate) abort();
    return translate(text, textLength, local, spv, spvLength, meta, metaLength,
        error, errorLength, capabilities);
}

int garnetx_translate_linked_with_capabilities(const uint8_t *text, size_t textLength, const uint32_t *local,
    const void *deps, size_t count, uint8_t **spv, size_t *spvLength, uint8_t **meta, size_t *metaLength,
    char *error, size_t errorLength, const rdna4x_backend_capabilities *capabilities) {
    if (refuseTranslation(spv, spvLength, meta, metaLength, error, errorLength)) return 1;
    int (*translate)(const uint8_t *, size_t, const uint32_t *, const void *, size_t, uint8_t **,
        size_t *, uint8_t **, size_t *, char *, size_t, const rdna4x_backend_capabilities *);
    *(void **)&translate = dlsym(realLibrary, "garnetx_translate_linked_with_capabilities");
    if (!translate) abort();
    return translate(text, textLength, local, deps, count, spv, spvLength, meta, metaLength,
        error, errorLength, capabilities);
}

int garnetx_lower_argument_resources_with_capabilities(const uint8_t *spv, size_t spvLength,
    const uint8_t *meta, size_t metaLength, uint8_t **code, size_t *codeLength,
    uint8_t **json, size_t *jsonLength, char *error, size_t errorLength,
    const rdna4x_backend_capabilities *capabilities) {
    int (*lower)(const uint8_t *, size_t, const uint8_t *, size_t, uint8_t **, size_t *,
        uint8_t **, size_t *, char *, size_t, const rdna4x_backend_capabilities *);
    *(void **)&lower = dlsym(realLibrary, "garnetx_lower_argument_resources_with_capabilities");
    if (!lower) abort();
    return lower(spv, spvLength, meta, metaLength, code, codeLength, json, jsonLength,
        error, errorLength, capabilities);
}
