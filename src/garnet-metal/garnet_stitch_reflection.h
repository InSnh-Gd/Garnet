#pragma once
/* garnet_stitch_reflection.h - AIRR -> MTLPSBIN stitching-reflection adapter.
 *
 * RDNA4's pipelines compile from AIR, so [function reflectionData] carries
 * Apple's AIRR flatbuffer.  When Metal's MTLCompiler asks for stitching
 * reflection (options 0x20000000, e.g. RenderBox custom effects), the result
 * must be a compiler reflection container carrying an "MTLPSBIN" record.
 * The container records its unpadded length; passing the padded record alone
 * makes MTLStitchingReflectionDeserializer::deserialize abort.
 *
 * This adapter parses the AIRR VisibleFunction signature and serializes it
 * using the MTLPSBIN layout and 80-byte envelope captured from Apple's MTLCompiler
 * (see src/garnet-metal/test-stitch-reflection.m).
 *
 * AIRR facts used (verified against the captured test shader corpus):
 *   root.field1 -> vector<Offset<Node>>, root.field6 -> vector<u32> visible idx
 *   Node = { u8 nodeType, uoffset payload }
 *   VisibleFunction(tag 4): f0=name string, f1=signature table;
 *     signature.f0=return NodeId, signature.f1=vector<argument NodeId>.
 *   Name node(tag 2): f0=u32 decl/type NodeId, f1=optional arg-name string.
 *     Linked stitched functions omit the argument names.
 *   Decl node: f0=byteSize, f1=byteAlign, f3=type NodeId;
 *     tag17.f4=vector width; tag19.f4/f5=matrix columns/rows.
 *   Struct(tag 28): f0=size, f1=align, f3=name, f4=member NodeId vector.
 *     Member(tag 27): f0=offset (default 0), f2=type NodeId, f3=name.
 *   Type tags: 3=Char 5=Short 7=Int 8=UInt 13=HalfN 14=FloatN 28=Struct
 *     (tag2 also appears as Bool type node, tag4 reused for texture decls,
 *      tag18 for sampler decls).
 */
#ifndef RDNA4_STITCH_REFLECTION_H
#define RDNA4_STITCH_REFLECTION_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#ifndef RDNA4S_TRACE
#define RDNA4S_TRACE(...) fprintf(stderr,__VA_ARGS__)
#endif
#define RDNA4S_FAIL(l) do{RDNA4S_TRACE("fail@%d\n",l);goto fail;}while(0)

#ifdef __cplusplus
extern "C" {
#endif

/* One parsed signature argument (or the return slot). */
typedef struct {
    char     name[64];          /* empty for unnamed inputs and the return   */
    uint32_t dataType;          /* MTLDataType (Float=3, Half=16, Texture=58) */
    uint32_t align;             /* byte alignment                            */
    uint32_t size;              /* byte size                                 */
    uint32_t extra[3];          /* texture/sampler descriptors, else zero    */
    /* struct members (dataType==1): serialized nested field records */
    char     structName[64];
    uint32_t memberCount;
    struct { char name[64]; uint32_t offset, dataType, align, size, extra[3]; }
           members[16];
} rdna4_stitch_arg;

/* Parse the AIRR flatbuffer into an argument list (+ implicit return record).
 * Returns record count (return record last), zero for a visible function
 * without a stitching signature, or
 * -1 on malformed input / unsupported constructs.  *args_out is malloc'd. */
static int rdna4_stitch_parse_airr(const uint8_t *buf, size_t len,
                                 const char *function_name,
                                 rdna4_stitch_arg **args_out);

/* Serialize args to the raw, 8-byte-padded MTLPSBIN payload.
 * This is the first section of the compiler reply, not the complete reply.
 * Returns malloc'd bytes or NULL. */
static uint8_t *rdna4_stitch_write_psbin(const rdna4_stitch_arg *args, int count,
                                       size_t *out_size);

/* Convenience: AIRR in, compiler reflection container out. output is malloc'd.
 * Returns 1 on success, 0 on malformed/unsupported input. */
static int rdna4_stitch_reflection(const uint8_t *airr, size_t airr_size,
                                 const char *function_name,
                                 uint8_t **output, size_t *output_size);

/* ------------------------------------------------------------------ */

static inline uint16_t rdna4s_u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t rdna4s_u32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

typedef struct {
    const uint8_t *buf;
    size_t len;
} rdna4s_slice;

/* Bounds-checked flatbuffers table field accessor: returns offset of field
 * `fi`'s inline data inside the table at `t`, or 0 when absent/invalid. */
static size_t rdna4s_field(const rdna4s_slice *s, size_t t, unsigned fi) {
    if (!t || t > s->len || s->len - t < 4) return 0;
    int32_t soff = (int32_t)rdna4s_u32(s->buf + t);
    int64_t vto = (int64_t)t - soff;
    if (vto < 0 || (uint64_t)vto > s->len || s->len - (size_t)vto < 4) return 0;
    size_t vt = (size_t)vto;
    uint16_t vlen = rdna4s_u16(s->buf + vt), tlen = rdna4s_u16(s->buf + vt + 2);
    if (vlen < 4 || vlen > 1024 || vlen > s->len - vt || tlen > s->len - t) return 0;
    size_t fe = 4 + 2 * (size_t)fi;
    if (fe + 2 > vlen) return 0;
    uint16_t off = rdna4s_u16(s->buf + vt + fe);
    if (!off || off >= tlen || t + off > s->len) return 0;
    return t + off;
}
static int rdna4s_u32_at(const rdna4s_slice *s, size_t o, uint32_t *v) {
    if (o > s->len || s->len - o < 4) return 0;
    *v = rdna4s_u32(s->buf + o); return 1;
}
static int rdna4s_string(const rdna4s_slice *s, size_t field_loc, char *out, size_t cap) {
    uint32_t uoff;
    if (!field_loc || !rdna4s_u32_at(s, field_loc, &uoff) || !uoff || uoff > s->len - field_loc) return 0;
    size_t t = field_loc + uoff;
    uint32_t n;
    if (!rdna4s_u32_at(s, t, &n) || n >= cap || n >= s->len - t - 4 || s->buf[t + 4 + n]) return 0;
    memcpy(out, s->buf + t + 4, n); out[n] = 0;
    for (uint32_t i = 0; i < n; ++i) if (out[i] < 0x20 || out[i] > 0x7e) return 0;
    return 1;
}

/* MTLDataType encodings used by the serialized record. */
enum {
    RDNA4S_DT_STRUCT = 1, RDNA4S_DT_FLOAT = 3, RDNA4S_DT_HALF = 16,
    RDNA4S_DT_INT = 29, RDNA4S_DT_UINT = 33, RDNA4S_DT_SHORT = 37,
    RDNA4S_DT_CHAR = 45, RDNA4S_DT_BOOL = 53,
    RDNA4S_DT_TEXTURE = 58, RDNA4S_DT_SAMPLER = 59
};

/* Missing scalar fields have FlatBuffer's zero default. */
static int rdna4s_value(const rdna4s_slice *s, size_t t, unsigned fi, uint32_t *v) {
    size_t f = rdna4s_field(s, t, fi);
    *v = 0;
    return !f || rdna4s_u32_at(s, f, v);
}
static size_t rdna4s_indirect(const rdna4s_slice *s, size_t f) {
    uint32_t off;
    if (!f || !rdna4s_u32_at(s, f, &off) || !off || off > s->len - f) return 0;
    return f + off;
}
static int rdna4s_vector(const rdna4s_slice *s, size_t f, size_t *v, uint32_t *n) {
    *v = rdna4s_indirect(s, f);
    return *v && rdna4s_u32_at(s, *v, n) && *n <= (s->len - *v - 4) / 4;
}

/* Parse one type for either an argument, return, or flat struct member.
 * Nested structs and resource members have no captured wire layout: reject. */
static int rdna4s_type(const rdna4s_slice *s, const uint8_t *tags,
                      const size_t *pay, uint32_t nvec, uint32_t node,
                      rdna4_stitch_arg *a, int member) {
    if (node >= nvec) return 0;
    uint32_t tag = tags[node], fam = tag, width = 1, rows = 0;
    size_t p = pay[node];
    if (tag == 28) {
        size_t v; uint32_t n;
        if (member || !rdna4s_value(s, p, 0, &a->size) ||
            !rdna4s_value(s, p, 1, &a->align) || !a->size || !a->align ||
            !rdna4s_string(s, rdna4s_field(s, p, 3), a->structName, sizeof a->structName) ||
            !a->structName[0] || !rdna4s_vector(s, rdna4s_field(s, p, 4), &v, &n) ||
            !n || n > 16) return 0;
        a->dataType = RDNA4S_DT_STRUCT;
        a->memberCount = n;
        for (uint32_t i = 0; i < n; ++i) {
            uint32_t mn, tn, offset;
            if (!rdna4s_u32_at(s, v + 4 + 4 * i, &mn) || mn >= nvec || tags[mn] != 27) return 0;
            size_t mp = pay[mn];
            rdna4_stitch_arg m = {0};
            if (!rdna4s_value(s, mp, 0, &offset) ||
                !rdna4s_value(s, mp, 2, &tn) || !rdna4s_type(s, tags, pay, nvec, tn, &m, 1) ||
                !rdna4s_string(s, rdna4s_field(s, mp, 3), a->members[i].name, sizeof a->members[i].name) ||
                !a->members[i].name[0] || offset > a->size || m.size > a->size - offset) return 0;
            a->members[i].offset = offset;
            a->members[i].dataType = m.dataType;
            a->members[i].size = m.size;
            a->members[i].align = m.align;
        }
        return 1;
    }
    if (tag == 18 || tag == 4) {
        if (member) return 0;
        a->dataType = tag == 18 ? RDNA4S_DT_SAMPLER : RDNA4S_DT_TEXTURE;
        return tag == 18 || rdna4s_value(s, p, 3, &a->extra[0]);
    }
    if (tag == 17 || tag == 19) {
        uint32_t tn;
        if (!rdna4s_value(s, p, 0, &a->size) || !rdna4s_value(s, p, 1, &a->align) ||
            !a->size || !a->align || !rdna4s_value(s, p, 3, &tn) || tn >= nvec ||
            !rdna4s_value(s, p, 4, &width) || width < 2 || width > 4) return 0;
        fam = tags[tn];
        if (tag == 19) {
            if (!rdna4s_value(s, p, 5, &rows) || rows < 2 || rows > 4 ||
                (fam != 14 && fam != 13)) return 0;
            /* MTLDataType matrices are column-major groups of three row counts. */
            a->dataType = (fam == 14 ? 7 : 20) + (width - 2) * 3 + rows - 2;
            return 1;
        }
    }
    static const struct { uint8_t tag; uint32_t dt, bytes; } scalars[] = {
        {7, RDNA4S_DT_INT, 4}, {8, RDNA4S_DT_UINT, 4}, {5, RDNA4S_DT_SHORT, 2},
        {3, RDNA4S_DT_CHAR, 1}, {2, RDNA4S_DT_BOOL, 1},
        {13, RDNA4S_DT_HALF, 2}, {14, RDNA4S_DT_FLOAT, 4}
    };
    for (unsigned i = 0; i < sizeof scalars / sizeof scalars[0]; ++i) {
        if (scalars[i].tag != fam) continue;
        a->dataType = scalars[i].dt + width - 1;
        if (width == 1) a->size = a->align = scalars[i].bytes;
        return 1;
    }
    return 0;
}

static int rdna4_stitch_parse_airr(const uint8_t *buf, size_t len,
                                 const char *function_name,
                                 rdna4_stitch_arg **args_out) {
    if (!args_out) return -1;
    *args_out = NULL;
    if (!buf || len < 16 || memcmp(buf + 4, "AIRR", 4)) return -1;
    rdna4s_slice s = { buf, len };
    uint32_t root = rdna4s_u32(buf), nvec;
    size_t vec;
    if (!rdna4s_vector(&s, rdna4s_field(&s, root, 1), &vec, &nvec) ||
        !nvec || nvec > 4096) return -1;

    rdna4_stitch_arg *out = NULL;
    uint8_t *tags = calloc(nvec, 1);
    size_t *pay = calloc(nvec, sizeof *pay);
    if (!tags || !pay) { free(tags); free(pay); return -1; }
    for (uint32_t i = 0; i < nvec; ++i) {
        size_t node = rdna4s_indirect(&s, vec + 4 + 4 * i);
        size_t f0 = rdna4s_field(&s, node, 0);
        if (!node || !f0 || f0 >= len) RDNA4S_FAIL(__LINE__);
        tags[i] = buf[f0];
        pay[i] = rdna4s_indirect(&s, rdna4s_field(&s, node, 1));
        if (!pay[i]) RDNA4S_FAIL(__LINE__);
    }

    size_t vv; uint32_t nv, vf = nvec;
    if (!rdna4s_vector(&s, rdna4s_field(&s, root, 6), &vv, &nv) || !nv) RDNA4S_FAIL(__LINE__);
    for (uint32_t i = 0; i < nv; ++i) {
        uint32_t candidate = rdna4s_u32(buf + vv + 4 + 4 * i);
        char name[96];
        if (candidate >= nvec || tags[candidate] != 4 ||
            !rdna4s_string(&s, rdna4s_field(&s, pay[candidate], 0), name, sizeof name)) RDNA4S_FAIL(__LINE__);
        if (!function_name || !strcmp(name, function_name)) { vf = candidate; break; }
    }
    if (vf == nvec) RDNA4S_FAIL(__LINE__);

    /* Ordinary [[visible]] functions carry a name but no stitching signature.
     * Apple emits an empty MTLPSBIN, so Metal exposes nil returnType and []. */
    if (!rdna4s_field(&s, pay[vf], 1)) {
        free(tags); free(pay);
        return 0;
    }

    /* VisibleFunction.f1 is a signature table, not a vector: f0 is the
     * return NodeId and f1 points to the ordered vector of named arguments. */
    size_t sig = rdna4s_indirect(&s, rdna4s_field(&s, pay[vf], 1)), av;
    uint32_t rn, argc;
    if (!sig || !rdna4s_value(&s, sig, 0, &rn) ||
        !rdna4s_vector(&s, rdna4s_field(&s, sig, 1), &av, &argc) || argc >= 64) RDNA4S_FAIL(__LINE__);
    out = calloc((size_t)argc + 1, sizeof *out);
    if (!out) RDNA4S_FAIL(__LINE__);
    for (uint32_t i = 0; i < argc; ++i) {
        uint32_t an = rdna4s_u32(buf + av + 4 + 4 * i), tn;
        if (an >= nvec || tags[an] != 2 || !rdna4s_value(&s, pay[an], 0, &tn) ||
            !rdna4s_type(&s, tags, pay, nvec, tn, &out[i], 0)) RDNA4S_FAIL(__LINE__);
        /* Stitched graph inputs have no name field; Apple writes empty names.
         * A present name must still be a valid bounded string. */
        size_t name = rdna4s_field(&s, pay[an], 1);
        if (name && !rdna4s_string(&s, name, out[i].name, sizeof out[i].name)) RDNA4S_FAIL(__LINE__);
    }
    if (!rdna4s_type(&s, tags, pay, nvec, rn, &out[argc], 0)) RDNA4S_FAIL(__LINE__);
    *args_out = out;
    free(tags); free(pay);
    return (int)argc + 1;
fail:
    free(tags); free(pay); free(out);
    return -1;
}

static uint8_t *rdna4_stitch_write_psbin(const rdna4_stitch_arg *args, int count,
                                       size_t *out_size) {
    if (!out_size) return NULL;
    *out_size = 0;
    if (count < 0 || count > 1024 || (count && !args)) return NULL;
    size_t names = 0, structs = 0;
    for (int i = 0; i < count; ++i) {
        if (!memchr(args[i].name, 0, sizeof args[i].name)) return NULL;
        names += strlen(args[i].name) + 1;
        if (args[i].dataType != RDNA4S_DT_STRUCT) continue;
        if (!memchr(args[i].structName, 0, sizeof args[i].structName) ||
            !args[i].structName[0] || !args[i].memberCount || args[i].memberCount > 16) return NULL;
        structs += strlen(args[i].structName) + 1 + 4;
        for (uint32_t j = 0; j < args[i].memberCount; ++j) {
            if (!memchr(args[i].members[j].name, 0, sizeof args[i].members[j].name) ||
                !args[i].members[j].name[0] || args[i].members[j].dataType == RDNA4S_DT_STRUCT) return NULL;
            structs += strlen(args[i].members[j].name) + 1 + 40;
        }
    }
    /* Header + argument records + per-arg zero u32s + 36-byte trailer. */
    size_t body = 25 + names + structs + (size_t)count * (44 + 4) + 36;
    body = (body + 7) & ~(size_t)7;
    uint8_t *b = calloc(1, body);
    if (!b) return NULL;
    size_t pos = 0;
    memcpy(b, "MTLPSBIN", 8); pos += 8;
    uint32_t v = 0x20001; memcpy(b + pos, &v, 4); pos += 4;
    v = 0; memcpy(b + pos, &v, 4); pos += 4;           /* empty library name */
    b[pos++] = 0;                                    /* flag byte */
    size_t size_off = pos; pos += 4;                 /* payload-size offset  */
    v = (uint32_t)count; memcpy(b + pos, &v, 4); pos += 4;
    for (int i = 0; i < count; ++i) {
        size_t nl = strlen(args[i].name) + 1;
        memcpy(b + pos, args[i].name, nl); pos += nl;
        v = 0x14; memcpy(b + pos, &v, 4); pos += 4;
        pos += 8;                                    /* tag&0xf!=6 extras  */
        memcpy(b + pos, &args[i].dataType, 4); pos += 4;
        /* The wire order is byte size, then byte alignment. */
        memcpy(b + pos, &args[i].size, 4); pos += 4;
        memcpy(b + pos, &args[i].align, 4); pos += 4;
        memcpy(b + pos, args[i].extra, 12); pos += 12;
        pos += 8;
        if (args[i].dataType == RDNA4S_DT_STRUCT) {
            nl = strlen(args[i].structName) + 1;
            memcpy(b + pos, args[i].structName, nl); pos += nl;
            memcpy(b + pos, &args[i].memberCount, 4); pos += 4;
            for (uint32_t j = 0; j < args[i].memberCount; ++j) {
                nl = strlen(args[i].members[j].name) + 1;
                memcpy(b + pos, args[i].members[j].name, nl); pos += nl;
                memcpy(b + pos, &args[i].members[j].offset, 4); pos += 4;
                memset(b + pos, 0xff, 4); pos += 4;
                memcpy(b + pos, &args[i].members[j].dataType, 4); pos += 4;
                memcpy(b + pos, &args[i].members[j].size, 4); pos += 4;
                memcpy(b + pos, &args[i].members[j].align, 4); pos += 4;
                memcpy(b + pos, args[i].members[j].extra, 12); pos += 12;
                pos += 8;
            }
        }
    }
    /* Native trailer: per-arg zero u32 section, then at offset storedOff:
       ff ff ff ff, 24 zeros, u64 = storedOff + 36, pad to 8-byte boundary. */
    pos += 4 * (size_t)count;
    uint32_t storedOff = (uint32_t)pos;
    memcpy(b + size_off, &storedOff, 4);
    memset(b + pos, 0xff, 4); pos += 4;
    pos += 24;
    uint64_t total = (uint64_t)storedOff + 36;
    memcpy(b + pos, &total, 8); pos += 8;
    while (pos & 7) pos++;
    *out_size = pos;
    return b;
}

/* Also used for a well-formed zero-record reply when no AIRR is available. */
static int rdna4_stitch_container(const rdna4_stitch_arg *args, int count,
                                 uint8_t **output, size_t *output_size) {
    if (!output || !output_size) return 0;
    *output = NULL; *output_size = 0;
    size_t padded_size = 0;
    uint8_t *ps = rdna4_stitch_write_psbin(args, count, &padded_size);
    if (!ps) return 0;

    /* Native compiler envelope: u32 magic, u32 header word, u64 format stamp,
     * then four u64 (offset, length) pairs. Only the first section is present.
     * Empty sections point to EOF after the first section's alignment padding.
     * The section length excludes padding: the payload trailer stores the same
     * logical length, 36 bytes past the offset at payload byte 17. */
    uint64_t payload_size = (uint64_t)rdna4s_u32(ps + 17) + 36;
    uint64_t end = 80 + padded_size;
    const uint64_t header[10] = {
        UINT64_C(0x00000008ef13c710), UINT64_C(20230418),
        80, payload_size, end, 0, end, 0, end, 0
    };
    uint8_t *reply = malloc((size_t)end);
    if (!reply) { free(ps); return 0; }
    memcpy(reply, header, sizeof header);
    memcpy(reply + sizeof header, ps, padded_size);
    free(ps);
    *output = reply;
    *output_size = (size_t)end;
    return 1;
}

static int rdna4_stitch_reflection(const uint8_t *airr, size_t airr_size,
                                 const char *function_name,
                                 uint8_t **output, size_t *output_size) {
    if (!output || !output_size) return 0;
    *output = NULL; *output_size = 0;
    rdna4_stitch_arg *args = NULL;
    int count = rdna4_stitch_parse_airr(airr, airr_size, function_name, &args);
    if (count < 0) return 0;
    int ok = rdna4_stitch_container(args, count, output, output_size);
    free(args);
    return ok;
}

#ifdef __cplusplus
}
#endif
#endif /* RDNA4_STITCH_REFLECTION_H */
