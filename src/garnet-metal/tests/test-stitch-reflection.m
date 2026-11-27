// test-stitch-reflection CASES_DIR - byte-compare rdna4_stitch_reflection output
// with the native MTLCompiler oracle (stitch-corpus + b* fixtures).
// Layout: CASES_DIR must contain *.airr inputs and *.reply/*.out expected files.
// Also accepts AIRR_FILE REPLY_FILE; compile with -x c for Linux host checks.
// Directory mode also runs truncation checks and unsupported-input regressions.
#define RDNA4S_TRACE(...) ((void)0)
#include "garnet_stitch_reflection.h"
#include <assert.h>
#include <dirent.h>
#include <string.h>
#include <unistd.h>

static uint8_t *readFile(const char *path, size_t *length) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END)) { fclose(f); return NULL; }
    long size = ftell(f);
    if (size < 0 || fseek(f, 0, SEEK_SET)) { fclose(f); return NULL; }
    uint8_t *bytes = malloc(size ? (size_t)size : 1);
    if (!bytes || fread(bytes, 1, (size_t)size, f) != (size_t)size) {
        free(bytes); fclose(f); return NULL;
    }
    fclose(f);
    *length = (size_t)size;
    return bytes;
}

static int check(const char *airrPath, const char *expectPath) {
    size_t airrLen = 0, el = 0;
    uint8_t *airr = readFile(airrPath, &airrLen);
    uint8_t *eb = readFile(expectPath, &el);
    if (!airr || !eb) {
        printf("FAIL %s (cannot read pair)\n", airrPath);
        free(airr); free(eb); return 1;
    }
    uint8_t *out = NULL; size_t outLen = 0;
    int ok = rdna4_stitch_reflection(airr, airrLen, NULL, &out, &outLen);
    if (!ok) {
        printf("FAIL %s: adapter returned error\n", airrPath);
        free(airr); free(eb); return 1;
    }
    int pass = outLen == el && !memcmp(out, eb, el);
    printf("%s %s: got %zu bytes, want %zu %s\n", pass ? "PASS" : "DIFF",
           airrPath, outLen, el, pass ? "" : "(mismatch)");
    if (!pass) {
        size_t first = outLen < el ? outLen : el;
        for (size_t i = 0; i < first; ++i)
            if (out[i] != eb[i]) { printf("  first diff @0x%zx: got %02x want %02x\n", i, out[i], eb[i]); break; }
    }
    /* Exact-size allocations put each truncated input against ASan's redzone.
     * Unused trailing bytes may be omitted, but any accepted prefix must still
     * reproduce the complete native reply. */
    for (size_t n = 0; pass && n < airrLen; ++n) {
        uint8_t *prefix = malloc(n ? n : 1), *reply = NULL;
        size_t replyLen = 123;
        if (!prefix) { pass = 0; break; }
        memcpy(prefix, airr, n);
        int accepted = rdna4_stitch_reflection(prefix, n, NULL, &reply, &replyLen);
        pass = accepted ? replyLen == el && !memcmp(reply, eb, el) : !reply && !replyLen;
        if (!pass) printf("FAIL %s: truncated prefix %zu\n", airrPath, n);
        free(reply); free(prefix);
    }
    free(out); free(airr); free(eb);
    return pass ? 0 : 1;
}

static void reject(const uint8_t *bytes, size_t length) {
    uint8_t *reply = NULL;
    size_t replyLen = 123;
    assert(!rdna4_stitch_reflection(bytes, length, NULL, &reply, &replyLen));
    assert(!reply && !replyLen);
}

static int safetyChecks(const char *dir) {
    char path[1024]; size_t length;
    snprintf(path, sizeof path, "%s/structure1.airr", dir);
    uint8_t *b = readFile(path, &length);
    if (!b) return 0; /* Arbitrary external corpora need not have this fixture. */
    /* These offsets belong to the captured structure1 fixture (see README). */
    if (length != 460) { free(b); puts("FAIL unexpected structure1 fixture"); return 1; }
    reject(NULL, 0);
    uint8_t malformed[16] = {0, 0, 0, 0, 'A', 'I', 'R', 'R'};
    reject(malformed, sizeof malformed);
    uint8_t *reply = NULL; size_t replyLen;
    assert(!rdna4_stitch_reflection(b, length, NULL, NULL, &replyLen));
    assert(!rdna4_stitch_reflection(b, length, NULL, &reply, NULL));
    assert(!rdna4_stitch_reflection(b, length, "missing_function", &reply, &replyLen));
    assert(!reply && !replyLen);

    const struct { size_t offset; uint32_t value; } bad[] = {
        {0xb4, 1},       /* member b references Pair itself: nested struct */
        {0xb4, 9999},    /* invalid member type NodeId */
        {0xac, 17},      /* member offset beyond Pair's 16 bytes */
        {0x170, 17},     /* too many struct members */
        {0x17c, 64},     /* struct name exceeds supported capacity */
        {0x12c, 64},     /* member name exceeds supported capacity */
        {0x1b8, 9999},   /* invalid argument NodeId in the signature */
        {0x1b8, 6},      /* unnamed type node used as argument */
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        uint32_t saved = rdna4s_u32(b + bad[i].offset);
        memcpy(b + bad[i].offset, &bad[i].value, 4);
        reject(b, length);
        memcpy(b + bad[i].offset, &saved, 4);
    }
    free(b);

    snprintf(path, sizeof path, "%s/b8.airr", dir);
    b = readFile(path, &length);
    if (!b || length != 304) { free(b); puts("FAIL missing b8 fixture"); return 1; }
    /* Make the return type the existing matrix node, too. */
    uint32_t matrix = 4;
    memcpy(b + 0x118, &matrix, 4);
    rdna4_stitch_arg *parsed = NULL;
    assert(rdna4_stitch_parse_airr(b, length, "f", &parsed) == 2);
    assert(parsed[0].dataType == 15 && parsed[1].dataType == 15);
    assert(parsed[1].size == 64 && parsed[1].align == 16 && !parsed[1].name[0]);
    free(parsed);
    uint32_t invalidRows = 5;
    memcpy(b + 0x84, &invalidRows, 4);
    reject(b, length);
    free(b);

    snprintf(path, sizeof path, "%s/vector3.airr", dir);
    b = readFile(path, &length);
    if (!b || length != 332) { free(b); puts("FAIL missing vector3 fixture"); return 1; }
    /* Change signature order without changing the order of name nodes. */
    uint32_t third = 5, first = 3;
    memcpy(b + 0x134, &third, 4);
    memcpy(b + 0x13c, &first, 4);
    assert(rdna4_stitch_parse_airr(b, length, "vector3", &parsed) == 4);
    assert(!strcmp(parsed[0].name, "z") && !strcmp(parsed[2].name, "x"));
    free(parsed); free(b);

    snprintf(path, sizeof path, "%s/visible1.airr", dir);
    b = readFile(path, &length);
    if (!b || length != 124) { free(b); puts("FAIL missing visible fixture"); return 1; }
    assert(rdna4_stitch_parse_airr(b, length, "probe_visible", &parsed) == 0 && !parsed);
    assert(rdna4_stitch_parse_airr(b, length, "missing_function", &parsed) == -1 && !parsed);
    snprintf(path, sizeof path, "%s/visible1.reply", dir);
    size_t emptyLength = 0;
    uint8_t *emptyOracle = readFile(path, &emptyLength);
    assert(emptyOracle && rdna4_stitch_container(NULL, 0, &reply, &replyLen));
    assert(replyLen == emptyLength && !memcmp(reply, emptyOracle, replyLen));
    free(reply); free(emptyOracle);
    /* A missing signature is valid; invalid function names/references are not. */
    b[0x79] = 'x';
    reject(b, length);
    b[0x79] = 0;
    uint32_t invalidNode = 9999;
    memcpy(b + 0x38, &invalidNode, 4);
    reject(b, length);
    free(b);

    snprintf(path, sizeof path, "%s/linked1.airr", dir);
    b = readFile(path, &length);
    if (!b || length != 340) { free(b); puts("FAIL missing linked fixture"); return 1; }
    assert(rdna4_stitch_parse_airr(b, length, "rdna4LinkedTint", &parsed) == 3);
    assert(parsed[0].dataType == 4 && parsed[1].dataType == 19 && parsed[2].dataType == 19);
    for (unsigned i = 0; i < 3; ++i)
        assert(!parsed[i].name[0] && parsed[i].size == 8 && parsed[i].align == 8);
    free(parsed);
    /* Unnamed arguments must still reference a valid type through a tag-2 node. */
    memcpy(b + 0xb8, &invalidNode, 4);
    reject(b, length);
    free(b);

    /* Exercise allocation sizing with maximum names and all 16 member slots. */
    rdna4_stitch_arg *args = calloc(1024, sizeof *args);
    assert(args);
    for (int i = 0; i < 1024; ++i) {
        memset(args[i].name, 'a', sizeof args[i].name - 1);
        args[i].dataType = RDNA4S_DT_STRUCT;
        memset(args[i].structName, 'S', sizeof args[i].structName - 1);
        args[i].memberCount = 16;
        for (unsigned j = 0; j < 16; ++j) {
            memset(args[i].members[j].name, 'm', sizeof args[i].members[j].name - 1);
            args[i].members[j].dataType = RDNA4S_DT_FLOAT;
        }
    }
    for (int count = 1; count <= 1024; count *= 2) {
        reply = rdna4_stitch_write_psbin(args, count, &replyLen);
        assert(reply && !(replyLen & 7));
        assert((uint64_t)rdna4s_u32(reply + 17) + 36 <= replyLen);
        free(reply);
    }
    reply = rdna4_stitch_write_psbin(NULL, 0, &replyLen);
    assert(reply && replyLen == 64 && rdna4s_u32(reply + 17) == 25 && !rdna4s_u32(reply + 21));
    free(reply);
    assert(!rdna4_stitch_write_psbin(NULL, 1, &replyLen));
    assert(!rdna4_stitch_write_psbin(args, -1, &replyLen));
    assert(!rdna4_stitch_write_psbin(args, 1025, &replyLen));
    args[0].memberCount = 17;
    assert(!rdna4_stitch_write_psbin(args, 1, &replyLen));
    for (int i = 0; i < 1024; ++i) args[i].dataType = RDNA4S_DT_HALF;
    for (int count = 1; count <= 1024; count *= 2) {
        reply = rdna4_stitch_write_psbin(args, count, &replyLen);
        assert(reply && !(replyLen & 7));
        free(reply);
    }
    free(args);
    puts("STITCH_SAFETY_PASS");
    return 0;
}

int main(int argc, const char *argv[]) {
    if (argc == 3) return check(argv[1], argv[2]);
    if (argc != 2) { puts("usage: test-stitch-reflection CASES_DIR | AIRR_FILE REPLY_FILE"); return 2; }
    int fails = 0, checked = 0;
    DIR *d = opendir(argv[1]);
    if (!d) { perror(argv[1]); return 2; }
    struct dirent *de;
    while ((de = readdir(d))) {
        const char *n = de->d_name;
        const char *dot = strrchr(n, '.');
        if (!dot || strcmp(dot, ".airr")) continue;
        char airr[1024], a[1024];
        snprintf(airr, sizeof airr, "%s/%s", argv[1], n);
        /* try .reply then .out */
        snprintf(a, sizeof a, "%s/%.*s.reply", argv[1], (int)(dot - n), n);
        if (access(a, R_OK)) snprintf(a, sizeof a, "%s/%.*s.out", argv[1], (int)(dot - n), n);
        if (access(a, R_OK)) {
            printf("FAIL %s: missing expected reply\n", airr);
            fails++; continue;
        }
        fails += check(airr, a); checked++;
    }
    closedir(d);
    /* also rdna4-stitch-input.bin/native-stitch-output.bin pair if present */
    char ip[1024], op[1024];
    snprintf(ip, sizeof ip, "%s/rdna4-stitch-input.bin", argv[1]);
    snprintf(op, sizeof op, "%s/native-stitch-output.bin", argv[1]);
    if (!access(ip, R_OK) && !access(op, R_OK)) { fails += check(ip, op); checked++; }
    if (!checked) { puts("STITCH_FAIL no fixture pairs"); return 1; }
    fails += safetyChecks(argv[1]);
    printf("STITCH_%s cases=%d failures=%d\n", fails ? "FAIL" : "PASS", checked, fails);
    return fails ? 1 : 0;
}
