// garnet_rt_runtime.h: pure-C Metal 3 hardware ray tracing runtime helpers for GarnetMetal.
// Covers 0077 JSON reflection parsing ("acceleration_structures" & "intersection_function_tables"),
// IFT storage-buffer ABI encoding (`word[0]=slot_count, word[1+slot]=handle_id`) + opaque
// triangle/curve signature handling + dispatch simulation, AS direct descriptor-binding vs
// argument-buffer 64-bit BDA (`u64_device_address`) planning, AIR intersection kind inference,
// and honest capability / Footnote 10 gating.
// No Metal, no Foundation, no mandatory Vulkan headers — shared by GarnetDevice.m and host C tests.
#ifndef RDNA4_RT_RUNTIME_H
#define RDNA4_RT_RUNTIME_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// =================================================================================================
// 1. Constants, ABI mirrors (0077 garnetxlate / Metal 3 / Vulkan KHR), and status codes
// =================================================================================================

// metal2vulkan buffer binding band (BUFFER_BINDING_BASE = 0, MAX_BUFFER_SLOTS = 31).
#define RDNA4RTR_BUFFER_BINDING_BASE    UINT32_C(0)
#define RDNA4RTR_MAX_BUFFER_SLOTS       UINT32_C(31)
#define RDNA4RTR_MAX_AS_BINDINGS        UINT32_C(64)
#define RDNA4RTR_MAX_IFT_TABLES         UINT32_C(31)
#define RDNA4RTR_MAX_IFT_HANDLES        UINT32_C(1024)
#define RDNA4RTR_MAX_IFT_SLOTS          UINT32_C(65536)
#define RDNA4RTR_MAX_IFT_SYMBOL_LEN     UINT32_C(4096)
#define RDNA4RTR_MAX_ATTACH_BUFFERS     UINT32_C(31)
#define RDNA4RTR_MAX_NESTED_VFTS        UINT32_C(31)
#define RDNA4RTR_JSON_MAX_DEPTH         UINT32_C(64)

// Exact 0077 garnetxlate.h RDNA4xIntersectionFunction::kind constants.
#define RDNA4RTR_IFT_KIND_BOUNDING_BOX      UINT32_C(0)
#define RDNA4RTR_IFT_KIND_TRIANGLE_ANY_HIT  UINT32_C(1)

#ifndef GARNETX_CAP_RT_POSITION_FETCH
#define GARNETX_CAP_RT_POSITION_FETCH       (1u << 9)
#endif
#ifndef GARNETX_CAP_RT_IFT_ABI_V2
#define GARNETX_CAP_RT_IFT_ABI_V2           (1u << 10)
#endif
#ifndef GARNETX_CAP_RT_PRIMITIVE_DATA
#define GARNETX_CAP_RT_PRIMITIVE_DATA       (1u << 11)
#endif

// Exact Wave 4.1 RT primitiveDataBuffer lookup table ABI (shared bit-for-bit with garnetxlate).
#define RDNA4RT_PRIM_DATA_BINDING            UINT32_C(676)
#define RDNA4RT_PRIM_DATA_STAGE_TABLE_BYTES  UINT32_C(264)
#define RDNA4RT_PRIM_TABLE_CAP               UINT32_C(4096)
#define RDNA4RT_PRIM_TABLE_MASK              UINT32_C(4095)
#define RDNA4RT_PRIM_TABLE_MAX_PROBE         UINT32_C(32)
#define RDNA4RT_PRIM_TABLE_BYTES             ((size_t)RDNA4RT_PRIM_TABLE_CAP * 32u)
#define RDNA4RT_PRIM_HASH_MULT               UINT64_C(0x61C8864680B583EB)
#define RDNA4RT_PRIM_SLOT_EMPTY              UINT64_C(0)
#define RDNA4RT_PRIM_SLOT_TOMBSTONE          UINT64_C(1)
#define RDNA4RT_PRIM_KIND_BLAS               UINT32_C(0)
#define RDNA4RT_PRIM_KIND_TLAS               UINT32_C(1)
#define RDNA4RT_PRIM_TLAS_ENTRY_STRIDE       UINT32_C(64)
#define RDNA4RT_PRIM_TLAS_REF_OFFSET         UINT32_C(56)

_Static_assert(RDNA4RT_PRIM_HASH_MULT == UINT64_C(7046029254386353131),
               "RT prim hash multiplier must match garnetxlate decimal constant");
_Static_assert((RDNA4RT_PRIM_TABLE_CAP & (RDNA4RT_PRIM_TABLE_CAP - 1u)) == 0u &&
               RDNA4RT_PRIM_TABLE_MASK == RDNA4RT_PRIM_TABLE_CAP - 1u,
               "RT prim table capacity must be power-of-two");

// Exact 0077 / Wave 2 IFT buffer ABI constants:
//   ABI v1: word[0]=slot_count, word[1+slot]=handle_id
//   ABI v2: word[0]=slot_count, word[1]=2, word[2]=attached_buf_mask, word[3]=0,
//           word[4..65]=u64 attached_bda[31] (lo, hi), word[66+slot]=handle_id
#define RDNA4RTR_IFT_ABI_VERSION        UINT32_C(1)
#define RDNA4RTR_IFT_HEADER_WORDS       UINT32_C(1)
#define RDNA4RTR_IFT_ABI_VERSION_V1     UINT32_C(1)
#define RDNA4RTR_IFT_HEADER_WORDS_V1    UINT32_C(1)
#define RDNA4RTR_IFT_ABI_VERSION_V2     UINT32_C(2)
#define RDNA4RTR_IFT_HEADER_WORDS_V2    UINT32_C(66)
#define RDNA4RTR_IFT_V2_ATTACH_WORD_BASE UINT32_C(4)
#define RDNA4RTR_IFT_HANDLE_NULL        UINT32_C(0)

// Exact 0077 acceleration structure BDA ABI tag.
#define RDNA4RTR_BDA_ABI_U64            "u64_device_address"

// SPIR-V & Vulkan KHR ray tracing constants (pinned against <vulkan/vulkan.h> when present).
#define RDNA4RTR_SPV_OP_TYPE_ACCEL_KHR  UINT32_C(5341)
#define RDNA4RTR_SPV_CAP_RAY_QUERY_KHR  UINT32_C(4472)
#define RDNA4RTR_VK_DESC_TYPE_AS        UINT32_C(1000150000)
#define RDNA4RTR_VK_STYPE_WRITE_DESC_AS UINT32_C(1000150007)

#if defined(VK_KHR_acceleration_structure) || defined(VK_VERSION_1_0)
_Static_assert(RDNA4RTR_VK_DESC_TYPE_AS == (uint32_t)VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
               "VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR mismatch");
_Static_assert(RDNA4RTR_VK_STYPE_WRITE_DESC_AS == (uint32_t)VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
               "VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR mismatch");
_Static_assert(sizeof(VkAccelerationStructureKHR) == sizeof(uint64_t),
               "VkAccelerationStructureKHR must fit in uint64_t");
_Static_assert(sizeof(VkDeviceAddress) == sizeof(uint64_t),
               "VkDeviceAddress must fit in uint64_t");
#endif

// Metal MTLIntersectionFunctionSignature bitmask mirrors (macOS 11/12 bits pinned in ObjC).
#define RDNA4RTR_SIG_NONE               UINT64_C(0)
#define RDNA4RTR_SIG_INSTANCING         (UINT64_C(1) << 0)
#define RDNA4RTR_SIG_TRIANGLE_DATA      (UINT64_C(1) << 1)
#define RDNA4RTR_SIG_WORLD_SPACE_DATA   (UINT64_C(1) << 2)
#define RDNA4RTR_SIG_INSTANCE_MOTION    (UINT64_C(1) << 3)
#define RDNA4RTR_SIG_PRIMITIVE_MOTION   (UINT64_C(1) << 4)
#define RDNA4RTR_SIG_EXTENDED_LIMITS    (UINT64_C(1) << 5)
#define RDNA4RTR_SIG_MAX_LEVELS         (UINT64_C(1) << 6)
#define RDNA4RTR_SIG_CURVE_DATA         (UINT64_C(1) << 7)
#define RDNA4RTR_SIG_KNOWN_MASK         ((UINT64_C(1) << 8) - UINT64_C(1))
#define RDNA4RTR_SIG_MOTION_MASK        (RDNA4RTR_SIG_INSTANCE_MOTION | RDNA4RTR_SIG_PRIMITIVE_MOTION)

// Stage indices matching GarnetMetal descriptor set layout: compute=set 0, vertex=set 0, fragment=set 1.
typedef enum {
    RDNA4RTR_STAGE_COMPUTE  = 0,
    RDNA4RTR_STAGE_VERTEX   = 1,
    RDNA4RTR_STAGE_FRAGMENT = 2,
} rdna4rtr_stage_t;

// Mirrors rdna4rt_kind_t (0=unknown, 1=primitive/BLAS, 2=instance/TLAS).
typedef enum {
    RDNA4RTR_AS_KIND_UNKNOWN   = 0,
    RDNA4RTR_AS_KIND_PRIMITIVE = 1,
    RDNA4RTR_AS_KIND_INSTANCE  = 2,
} rdna4rtr_as_kind_t;

typedef enum {
    RDNA4RTR_AS_BIND_UNKNOWN    = 0,
    RDNA4RTR_AS_BIND_DESCRIPTOR = 1,
    RDNA4RTR_AS_BIND_BDA        = 2,
} rdna4rtr_as_bind_mode_t;

typedef enum {
    RDNA4RTR_ARG_KIND_NONE         = 0,
    RDNA4RTR_ARG_KIND_PRIMITIVE_AS = 1,
    RDNA4RTR_ARG_KIND_INSTANCE_AS  = 2,
    RDNA4RTR_ARG_KIND_IFT          = 3,
} rdna4rtr_arg_kind_t;

typedef enum {
    RDNA4RTR_OK = 0,
    RDNA4RTR_ERR_INVALID,
    RDNA4RTR_ERR_JSON,
    RDNA4RTR_ERR_ABI_MISMATCH,
    RDNA4RTR_ERR_BOUNDS,
    RDNA4RTR_ERR_DUPLICATE,
    RDNA4RTR_ERR_UNBUILT,
    RDNA4RTR_ERR_KIND_MISMATCH,
    RDNA4RTR_ERR_NULL_HANDLE,
    RDNA4RTR_ERR_UNSUPPORTED,
    RDNA4RTR_ERR_ALIGNMENT,
    RDNA4RTR_ERR_FOOTNOTE10,
    RDNA4RTR_ERR_OOM,
} rdna4rtr_status_t;

static inline const char *rdna4rtr_status_str(rdna4rtr_status_t st) {
    switch (st) {
    case RDNA4RTR_OK:                return "ok";
    case RDNA4RTR_ERR_INVALID:       return "invalid ray tracing runtime argument";
    case RDNA4RTR_ERR_JSON:          return "malformed ray tracing reflection JSON";
    case RDNA4RTR_ERR_ABI_MISMATCH:  return "ray tracing reflection ABI mismatch";
    case RDNA4RTR_ERR_BOUNDS:        return "ray tracing index, count, or offset out of bounds";
    case RDNA4RTR_ERR_DUPLICATE:     return "duplicate ray tracing binding or IFT handle";
    case RDNA4RTR_ERR_UNBUILT:       return "acceleration structure has not been built";
    case RDNA4RTR_ERR_KIND_MISMATCH: return "acceleration structure kind mismatch (BLAS vs TLAS)";
    case RDNA4RTR_ERR_NULL_HANDLE:   return "acceleration structure or table GPU handle/address is null";
    case RDNA4RTR_ERR_UNSUPPORTED:   return "ray tracing feature unsupported on this device";
    case RDNA4RTR_ERR_ALIGNMENT:     return "ray tracing argument buffer offset is not 8-byte aligned";
    case RDNA4RTR_ERR_FOOTNOTE10:    return "render-stage ray tracing cannot be combined with mesh/object shaders (Footnote 10)";
    case RDNA4RTR_ERR_OOM:           return "out of memory";
    }
    return "unknown ray tracing runtime error";
}

// =================================================================================================
// 2. Honest Capability & Footnote 10 Helpers
// =================================================================================================

typedef struct {
    uint8_t accel_struct_supported;   // VK_KHR_acceleration_structure
    uint8_t ray_query_supported;      // VK_KHR_ray_query
    uint8_t buffer_device_address;    // VK_KHR_buffer_device_address
    uint8_t deferred_host_ops;        // VK_KHR_deferred_host_operations
    uint8_t translator_stage3_ready;  // in-process garnetx_translate_stage3 available
    uint8_t motion_blur_ext_supported;// VK_NV_ray_tracing_motion_blur (0 on RADV GFX12)
    uint8_t curve_geometry_supported; // Metal 3.1 curve RT (0 on RADV GFX12)
} rdna4rtr_hw_caps_t;

static inline int rdna4rtr_supports_raytracing(const rdna4rtr_hw_caps_t *c) {
    if (!c) return 0;
    return c->accel_struct_supported &&
           c->ray_query_supported &&
           c->buffer_device_address &&
           c->deferred_host_ops &&
           c->translator_stage3_ready;
}

static inline int rdna4rtr_supports_raytracing_from_render(const rdna4rtr_hw_caps_t *c) {
    // RADV GFX12 supports VK_KHR_ray_query uniformly in compute, vertex, and fragment stages.
    return rdna4rtr_supports_raytracing(c);
}

static inline int rdna4rtr_supports_primitive_motion_blur(const rdna4rtr_hw_caps_t *c) {
    if (!rdna4rtr_supports_raytracing(c)) return 0;
    return c->motion_blur_ext_supported != 0u;
}

static inline int rdna4rtr_supports_curve_intersections(const rdna4rtr_hw_caps_t *c) {
    if (!rdna4rtr_supports_raytracing(c)) return 0;
    return c->curve_geometry_supported != 0u;
}

static inline rdna4rtr_status_t rdna4rtr_mesh_raytracing_conflict(int has_mesh_or_object,
                                                                  int uses_render_rt) {
    return (has_mesh_or_object && uses_render_rt) ? RDNA4RTR_ERR_FOOTNOTE10 : RDNA4RTR_OK;
}

static inline uint32_t rdna4rtr_stage_descriptor_set(rdna4rtr_stage_t stage) {
    return (stage == RDNA4RTR_STAGE_FRAGMENT) ? 1u : 0u;
}

static inline int rdna4rtr_binding_to_metal_index(uint32_t binding,
                                                  uint32_t count,
                                                  uint32_t *metal_idx_out) {
    if (count == 0u) return 0;
    uint32_t delta = binding - RDNA4RTR_BUFFER_BINDING_BASE;
    if (delta >= RDNA4RTR_MAX_BUFFER_SLOTS || count > RDNA4RTR_MAX_BUFFER_SLOTS - delta)
        return 0;
    if (metal_idx_out) *metal_idx_out = delta;
    return 1;
}

static inline void rdna4rtr_pack_u64_le(uint8_t dst[8], uint64_t val) {
    dst[0] = (uint8_t)(val & 0xFFu);
    dst[1] = (uint8_t)((val >> 8) & 0xFFu);
    dst[2] = (uint8_t)((val >> 16) & 0xFFu);
    dst[3] = (uint8_t)((val >> 24) & 0xFFu);
    dst[4] = (uint8_t)((val >> 32) & 0xFFu);
    dst[5] = (uint8_t)((val >> 40) & 0xFFu);
    dst[6] = (uint8_t)((val >> 48) & 0xFFu);
    dst[7] = (uint8_t)((val >> 56) & 0xFFu);
}

static inline uint64_t rdna4rtr_unpack_u64_le(const uint8_t src[8]) {
    return ((uint64_t)src[0]) |
           ((uint64_t)src[1] << 8) |
           ((uint64_t)src[2] << 16) |
           ((uint64_t)src[3] << 24) |
           ((uint64_t)src[4] << 32) |
           ((uint64_t)src[5] << 40) |
           ((uint64_t)src[6] << 48) |
           ((uint64_t)src[7] << 56);
}

// =================================================================================================
// 3. Minimal Strict JSON Scanner (bounds-checked, depth-guarded, zero namespace collisions)
// =================================================================================================

typedef struct {
    const char *s;
    size_t len;
    size_t pos;
} rdna4rtr_jcur_t;

static inline void rdna4rtr_j_ws(rdna4rtr_jcur_t *c) {
    while (c->pos < c->len) {
        char ch = c->s[c->pos];
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') c->pos++;
        else break;
    }
}

static inline int rdna4rtr_j_peek(rdna4rtr_jcur_t *c) {
    rdna4rtr_j_ws(c);
    return (c->pos < c->len) ? (unsigned char)c->s[c->pos] : -1;
}

static inline int rdna4rtr_j_consume(rdna4rtr_jcur_t *c, char ch) {
    rdna4rtr_j_ws(c);
    if (c->pos < c->len && c->s[c->pos] == ch) {
        c->pos++;
        return 1;
    }
    return 0;
}

static inline int rdna4rtr_j_match_lit(rdna4rtr_jcur_t *c, const char *lit) {
    rdna4rtr_j_ws(c);
    size_t n = strlen(lit);
    if (c->pos + n > c->len) return 0;
    if (memcmp(c->s + c->pos, lit, n) != 0) return 0;
    if (c->pos + n < c->len) {
        unsigned char nxt = (unsigned char)c->s[c->pos + n];
        if ((nxt >= 'a' && nxt <= 'z') || (nxt >= 'A' && nxt <= 'Z') ||
            (nxt >= '0' && nxt <= '9') || nxt == '_')
            return 0;
    }
    c->pos += n;
    return 1;
}

static inline int rdna4rtr_j_hex(char ch, uint32_t *out) {
    if (ch >= '0' && ch <= '9') { *out = (uint32_t)(ch - '0'); return 1; }
    if (ch >= 'a' && ch <= 'f') { *out = (uint32_t)(10 + (ch - 'a')); return 1; }
    if (ch >= 'A' && ch <= 'F') { *out = (uint32_t)(10 + (ch - 'A')); return 1; }
    return 0;
}

static inline int rdna4rtr_j_read_string(rdna4rtr_jcur_t *c, char *buf, size_t cap, size_t *out_len) {
    rdna4rtr_j_ws(c);
    if (c->pos >= c->len || c->s[c->pos] != '"') return 0;
    c->pos++;
    size_t n = 0;
    while (c->pos < c->len) {
        unsigned char ch = (unsigned char)c->s[c->pos++];
        if (ch == '"') {
            if (buf && cap > 0) buf[n < cap ? n : cap - 1u] = '\0';
            if (out_len) *out_len = n;
            return 1;
        }
        if (ch < 0x20u) return 0;
        if (ch == '\\') {
            if (c->pos >= c->len) return 0;
            char esc = c->s[c->pos++];
            uint32_t cp = 0;
            switch (esc) {
            case '"':  cp = '"';  break;
            case '\\': cp = '\\'; break;
            case '/':  cp = '/';  break;
            case 'b':  cp = '\b'; break;
            case 'f':  cp = '\f'; break;
            case 'n':  cp = '\n'; break;
            case 'r':  cp = '\r'; break;
            case 't':  cp = '\t'; break;
            case 'u': {
                if (c->pos + 4u > c->len) return 0;
                uint32_t d0 = 0, d1 = 0, d2 = 0, d3 = 0;
                if (!rdna4rtr_j_hex(c->s[c->pos], &d0) ||
                    !rdna4rtr_j_hex(c->s[c->pos + 1u], &d1) ||
                    !rdna4rtr_j_hex(c->s[c->pos + 2u], &d2) ||
                    !rdna4rtr_j_hex(c->s[c->pos + 3u], &d3))
                    return 0;
                c->pos += 4u;
                cp = (d0 << 12) | (d1 << 8) | (d2 << 4) | d3;
                if (cp == 0u || (cp >= 0xD800u && cp <= 0xDFFFu)) return 0;
                break;
            }
            default: return 0;
            }
            if (cp < 0x80u) {
                if (buf && n + 1u < cap) buf[n] = (char)cp;
                n++;
            } else if (cp < 0x800u) {
                if (buf && n + 2u < cap) {
                    buf[n] = (char)(0xC0u | (cp >> 6));
                    buf[n + 1u] = (char)(0x80u | (cp & 0x3Fu));
                }
                n += 2u;
            } else {
                if (buf && n + 3u < cap) {
                    buf[n] = (char)(0xE0u | (cp >> 12));
                    buf[n + 1u] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
                    buf[n + 2u] = (char)(0x80u | (cp & 0x3Fu));
                }
                n += 3u;
            }
        } else {
            if (buf && n + 1u < cap) buf[n] = (char)ch;
            n++;
        }
    }
    return 0;
}

static inline int rdna4rtr_j_read_u32(rdna4rtr_jcur_t *c, uint32_t *out) {
    rdna4rtr_j_ws(c);
    if (c->pos >= c->len) return 0;
    char ch = c->s[c->pos];
    if (ch < '0' || ch > '9') return 0;
    if (ch == '0' && c->pos + 1u < c->len &&
        c->s[c->pos + 1u] >= '0' && c->s[c->pos + 1u] <= '9')
        return 0;
    uint64_t val = 0;
    while (c->pos < c->len && c->s[c->pos] >= '0' && c->s[c->pos] <= '9') {
        val = val * 10u + (uint64_t)(c->s[c->pos] - '0');
        if (val > UINT32_MAX) return 0;
        c->pos++;
    }
    if (c->pos < c->len) {
        char nxt = c->s[c->pos];
        if (nxt == '.' || nxt == 'e' || nxt == 'E') return 0;
    }
    *out = (uint32_t)val;
    return 1;
}

static inline int rdna4rtr_j_skip_val(rdna4rtr_jcur_t *c, uint32_t depth) {
    if (depth > RDNA4RTR_JSON_MAX_DEPTH) return 0;
    int ch = rdna4rtr_j_peek(c);
    if (ch < 0) return 0;
    if (ch == '"') return rdna4rtr_j_read_string(c, NULL, 0u, NULL);
    if (ch == '{') {
        c->pos++;
        if (rdna4rtr_j_consume(c, '}')) return 1;
        for (;;) {
            if (!rdna4rtr_j_read_string(c, NULL, 0u, NULL)) return 0;
            if (!rdna4rtr_j_consume(c, ':')) return 0;
            if (!rdna4rtr_j_skip_val(c, depth + 1u)) return 0;
            if (rdna4rtr_j_consume(c, '}')) return 1;
            if (!rdna4rtr_j_consume(c, ',')) return 0;
        }
    }
    if (ch == '[') {
        c->pos++;
        if (rdna4rtr_j_consume(c, ']')) return 1;
        for (;;) {
            if (!rdna4rtr_j_skip_val(c, depth + 1u)) return 0;
            if (rdna4rtr_j_consume(c, ']')) return 1;
            if (!rdna4rtr_j_consume(c, ',')) return 0;
        }
    }
    if (ch == 't') return rdna4rtr_j_match_lit(c, "true");
    if (ch == 'f') return rdna4rtr_j_match_lit(c, "false");
    if (ch == 'n') return rdna4rtr_j_match_lit(c, "null");
    if (ch == '-' || (ch >= '0' && ch <= '9')) {
        if (c->s[c->pos] == '-') c->pos++;
        if (c->pos >= c->len || c->s[c->pos] < '0' || c->s[c->pos] > '9') return 0;
        while (c->pos < c->len && c->s[c->pos] >= '0' && c->s[c->pos] <= '9') c->pos++;
        if (c->pos < c->len && c->s[c->pos] == '.') {
            c->pos++;
            if (c->pos >= c->len || c->s[c->pos] < '0' || c->s[c->pos] > '9') return 0;
            while (c->pos < c->len && c->s[c->pos] >= '0' && c->s[c->pos] <= '9') c->pos++;
        }
        if (c->pos < c->len && (c->s[c->pos] == 'e' || c->s[c->pos] == 'E')) {
            c->pos++;
            if (c->pos < c->len && (c->s[c->pos] == '+' || c->s[c->pos] == '-')) c->pos++;
            if (c->pos >= c->len || c->s[c->pos] < '0' || c->s[c->pos] > '9') return 0;
            while (c->pos < c->len && c->s[c->pos] >= '0' && c->s[c->pos] <= '9') c->pos++;
        }
        return 1;
    }
    return 0;
}

static inline int rdna4rtr_j_parse_desc_obj(rdna4rtr_jcur_t *c,
                                            uint32_t *set_out,
                                            uint32_t *binding_out,
                                            uint32_t *count_out) {
    if (!rdna4rtr_j_consume(c, '{')) return 0;
    int have_set = 0, have_binding = 0, have_count = 0;
    uint32_t set_v = 0, bind_v = 0, count_v = 0;
    if (!rdna4rtr_j_consume(c, '}')) {
        for (;;) {
            char key[64];
            size_t klen = 0;
            if (!rdna4rtr_j_read_string(c, key, sizeof key, &klen)) return 0;
            if (!rdna4rtr_j_consume(c, ':')) return 0;
            if (klen < sizeof key && strcmp(key, "set") == 0) {
                if (!rdna4rtr_j_read_u32(c, &set_v)) return 0;
                have_set = 1;
            } else if (klen < sizeof key && strcmp(key, "binding") == 0) {
                if (!rdna4rtr_j_read_u32(c, &bind_v)) return 0;
                have_binding = 1;
            } else if (klen < sizeof key && strcmp(key, "count") == 0) {
                if (!rdna4rtr_j_read_u32(c, &count_v)) return 0;
                have_count = 1;
            } else {
                if (!rdna4rtr_j_skip_val(c, 1u)) return 0;
            }
            if (rdna4rtr_j_consume(c, '}')) break;
            if (!rdna4rtr_j_consume(c, ',')) return 0;
        }
    }
    if (!have_set || !have_binding || !have_count || count_v == 0u) return 0;
    *set_out = set_v;
    *binding_out = bind_v;
    *count_out = count_v;
    return 1;
}

// =================================================================================================
// 4. 0077 JSON Reflection Parsers ("acceleration_structures" & "intersection_function_tables")
// =================================================================================================

typedef struct {
    uint8_t has_param_index;
    uint32_t param_index;
    uint32_t metal_index;          // [[buffer(n)]] for Descriptor; [[id(n)]] for BDA
    rdna4rtr_as_kind_t kind;       // PRIMITIVE or INSTANCE
    rdna4rtr_as_bind_mode_t mode;  // DESCRIPTOR or BDA
    uint8_t has_descriptor;
    uint32_t set;
    uint32_t binding;
    uint32_t count;
    uint8_t has_argbuf_index;
    uint32_t argbuf_index;         // argument buffer [[buffer(m)]] for BDA
    uint8_t has_byte_offset;
    uint32_t byte_offset;          // 8-byte aligned byte offset in argument buffer
} rdna4rtr_as_binding_t;

typedef struct {
    uint8_t present;               // 1 if non-empty "acceleration_structures" array was parsed
    uint32_t count;
    uint32_t descriptor_count;
    uint32_t bda_count;
    rdna4rtr_as_binding_t entries[RDNA4RTR_MAX_AS_BINDINGS];
} rdna4rtr_as_reflection_t;

typedef struct {
    uint32_t param_index;
    uint32_t metal_index;
    uint8_t has_descriptor;
    uint32_t set;
    uint32_t binding;
    uint32_t count;
} rdna4rtr_ift_table_binding_t;

typedef struct {
    uint32_t handle_id;            // 1..N (0 reserved for null/default)
    uint32_t kind;                 // RDNA4RTR_IFT_KIND_BOUNDING_BOX or TRIANGLE_ANY_HIT
    char *symbol;                  // heap-allocated NUL-terminated UTF-8 symbol name
} rdna4rtr_ift_handle_entry_t;

typedef struct {
    uint8_t present;               // 1 if non-null "intersection_function_tables" object was parsed
    uint32_t abi_version;
    uint32_t header_words;
    uint32_t table_count;
    rdna4rtr_ift_table_binding_t tables[RDNA4RTR_MAX_IFT_TABLES];
    rdna4rtr_ift_handle_entry_t *handles;
    uint32_t handle_count;
    uint32_t max_handle_id;
} rdna4rtr_ift_reflection_t;

typedef struct {
    rdna4rtr_as_reflection_t as;
    rdna4rtr_ift_reflection_t ift;
} rdna4rtr_reflection_t;

static inline void rdna4rtr_ift_refl_destroy(rdna4rtr_ift_reflection_t *r) {
    if (!r) return;
    if (r->handles) {
        for (uint32_t i = 0; i < r->handle_count; i++) free(r->handles[i].symbol);
        free(r->handles);
    }
    memset(r, 0, sizeof *r);
}

static inline void rdna4rtr_refl_destroy(rdna4rtr_reflection_t *r) {
    if (!r) return;
    rdna4rtr_ift_refl_destroy(&r->ift);
    memset(&r->as, 0, sizeof r->as);
}

static inline int rdna4rtr_refl_uses_raytracing(const rdna4rtr_reflection_t *r) {
    return r && (r->as.present || r->ift.present);
}

static inline const rdna4rtr_as_binding_t *rdna4rtr_as_find_descriptor(
    const rdna4rtr_as_reflection_t *r, uint32_t metal_index) {
    if (!r || !r->present) return NULL;
    for (uint32_t i = 0; i < r->count; i++) {
        if (r->entries[i].mode == RDNA4RTR_AS_BIND_DESCRIPTOR &&
            r->entries[i].metal_index == metal_index)
            return &r->entries[i];
    }
    return NULL;
}

static inline const rdna4rtr_as_binding_t *rdna4rtr_as_find_by_binding(
    const rdna4rtr_as_reflection_t *r, uint32_t set, uint32_t binding) {
    if (!r || !r->present) return NULL;
    for (uint32_t i = 0; i < r->count; i++) {
        if (r->entries[i].mode == RDNA4RTR_AS_BIND_DESCRIPTOR &&
            r->entries[i].has_descriptor &&
            r->entries[i].set == set &&
            r->entries[i].binding == binding)
            return &r->entries[i];
    }
    return NULL;
}

static inline uint32_t rdna4rtr_ift_refl_find_handle(const rdna4rtr_ift_reflection_t *r,
                                                     const char *symbol,
                                                     uint32_t *kind_out) {
    if (kind_out) *kind_out = RDNA4RTR_IFT_KIND_BOUNDING_BOX;
    if (!r || !r->present || !symbol || !symbol[0]) return RDNA4RTR_IFT_HANDLE_NULL;
    for (uint32_t i = 0; i < r->handle_count; i++) {
        if (r->handles[i].symbol && strcmp(r->handles[i].symbol, symbol) == 0) {
            if (kind_out) *kind_out = r->handles[i].kind;
            return r->handles[i].handle_id;
        }
    }
    return RDNA4RTR_IFT_HANDLE_NULL;
}

static inline const rdna4rtr_ift_handle_entry_t *rdna4rtr_ift_refl_find_by_id(
    const rdna4rtr_ift_reflection_t *r, uint32_t handle_id) {
    if (!r || !r->present || handle_id == RDNA4RTR_IFT_HANDLE_NULL) return NULL;
    for (uint32_t i = 0; i < r->handle_count; i++) {
        if (r->handles[i].handle_id == handle_id) return &r->handles[i];
    }
    return NULL;
}

static inline int rdna4rtr_ift_refl_has_table_at_buffer(const rdna4rtr_ift_reflection_t *r,
                                                        uint32_t metal_index) {
    if (!r || !r->present) return 0;
    for (uint32_t i = 0; i < r->table_count; i++) {
        if (r->tables[i].metal_index == metal_index) return 1;
    }
    return 0;
}

// Parses the value of "acceleration_structures" at cursor c (null or array).
static inline rdna4rtr_status_t rdna4rtr_parse_as_array_cur(rdna4rtr_jcur_t *c,
                                                            rdna4rtr_as_reflection_t *out) {
    memset(out, 0, sizeof *out);
    if (rdna4rtr_j_peek(c) == 'n') {
        return rdna4rtr_j_match_lit(c, "null") ? RDNA4RTR_OK : RDNA4RTR_ERR_JSON;
    }
    if (!rdna4rtr_j_consume(c, '[')) return RDNA4RTR_ERR_JSON;
    if (rdna4rtr_j_consume(c, ']')) return RDNA4RTR_OK;

    for (;;) {
        if (out->count >= RDNA4RTR_MAX_AS_BINDINGS) return RDNA4RTR_ERR_BOUNDS;
        if (!rdna4rtr_j_consume(c, '{')) return RDNA4RTR_ERR_JSON;

        rdna4rtr_as_binding_t ent;
        memset(&ent, 0, sizeof ent);
        int have_kind = 0, have_mode = 0, have_metal = 0;
        int have_bda_abi = 0, bda_abi_ok = 0;

        if (!rdna4rtr_j_consume(c, '}')) {
            for (;;) {
                char key[64];
                size_t klen = 0;
                if (!rdna4rtr_j_read_string(c, key, sizeof key, &klen)) return RDNA4RTR_ERR_JSON;
                if (!rdna4rtr_j_consume(c, ':')) return RDNA4RTR_ERR_JSON;

                if (klen < sizeof key && strcmp(key, "kind") == 0) {
                    char val[32];
                    size_t vlen = 0;
                    if (!rdna4rtr_j_read_string(c, val, sizeof val, &vlen) || vlen >= sizeof val)
                        return RDNA4RTR_ERR_JSON;
                    if (strcmp(val, "Instance") == 0) ent.kind = RDNA4RTR_AS_KIND_INSTANCE;
                    else if (strcmp(val, "Primitive") == 0) ent.kind = RDNA4RTR_AS_KIND_PRIMITIVE;
                    else return RDNA4RTR_ERR_JSON;
                    have_kind = 1;
                } else if (klen < sizeof key && strcmp(key, "binding_mode") == 0) {
                    char val[32];
                    size_t vlen = 0;
                    if (!rdna4rtr_j_read_string(c, val, sizeof val, &vlen) || vlen >= sizeof val)
                        return RDNA4RTR_ERR_JSON;
                    if (strcmp(val, "Descriptor") == 0) ent.mode = RDNA4RTR_AS_BIND_DESCRIPTOR;
                    else if (strcmp(val, "BufferDeviceAddress") == 0) ent.mode = RDNA4RTR_AS_BIND_BDA;
                    else return RDNA4RTR_ERR_JSON;
                    have_mode = 1;
                } else if (klen < sizeof key && strcmp(key, "param_index") == 0) {
                    if (rdna4rtr_j_peek(c) == 'n') {
                        if (!rdna4rtr_j_match_lit(c, "null")) return RDNA4RTR_ERR_JSON;
                    } else {
                        if (!rdna4rtr_j_read_u32(c, &ent.param_index)) return RDNA4RTR_ERR_JSON;
                        ent.has_param_index = 1u;
                    }
                } else if (klen < sizeof key && strcmp(key, "metal_index") == 0) {
                    if (!rdna4rtr_j_read_u32(c, &ent.metal_index)) return RDNA4RTR_ERR_JSON;
                    have_metal = 1;
                } else if (klen < sizeof key && strcmp(key, "descriptor") == 0) {
                    if (rdna4rtr_j_peek(c) == 'n') {
                        if (!rdna4rtr_j_match_lit(c, "null")) return RDNA4RTR_ERR_JSON;
                    } else {
                        if (!rdna4rtr_j_parse_desc_obj(c, &ent.set, &ent.binding, &ent.count))
                            return RDNA4RTR_ERR_JSON;
                        ent.has_descriptor = 1u;
                    }
                } else if (klen < sizeof key && strcmp(key, "argument_buffer_index") == 0) {
                    if (rdna4rtr_j_peek(c) == 'n') {
                        if (!rdna4rtr_j_match_lit(c, "null")) return RDNA4RTR_ERR_JSON;
                    } else {
                        if (!rdna4rtr_j_read_u32(c, &ent.argbuf_index)) return RDNA4RTR_ERR_JSON;
                        ent.has_argbuf_index = 1u;
                    }
                } else if (klen < sizeof key && strcmp(key, "byte_offset") == 0) {
                    if (rdna4rtr_j_peek(c) == 'n') {
                        if (!rdna4rtr_j_match_lit(c, "null")) return RDNA4RTR_ERR_JSON;
                    } else {
                        if (!rdna4rtr_j_read_u32(c, &ent.byte_offset)) return RDNA4RTR_ERR_JSON;
                        ent.has_byte_offset = 1u;
                    }
                } else if (klen < sizeof key && strcmp(key, "bda_abi") == 0) {
                    if (rdna4rtr_j_peek(c) == 'n') {
                        if (!rdna4rtr_j_match_lit(c, "null")) return RDNA4RTR_ERR_JSON;
                    } else {
                        char val[48];
                        size_t vlen = 0;
                        if (!rdna4rtr_j_read_string(c, val, sizeof val, &vlen) || vlen >= sizeof val)
                            return RDNA4RTR_ERR_JSON;
                        have_bda_abi = 1;
                        bda_abi_ok = (strcmp(val, RDNA4RTR_BDA_ABI_U64) == 0);
                    }
                } else {
                    if (!rdna4rtr_j_skip_val(c, 1u)) return RDNA4RTR_ERR_JSON;
                }

                if (rdna4rtr_j_consume(c, '}')) break;
                if (!rdna4rtr_j_consume(c, ',')) return RDNA4RTR_ERR_JSON;
            }
        }

        if (!have_kind || !have_mode || !have_metal) return RDNA4RTR_ERR_JSON;

        if (ent.mode == RDNA4RTR_AS_BIND_DESCRIPTOR) {
            if (ent.metal_index >= RDNA4RTR_MAX_BUFFER_SLOTS) return RDNA4RTR_ERR_BOUNDS;
            if (!ent.has_descriptor || have_bda_abi) return RDNA4RTR_ERR_ABI_MISMATCH;
            if (ent.binding != RDNA4RTR_BUFFER_BINDING_BASE + ent.metal_index)
                return RDNA4RTR_ERR_ABI_MISMATCH;
            for (uint32_t i = 0; i < out->count; i++) {
                const rdna4rtr_as_binding_t *prev = &out->entries[i];
                if (prev->mode == RDNA4RTR_AS_BIND_DESCRIPTOR &&
                    (prev->metal_index == ent.metal_index ||
                     (prev->set == ent.set && prev->binding == ent.binding)))
                    return RDNA4RTR_ERR_DUPLICATE;
            }
            out->descriptor_count++;
        } else {
            // BufferDeviceAddress mode
            if (!have_bda_abi || !bda_abi_ok || ent.has_descriptor) return RDNA4RTR_ERR_ABI_MISMATCH;
            if (ent.has_argbuf_index && ent.argbuf_index >= RDNA4RTR_MAX_BUFFER_SLOTS)
                return RDNA4RTR_ERR_BOUNDS;
            if (ent.has_byte_offset && (ent.byte_offset & 7u) != 0u)
                return RDNA4RTR_ERR_ALIGNMENT;
            if (ent.has_argbuf_index && ent.has_byte_offset) {
                for (uint32_t i = 0; i < out->count; i++) {
                    const rdna4rtr_as_binding_t *prev = &out->entries[i];
                    if (prev->mode == RDNA4RTR_AS_BIND_BDA &&
                        prev->has_argbuf_index && prev->has_byte_offset &&
                        prev->argbuf_index == ent.argbuf_index &&
                        prev->byte_offset == ent.byte_offset)
                        return RDNA4RTR_ERR_DUPLICATE;
                }
            }
            out->bda_count++;
        }

        out->entries[out->count++] = ent;
        if (rdna4rtr_j_consume(c, ']')) break;
        if (!rdna4rtr_j_consume(c, ',')) return RDNA4RTR_ERR_JSON;
    }

    out->present = (out->count > 0u) ? 1u : 0u;
    return RDNA4RTR_OK;
}

// Parses the value of "intersection_function_tables" at cursor c (null or object).
static inline rdna4rtr_status_t rdna4rtr_parse_ift_obj_cur(rdna4rtr_jcur_t *c,
                                                           rdna4rtr_ift_reflection_t *out) {
    memset(out, 0, sizeof *out);
    if (rdna4rtr_j_peek(c) == 'n') {
        return rdna4rtr_j_match_lit(c, "null") ? RDNA4RTR_OK : RDNA4RTR_ERR_JSON;
    }
    if (!rdna4rtr_j_consume(c, '{')) return RDNA4RTR_ERR_JSON;

    int have_abi = 0, have_hdr = 0, have_tables = 0, have_handles = 0;
    rdna4rtr_status_t st = RDNA4RTR_OK;
    uint32_t handle_cap = 0;

    if (!rdna4rtr_j_consume(c, '}')) {
        for (;;) {
            char key[64];
            size_t klen = 0;
            if (!rdna4rtr_j_read_string(c, key, sizeof key, &klen)) { st = RDNA4RTR_ERR_JSON; goto fail; }
            if (!rdna4rtr_j_consume(c, ':')) { st = RDNA4RTR_ERR_JSON; goto fail; }

            if (klen < sizeof key && strcmp(key, "abi_version") == 0) {
                if (!rdna4rtr_j_read_u32(c, &out->abi_version)) { st = RDNA4RTR_ERR_JSON; goto fail; }
                have_abi = 1;
            } else if (klen < sizeof key && strcmp(key, "header_words") == 0) {
                if (!rdna4rtr_j_read_u32(c, &out->header_words)) { st = RDNA4RTR_ERR_JSON; goto fail; }
                have_hdr = 1;
            } else if (klen < sizeof key && strcmp(key, "tables") == 0) {
                if (!rdna4rtr_j_consume(c, '[')) { st = RDNA4RTR_ERR_JSON; goto fail; }
                if (!rdna4rtr_j_consume(c, ']')) {
                    for (;;) {
                        if (out->table_count >= RDNA4RTR_MAX_IFT_TABLES) { st = RDNA4RTR_ERR_BOUNDS; goto fail; }
                        if (!rdna4rtr_j_consume(c, '{')) { st = RDNA4RTR_ERR_JSON; goto fail; }
                        rdna4rtr_ift_table_binding_t tb;
                        memset(&tb, 0, sizeof tb);
                        int got_param = 0, got_metal = 0;
                        if (!rdna4rtr_j_consume(c, '}')) {
                            for (;;) {
                                char tk[64];
                                size_t tkl = 0;
                                if (!rdna4rtr_j_read_string(c, tk, sizeof tk, &tkl)) { st = RDNA4RTR_ERR_JSON; goto fail; }
                                if (!rdna4rtr_j_consume(c, ':')) { st = RDNA4RTR_ERR_JSON; goto fail; }
                                if (tkl < sizeof tk && strcmp(tk, "param_index") == 0) {
                                    if (!rdna4rtr_j_read_u32(c, &tb.param_index)) { st = RDNA4RTR_ERR_JSON; goto fail; }
                                    got_param = 1;
                                } else if (tkl < sizeof tk && strcmp(tk, "metal_index") == 0) {
                                    if (!rdna4rtr_j_read_u32(c, &tb.metal_index)) { st = RDNA4RTR_ERR_JSON; goto fail; }
                                    got_metal = 1;
                                } else if (tkl < sizeof tk && strcmp(tk, "descriptor") == 0) {
                                    if (rdna4rtr_j_peek(c) == 'n') {
                                        if (!rdna4rtr_j_match_lit(c, "null")) { st = RDNA4RTR_ERR_JSON; goto fail; }
                                    } else {
                                        if (!rdna4rtr_j_parse_desc_obj(c, &tb.set, &tb.binding, &tb.count)) {
                                            st = RDNA4RTR_ERR_JSON; goto fail;
                                        }
                                        tb.has_descriptor = 1u;
                                    }
                                } else {
                                    if (!rdna4rtr_j_skip_val(c, 1u)) { st = RDNA4RTR_ERR_JSON; goto fail; }
                                }
                                if (rdna4rtr_j_consume(c, '}')) break;
                                if (!rdna4rtr_j_consume(c, ',')) { st = RDNA4RTR_ERR_JSON; goto fail; }
                            }
                        }
                        if (!got_param || !got_metal) { st = RDNA4RTR_ERR_JSON; goto fail; }
                        if (tb.metal_index >= RDNA4RTR_MAX_BUFFER_SLOTS) { st = RDNA4RTR_ERR_BOUNDS; goto fail; }
                        if (tb.has_descriptor && tb.binding != RDNA4RTR_BUFFER_BINDING_BASE + tb.metal_index) {
                            st = RDNA4RTR_ERR_ABI_MISMATCH; goto fail;
                        }
                        for (uint32_t j = 0; j < out->table_count; j++) {
                            if (out->tables[j].metal_index == tb.metal_index) {
                                st = RDNA4RTR_ERR_DUPLICATE; goto fail;
                            }
                        }
                        out->tables[out->table_count++] = tb;
                        if (rdna4rtr_j_consume(c, ']')) break;
                        if (!rdna4rtr_j_consume(c, ',')) { st = RDNA4RTR_ERR_JSON; goto fail; }
                    }
                }
                have_tables = 1;
            } else if (klen < sizeof key && strcmp(key, "handles") == 0) {
                if (!rdna4rtr_j_consume(c, '[')) { st = RDNA4RTR_ERR_JSON; goto fail; }
                if (!rdna4rtr_j_consume(c, ']')) {
                    for (;;) {
                        if (out->handle_count >= RDNA4RTR_MAX_IFT_HANDLES) { st = RDNA4RTR_ERR_BOUNDS; goto fail; }
                        if (!rdna4rtr_j_consume(c, '{')) { st = RDNA4RTR_ERR_JSON; goto fail; }
                        uint32_t hid = 0, hkind = 0;
                        int got_id = 0, got_sym = 0, got_kind = 0;
                        char sym_buf[RDNA4RTR_MAX_IFT_SYMBOL_LEN + 1u];
                        size_t sym_len = 0;
                        sym_buf[0] = '\0';

                        if (!rdna4rtr_j_consume(c, '}')) {
                            for (;;) {
                                char hk[64];
                                size_t hkl = 0;
                                if (!rdna4rtr_j_read_string(c, hk, sizeof hk, &hkl)) { st = RDNA4RTR_ERR_JSON; goto fail; }
                                if (!rdna4rtr_j_consume(c, ':')) { st = RDNA4RTR_ERR_JSON; goto fail; }
                                if (hkl < sizeof hk && strcmp(hk, "handle_id") == 0) {
                                    if (!rdna4rtr_j_read_u32(c, &hid)) { st = RDNA4RTR_ERR_JSON; goto fail; }
                                    got_id = 1;
                                } else if (hkl < sizeof hk && strcmp(hk, "symbol") == 0) {
                                    if (!rdna4rtr_j_read_string(c, sym_buf, sizeof sym_buf, &sym_len)) {
                                        st = RDNA4RTR_ERR_JSON; goto fail;
                                    }
                                    if (sym_len == 0u) { st = RDNA4RTR_ERR_JSON; goto fail; }
                                    if (sym_len > RDNA4RTR_MAX_IFT_SYMBOL_LEN) { st = RDNA4RTR_ERR_BOUNDS; goto fail; }
                                    got_sym = 1;
                                } else if (hkl < sizeof hk && strcmp(hk, "kind") == 0) {
                                    char kv[32];
                                    size_t kvl = 0;
                                    if (!rdna4rtr_j_read_string(c, kv, sizeof kv, &kvl) || kvl >= sizeof kv) {
                                        st = RDNA4RTR_ERR_JSON; goto fail;
                                    }
                                    if (strcmp(kv, "BoundingBox") == 0) hkind = RDNA4RTR_IFT_KIND_BOUNDING_BOX;
                                    else if (strcmp(kv, "TriangleAnyHit") == 0) hkind = RDNA4RTR_IFT_KIND_TRIANGLE_ANY_HIT;
                                    else { st = RDNA4RTR_ERR_JSON; goto fail; }
                                    got_kind = 1;
                                } else {
                                    if (!rdna4rtr_j_skip_val(c, 1u)) { st = RDNA4RTR_ERR_JSON; goto fail; }
                                }
                                if (rdna4rtr_j_consume(c, '}')) break;
                                if (!rdna4rtr_j_consume(c, ',')) { st = RDNA4RTR_ERR_JSON; goto fail; }
                            }
                        }

                        if (!got_id || !got_sym || !got_kind) { st = RDNA4RTR_ERR_JSON; goto fail; }
                        if (hid == RDNA4RTR_IFT_HANDLE_NULL || hid > RDNA4RTR_MAX_IFT_HANDLES) {
                            st = RDNA4RTR_ERR_BOUNDS; goto fail;
                        }
                        for (uint32_t j = 0; j < out->handle_count; j++) {
                            if (out->handles[j].handle_id == hid ||
                                strcmp(out->handles[j].symbol, sym_buf) == 0) {
                                st = RDNA4RTR_ERR_DUPLICATE; goto fail;
                            }
                        }
                        if (out->handle_count == handle_cap) {
                            uint32_t ncap = handle_cap ? (handle_cap * 2u) : 8u;
                            if (ncap > RDNA4RTR_MAX_IFT_HANDLES) ncap = RDNA4RTR_MAX_IFT_HANDLES;
                            rdna4rtr_ift_handle_entry_t *nh = (rdna4rtr_ift_handle_entry_t *)realloc(
                                out->handles, (size_t)ncap * sizeof *nh);
                            if (!nh) { st = RDNA4RTR_ERR_OOM; goto fail; }
                            out->handles = nh;
                            handle_cap = ncap;
                        }
                        char *dup = (char *)malloc(sym_len + 1u);
                        if (!dup) { st = RDNA4RTR_ERR_OOM; goto fail; }
                        memcpy(dup, sym_buf, sym_len + 1u);
                        out->handles[out->handle_count++] = (rdna4rtr_ift_handle_entry_t){
                            .handle_id = hid, .kind = hkind, .symbol = dup
                        };
                        if (hid > out->max_handle_id) out->max_handle_id = hid;

                        if (rdna4rtr_j_consume(c, ']')) break;
                        if (!rdna4rtr_j_consume(c, ',')) { st = RDNA4RTR_ERR_JSON; goto fail; }
                    }
                }
                have_handles = 1;
            } else {
                if (!rdna4rtr_j_skip_val(c, 1u)) { st = RDNA4RTR_ERR_JSON; goto fail; }
            }

            if (rdna4rtr_j_consume(c, '}')) break;
            if (!rdna4rtr_j_consume(c, ',')) { st = RDNA4RTR_ERR_JSON; goto fail; }
        }
    }

    if (!have_abi || !have_hdr || !have_tables || !have_handles) { st = RDNA4RTR_ERR_JSON; goto fail; }
    if (!((out->abi_version == RDNA4RTR_IFT_ABI_VERSION_V1 &&
           out->header_words == RDNA4RTR_IFT_HEADER_WORDS_V1) ||
          (out->abi_version == RDNA4RTR_IFT_ABI_VERSION_V2 &&
           out->header_words == RDNA4RTR_IFT_HEADER_WORDS_V2))) {
        st = RDNA4RTR_ERR_ABI_MISMATCH;
        goto fail;
    }
    out->present = 1u;
    return RDNA4RTR_OK;

fail:
    rdna4rtr_ift_refl_destroy(out);
    return st;
}

// Parses both "acceleration_structures" and "intersection_function_tables" from a single
// garnetxlate reflection JSON document in one pass. Non-RT documents succeed with present=0.
static inline rdna4rtr_status_t rdna4rtr_parse_reflection_json(const char *json, size_t len,
                                                               rdna4rtr_reflection_t *out) {
    if (!out) return RDNA4RTR_ERR_INVALID;
    memset(out, 0, sizeof *out);
    if (!json || len == 0u) return RDNA4RTR_OK;

    rdna4rtr_jcur_t c = { json, len, 0u };
    if (!rdna4rtr_j_consume(&c, '{')) return RDNA4RTR_ERR_JSON;
    if (!rdna4rtr_j_consume(&c, '}')) {
        for (;;) {
            char key[64];
            size_t klen = 0;
            if (!rdna4rtr_j_read_string(&c, key, sizeof key, &klen)) {
                rdna4rtr_refl_destroy(out);
                return RDNA4RTR_ERR_JSON;
            }
            if (!rdna4rtr_j_consume(&c, ':')) {
                rdna4rtr_refl_destroy(out);
                return RDNA4RTR_ERR_JSON;
            }
            if (klen < sizeof key && strcmp(key, "acceleration_structures") == 0) {
                rdna4rtr_status_t st = rdna4rtr_parse_as_array_cur(&c, &out->as);
                if (st != RDNA4RTR_OK) {
                    rdna4rtr_refl_destroy(out);
                    return st;
                }
            } else if (klen < sizeof key && strcmp(key, "intersection_function_tables") == 0) {
                rdna4rtr_ift_refl_destroy(&out->ift);
                rdna4rtr_status_t st = rdna4rtr_parse_ift_obj_cur(&c, &out->ift);
                if (st != RDNA4RTR_OK) {
                    rdna4rtr_refl_destroy(out);
                    return st;
                }
            } else {
                if (!rdna4rtr_j_skip_val(&c, 1u)) {
                    rdna4rtr_refl_destroy(out);
                    return RDNA4RTR_ERR_JSON;
                }
            }
            if (rdna4rtr_j_consume(&c, '}')) break;
            if (!rdna4rtr_j_consume(&c, ',')) {
                rdna4rtr_refl_destroy(out);
                return RDNA4RTR_ERR_JSON;
            }
        }
    }
    if (rdna4rtr_j_peek(&c) != -1) {
        rdna4rtr_refl_destroy(out);
        return RDNA4RTR_ERR_JSON;
    }
    return RDNA4RTR_OK;
}

static inline rdna4rtr_status_t rdna4rtr_parse_as_json(const char *json, size_t len,
                                                       rdna4rtr_as_reflection_t *out) {
    if (!out) return RDNA4RTR_ERR_INVALID;
    rdna4rtr_reflection_t full;
    rdna4rtr_status_t st = rdna4rtr_parse_reflection_json(json, len, &full);
    if (st != RDNA4RTR_OK) {
        memset(out, 0, sizeof *out);
        return st;
    }
    *out = full.as;
    rdna4rtr_ift_refl_destroy(&full.ift);
    return RDNA4RTR_OK;
}

static inline rdna4rtr_status_t rdna4rtr_parse_ift_json(const char *json, size_t len,
                                                        rdna4rtr_ift_reflection_t *out) {
    if (!out) return RDNA4RTR_ERR_INVALID;
    rdna4rtr_reflection_t full;
    rdna4rtr_status_t st = rdna4rtr_parse_reflection_json(json, len, &full);
    if (st != RDNA4RTR_OK) {
        memset(out, 0, sizeof *out);
        return st;
    }
    *out = full.ift;
    return RDNA4RTR_OK;
}

// =================================================================================================
// 5. AIR/LLVM IR Intersection Function Kind Inference (for Stage 3 garnetx_translate_stage3)
// =================================================================================================
// Inspects LLVM IR text for a linked [[intersection(...)]] function to classify it as
// GARNETX_IFT_KIND_BOUNDING_BOX (0) or GARNETX_IFT_KIND_TRIANGLE_ANY_HIT (1).
// Returns 1 if inferred unambiguously, 0 if ambiguous (caller defaults to BOUNDING_BOX or errors).
static inline int rdna4rtr_infer_ift_kind_from_ir(const char *ir, size_t ir_len,
                                                  const char *fn_name, uint32_t *kind_out) {
    if (!ir || !ir_len || !fn_name || !fn_name[0] || !kind_out) return 0;
    const size_t nlen = strlen(fn_name);
    const char *end = ir + ir_len;

    // 1) Scan `define ... @<fn_name>(` header line:
    //    - `[[intersection(triangle, ...)]]` returns `i1` (bool accept)
    //    - `[[intersection(bounding_box, ...)]]` returns struct `{ i1, float }` or `bounding_box_result`
    const char *p = ir;
    while (p < end) {
        const char *line_end = (const char *)memchr(p, '\n', (size_t)(end - p));
        if (!line_end) line_end = end;
        const size_t llen = (size_t)(line_end - p);
        if (llen > 7u && memcmp(p, "define ", 7u) == 0) {
            // Search for `@fn_name(` or `@"fn_name"(` on this definition line.
            for (const char *q = p + 7u; q < line_end; q++) {
                if (*q != '@') continue;
                const char *sym = q + 1;
                int quoted = (sym < line_end && *sym == '"');
                if (quoted) sym++;
                if ((size_t)(line_end - sym) >= nlen + (quoted ? 2u : 1u) &&
                    memcmp(sym, fn_name, nlen) == 0 &&
                    (!quoted || sym[nlen] == '"') &&
                    sym[nlen + (quoted ? 1u : 0u)] == '(') {
                    // Inspect return type slice between "define " and '@'.
                    const char *ret_beg = p + 7u;
                    const size_t ret_len = (size_t)(q - ret_beg);
                    // Trim trailing spaces before '@'.
                    size_t r = ret_len;
                    while (r > 0u && (ret_beg[r - 1u] == ' ' || ret_beg[r - 1u] == '\t')) r--;
                    // `[[intersection(triangle, ...)]]` returns `i1` (possibly with zeroext/signext).
                    if (r >= 2u && memcmp(ret_beg + r - 2u, "i1", 2u) == 0 &&
                        (r == 2u || ret_beg[r - 3u] == ' ' || ret_beg[r - 3u] == '\t')) {
                        *kind_out = RDNA4RTR_IFT_KIND_TRIANGLE_ANY_HIT;
                        return 1;
                    }
                    // Aggregate/struct return (`{ i1, float }` or `%struct...`) is bounding_box.
                    for (size_t k = 0; k < r; k++) {
                        if (ret_beg[k] == '%' || ret_beg[k] == '{') {
                            *kind_out = RDNA4RTR_IFT_KIND_BOUNDING_BOX;
                            return 1;
                        }
                    }
                    // `void` with `sret` parameter is also an aggregate bounding_box return.
                    const char *arg_beg = sym + nlen + (quoted ? 2u : 1u);
                    for (const char *s = arg_beg; s + 4u <= line_end; s++) {
                        if (memcmp(s, "sret", 4u) == 0) {
                            *kind_out = RDNA4RTR_IFT_KIND_BOUNDING_BOX;
                            return 1;
                        }
                    }
                }
            }
        }
        p = (line_end < end) ? (line_end + 1) : end;
    }

    // 2) Fallback: check AIR metadata markers mentioning both function name and geometry tag.
    p = ir;
    while (p < end) {
        const char *line_end = (const char *)memchr(p, '\n', (size_t)(end - p));
        if (!line_end) line_end = end;
        if (*p == '!') {
            // Look for fn_name on the same metadata line.
            int has_fn = 0;
            for (const char *q = p; q + nlen <= line_end; q++) {
                if (memcmp(q, fn_name, nlen) == 0) { has_fn = 1; break; }
            }
            if (has_fn) {
                for (const char *q = p; q + 12u <= line_end; q++) {
                    if (memcmp(q, "bounding_box", 12u) == 0) {
                        *kind_out = RDNA4RTR_IFT_KIND_BOUNDING_BOX;
                        return 1;
                    }
                }
                for (const char *q = p; q + 8u <= line_end; q++) {
                    if (memcmp(q, "triangle", 8u) == 0) {
                        *kind_out = RDNA4RTR_IFT_KIND_TRIANGLE_ANY_HIT;
                        return 1;
                    }
                }
            }
        }
        p = (line_end < end) ? (line_end + 1) : end;
    }
    return 0;
}

// =================================================================================================
// 6. IFT Storage-Buffer Encoder & 0077 Dispatch Simulator
//    Buffer layout (matching 0077 ray_query_air.rs & stage2_link.rs):
//      word[0]        = slot_count
//      word[1 + slot] = handle_id (0 = null/default/opaque-triangle confirm; 1..N = linked fn)
// =================================================================================================

typedef enum {
    RDNA4RTR_IFT_SLOT_EMPTY           = 0,
    RDNA4RTR_IFT_SLOT_FUNCTION        = 1,
    RDNA4RTR_IFT_SLOT_OPAQUE_TRIANGLE = 2,
    RDNA4RTR_IFT_SLOT_OPAQUE_CURVE    = 3,
} rdna4rtr_ift_slot_mode_t;

typedef struct {
    uint32_t handle_id;   // 0 for empty/opaque; 1..max_handle_id for linked intersection function
    uint8_t  mode;        // rdna4rtr_ift_slot_mode_t
    uint8_t  fn_kind;     // RDNA4RTR_IFT_KIND_* when mode == FUNCTION
    uint64_t signature;   // MTLIntersectionFunctionSignature when mode == OPAQUE_*
} rdna4rtr_ift_slot_t;

typedef struct {
    uint32_t slot_count;
    uint32_t max_handle_id;
    uint32_t abi_version;
    uint32_t attached_buf_mask;
    uint64_t attached_bda[RDNA4RTR_MAX_ATTACH_BUFFERS];
    rdna4rtr_ift_slot_t *slots;
} rdna4rtr_ift_table_t;

static inline uint32_t rdna4rtr_ift_header_words_for_abi(uint32_t abi_version) {
    return (abi_version == RDNA4RTR_IFT_ABI_VERSION_V2)
        ? RDNA4RTR_IFT_HEADER_WORDS_V2
        : RDNA4RTR_IFT_HEADER_WORDS_V1;
}

static inline size_t rdna4rtr_ift_buffer_words_for_abi(uint32_t slot_count, uint32_t abi_version) {
    return (size_t)rdna4rtr_ift_header_words_for_abi(abi_version) + (size_t)slot_count;
}

static inline size_t rdna4rtr_ift_buffer_bytes_for_abi(uint32_t slot_count, uint32_t abi_version) {
    return rdna4rtr_ift_buffer_words_for_abi(slot_count, abi_version) * sizeof(uint32_t);
}

static inline size_t rdna4rtr_ift_aligned_buffer_bytes_for_abi(uint32_t slot_count, uint32_t abi_version) {
    size_t raw = rdna4rtr_ift_buffer_bytes_for_abi(slot_count, abi_version);
    size_t aligned = (raw + 15u) & ~(size_t)15u;
    return aligned < 16u ? 16u : aligned;
}

static inline size_t rdna4rtr_ift_buffer_words(uint32_t slot_count) {
    return rdna4rtr_ift_buffer_words_for_abi(slot_count, RDNA4RTR_IFT_ABI_VERSION_V1);
}

static inline size_t rdna4rtr_ift_buffer_bytes(uint32_t slot_count) {
    return rdna4rtr_ift_buffer_bytes_for_abi(slot_count, RDNA4RTR_IFT_ABI_VERSION_V1);
}

static inline size_t rdna4rtr_ift_aligned_buffer_bytes(uint32_t slot_count) {
    return rdna4rtr_ift_aligned_buffer_bytes_for_abi(slot_count, RDNA4RTR_IFT_ABI_VERSION_V1);
}

static inline rdna4rtr_status_t rdna4rtr_ift_init_with_abi(rdna4rtr_ift_table_t *t,
                                                           uint32_t slot_count,
                                                           uint32_t max_handle_id,
                                                           uint32_t abi_version) {
    if (!t) return RDNA4RTR_ERR_INVALID;
    memset(t, 0, sizeof *t);
    if (abi_version != RDNA4RTR_IFT_ABI_VERSION_V1 &&
        abi_version != RDNA4RTR_IFT_ABI_VERSION_V2)
        return RDNA4RTR_ERR_ABI_MISMATCH;
    if (slot_count > RDNA4RTR_MAX_IFT_SLOTS || max_handle_id > RDNA4RTR_MAX_IFT_HANDLES)
        return RDNA4RTR_ERR_BOUNDS;
    if (slot_count > 0u) {
        t->slots = (rdna4rtr_ift_slot_t *)calloc(slot_count, sizeof *t->slots);
        if (!t->slots) return RDNA4RTR_ERR_OOM;
    }
    t->slot_count = slot_count;
    t->max_handle_id = max_handle_id;
    t->abi_version = abi_version;
    return RDNA4RTR_OK;
}

static inline rdna4rtr_status_t rdna4rtr_ift_init(rdna4rtr_ift_table_t *t,
                                                  uint32_t slot_count,
                                                  uint32_t max_handle_id) {
    return rdna4rtr_ift_init_with_abi(t, slot_count, max_handle_id, RDNA4RTR_IFT_ABI_VERSION_V1);
}

static inline rdna4rtr_status_t rdna4rtr_ift_set_attached_bda(rdna4rtr_ift_table_t *t,
                                                              uint32_t index,
                                                              uint64_t bda) {
    if (!t) return RDNA4RTR_ERR_INVALID;
    if (index >= RDNA4RTR_MAX_ATTACH_BUFFERS) return RDNA4RTR_ERR_BOUNDS;
    t->attached_bda[index] = bda;
    if (bda != 0u) t->attached_buf_mask |= (UINT32_C(1) << index);
    else t->attached_buf_mask &= ~(UINT32_C(1) << index);
    return RDNA4RTR_OK;
}

static inline uint64_t rdna4rtr_ift_get_attached_bda(const rdna4rtr_ift_table_t *t,
                                                     uint32_t index) {
    if (!t || index >= RDNA4RTR_MAX_ATTACH_BUFFERS) return 0u;
    return t->attached_bda[index];
}

static inline void rdna4rtr_ift_destroy(rdna4rtr_ift_table_t *t) {
    if (!t) return;
    free(t->slots);
    memset(t, 0, sizeof *t);
}

static inline rdna4rtr_status_t rdna4rtr_ift_set_function(rdna4rtr_ift_table_t *t,
                                                          uint32_t index,
                                                          uint32_t handle_id,
                                                          uint32_t fn_kind) {
    if (!t) return RDNA4RTR_ERR_INVALID;
    if (index >= t->slot_count || !t->slots) return RDNA4RTR_ERR_BOUNDS;
    if (handle_id == RDNA4RTR_IFT_HANDLE_NULL) {
        t->slots[index] = (rdna4rtr_ift_slot_t){ 0u, RDNA4RTR_IFT_SLOT_EMPTY, 0u, 0u };
        return RDNA4RTR_OK;
    }
    if (handle_id > t->max_handle_id) return RDNA4RTR_ERR_BOUNDS;
    if (fn_kind != RDNA4RTR_IFT_KIND_BOUNDING_BOX && fn_kind != RDNA4RTR_IFT_KIND_TRIANGLE_ANY_HIT)
        return RDNA4RTR_ERR_INVALID;
    t->slots[index] = (rdna4rtr_ift_slot_t){
        .handle_id = handle_id,
        .mode = RDNA4RTR_IFT_SLOT_FUNCTION,
        .fn_kind = (uint8_t)fn_kind,
        .signature = 0u,
    };
    return RDNA4RTR_OK;
}

static inline rdna4rtr_status_t rdna4rtr_ift_set_functions_range(rdna4rtr_ift_table_t *t,
                                                                 uint32_t start,
                                                                 uint32_t count,
                                                                 const uint32_t *handle_ids,
                                                                 const uint32_t *fn_kinds) {
    if (!t) return RDNA4RTR_ERR_INVALID;
    if (count == 0u) return RDNA4RTR_OK;
    if (!handle_ids) return RDNA4RTR_ERR_INVALID;
    if (start > t->slot_count || count > t->slot_count - start || !t->slots) return RDNA4RTR_ERR_BOUNDS;
    // Validate atomically before mutating any slot.
    for (uint32_t i = 0; i < count; i++) {
        uint32_t hid = handle_ids[i];
        if (hid == RDNA4RTR_IFT_HANDLE_NULL) continue;
        if (hid > t->max_handle_id) return RDNA4RTR_ERR_BOUNDS;
        uint32_t fk = fn_kinds ? fn_kinds[i] : RDNA4RTR_IFT_KIND_BOUNDING_BOX;
        if (fk != RDNA4RTR_IFT_KIND_BOUNDING_BOX && fk != RDNA4RTR_IFT_KIND_TRIANGLE_ANY_HIT)
            return RDNA4RTR_ERR_INVALID;
    }
    for (uint32_t i = 0; i < count; i++) {
        uint32_t hid = handle_ids[i];
        uint32_t fk = fn_kinds ? fn_kinds[i] : RDNA4RTR_IFT_KIND_BOUNDING_BOX;
        if (hid == RDNA4RTR_IFT_HANDLE_NULL) {
            t->slots[start + i] = (rdna4rtr_ift_slot_t){ 0u, RDNA4RTR_IFT_SLOT_EMPTY, 0u, 0u };
        } else {
            t->slots[start + i] = (rdna4rtr_ift_slot_t){
                .handle_id = hid, .mode = RDNA4RTR_IFT_SLOT_FUNCTION,
                .fn_kind = (uint8_t)fk, .signature = 0u
            };
        }
    }
    return RDNA4RTR_OK;
}

static inline rdna4rtr_status_t rdna4rtr_ift_validate_opaque_triangle_sig(uint64_t sig) {
    if ((sig & ~RDNA4RTR_SIG_KNOWN_MASK) != 0u) return RDNA4RTR_ERR_INVALID;
    if ((sig & RDNA4RTR_SIG_CURVE_DATA) != 0u) return RDNA4RTR_ERR_INVALID;
    if ((sig & RDNA4RTR_SIG_MOTION_MASK) != 0u) return RDNA4RTR_ERR_UNSUPPORTED;
    return RDNA4RTR_OK;
}

// Opaque triangle intersection encodes handle_id = 0, matching 0077's default triangle path
// (`tri_default -> OpRayQueryConfirmIntersectionKHR`), while recording slot mode & signature.
static inline rdna4rtr_status_t rdna4rtr_ift_set_opaque_triangle(rdna4rtr_ift_table_t *t,
                                                                 uint32_t index,
                                                                 uint64_t signature) {
    if (!t) return RDNA4RTR_ERR_INVALID;
    if (index >= t->slot_count || !t->slots) return RDNA4RTR_ERR_BOUNDS;
    rdna4rtr_status_t st = rdna4rtr_ift_validate_opaque_triangle_sig(signature);
    if (st != RDNA4RTR_OK) return st;
    t->slots[index] = (rdna4rtr_ift_slot_t){
        .handle_id = RDNA4RTR_IFT_HANDLE_NULL,
        .mode = RDNA4RTR_IFT_SLOT_OPAQUE_TRIANGLE,
        .fn_kind = 0u,
        .signature = signature,
    };
    return RDNA4RTR_OK;
}

static inline rdna4rtr_status_t rdna4rtr_ift_set_opaque_triangle_range(rdna4rtr_ift_table_t *t,
                                                                       uint32_t start,
                                                                       uint32_t count,
                                                                       uint64_t signature) {
    if (!t) return RDNA4RTR_ERR_INVALID;
    if (count == 0u) return rdna4rtr_ift_validate_opaque_triangle_sig(signature);
    if (start > t->slot_count || count > t->slot_count - start || !t->slots) return RDNA4RTR_ERR_BOUNDS;
    rdna4rtr_status_t st = rdna4rtr_ift_validate_opaque_triangle_sig(signature);
    if (st != RDNA4RTR_OK) return st;
    for (uint32_t i = 0; i < count; i++) {
        t->slots[start + i] = (rdna4rtr_ift_slot_t){
            .handle_id = RDNA4RTR_IFT_HANDLE_NULL,
            .mode = RDNA4RTR_IFT_SLOT_OPAQUE_TRIANGLE,
            .fn_kind = 0u,
            .signature = signature,
        };
    }
    return RDNA4RTR_OK;
}

// Curve intersections are honestly unsupported on RADV GFX12. Validate bounds & known bits,
// record safe handle_id = 0 in the slot(s), and return RDNA4RTR_ERR_UNSUPPORTED so the runtime
// can sync the safe zero handle and log once without crashing.
static inline rdna4rtr_status_t rdna4rtr_ift_set_opaque_curve_range(rdna4rtr_ift_table_t *t,
                                                                    uint32_t start,
                                                                    uint32_t count,
                                                                    uint64_t signature) {
    if (!t) return RDNA4RTR_ERR_INVALID;
    if ((signature & ~RDNA4RTR_SIG_KNOWN_MASK) != 0u) return RDNA4RTR_ERR_INVALID;
    if (start > t->slot_count || count > t->slot_count - start || (count > 0u && !t->slots))
        return RDNA4RTR_ERR_BOUNDS;
    for (uint32_t i = 0; i < count; i++) {
        t->slots[start + i] = (rdna4rtr_ift_slot_t){
            .handle_id = RDNA4RTR_IFT_HANDLE_NULL,
            .mode = RDNA4RTR_IFT_SLOT_OPAQUE_CURVE,
            .fn_kind = 0u,
            .signature = signature,
        };
    }
    return RDNA4RTR_ERR_UNSUPPORTED;
}

static inline rdna4rtr_status_t rdna4rtr_ift_set_opaque_curve(rdna4rtr_ift_table_t *t,
                                                              uint32_t index,
                                                              uint64_t signature) {
    return rdna4rtr_ift_set_opaque_curve_range(t, index, 1u, signature);
}

static inline uint32_t rdna4rtr_ift_get_handle(const rdna4rtr_ift_table_t *t, uint32_t index) {
    if (!t || index >= t->slot_count) return RDNA4RTR_IFT_HANDLE_NULL;
    return t->slots[index].handle_id;
}

static inline rdna4rtr_ift_slot_mode_t rdna4rtr_ift_get_mode(const rdna4rtr_ift_table_t *t,
                                                             uint32_t index) {
    if (!t || index >= t->slot_count) return RDNA4RTR_IFT_SLOT_EMPTY;
    return (rdna4rtr_ift_slot_mode_t)t->slots[index].mode;
}

static inline rdna4rtr_status_t rdna4rtr_ift_encode(const rdna4rtr_ift_table_t *t,
                                                    uint32_t *dst_words,
                                                    size_t dst_word_cap,
                                                    size_t *written_words_out) {
    if (!t || !dst_words) return RDNA4RTR_ERR_INVALID;
    uint32_t abi = (t->abi_version == RDNA4RTR_IFT_ABI_VERSION_V2)
        ? RDNA4RTR_IFT_ABI_VERSION_V2
        : RDNA4RTR_IFT_ABI_VERSION_V1;
    uint32_t hdr = rdna4rtr_ift_header_words_for_abi(abi);
    size_t need = (size_t)hdr + (size_t)t->slot_count;
    if (dst_word_cap < need) return RDNA4RTR_ERR_BOUNDS;
    dst_words[0] = t->slot_count;
    if (abi == RDNA4RTR_IFT_ABI_VERSION_V2) {
        dst_words[1] = RDNA4RTR_IFT_ABI_VERSION_V2;
        dst_words[2] = t->attached_buf_mask;
        dst_words[3] = 0u;
        for (uint32_t i = 0; i < RDNA4RTR_MAX_ATTACH_BUFFERS; i++) {
            uint64_t addr = t->attached_bda[i];
            dst_words[RDNA4RTR_IFT_V2_ATTACH_WORD_BASE + 2u * i]      = (uint32_t)(addr & UINT64_C(0xFFFFFFFF));
            dst_words[RDNA4RTR_IFT_V2_ATTACH_WORD_BASE + 2u * i + 1u] = (uint32_t)(addr >> 32);
        }
    }
    for (uint32_t i = 0; i < t->slot_count; i++)
        dst_words[hdr + i] = t->slots[i].handle_id;
    if (written_words_out) *written_words_out = need;
    return RDNA4RTR_OK;
}

static inline rdna4rtr_status_t rdna4rtr_ift_read_attached_bda(const uint32_t *words,
                                                               size_t word_count,
                                                               uint32_t index,
                                                               uint64_t *bda_out) {
    if (!bda_out) return RDNA4RTR_ERR_INVALID;
    *bda_out = 0u;
    if (!words || word_count < RDNA4RTR_IFT_HEADER_WORDS_V2) return RDNA4RTR_ERR_BOUNDS;
    if (words[1] != RDNA4RTR_IFT_ABI_VERSION_V2) return RDNA4RTR_ERR_ABI_MISMATCH;
    if (index >= RDNA4RTR_MAX_ATTACH_BUFFERS) return RDNA4RTR_ERR_BOUNDS;
    uint64_t lo = (uint64_t)words[RDNA4RTR_IFT_V2_ATTACH_WORD_BASE + 2u * index];
    uint64_t hi = (uint64_t)words[RDNA4RTR_IFT_V2_ATTACH_WORD_BASE + 2u * index + 1u];
    *bda_out = lo | (hi << 32);
    return RDNA4RTR_OK;
}

// Host mirror of 0077 / Wave 2 ray_query_air.rs IFT lookup + dispatch switch (for unit verification).
typedef enum {
    RDNA4RTR_DISPATCH_CONFIRM_TRIANGLE = 0, // handle_id==0 on triangle -> OpRayQueryConfirmIntersectionKHR
    RDNA4RTR_DISPATCH_SKIP_AABB        = 1, // handle_id==0 on AABB -> safe skip (no confirm)
    RDNA4RTR_DISPATCH_CALL_HANDLE      = 2, // handle_id!=0 -> invoke linked intersection function
} rdna4rtr_ift_dispatch_action_t;

typedef struct {
    uint32_t computed_slot;
    uint32_t handle_id;
    uint8_t  out_of_bounds;
    rdna4rtr_ift_dispatch_action_t action;
} rdna4rtr_ift_dispatch_result_t;

static inline void rdna4rtr_ift_simulate_dispatch_abi(const uint32_t *words,
                                                      size_t word_count,
                                                      uint32_t abi_version,
                                                      int is_instancing,
                                                      uint32_t cand_sbt_offset,
                                                      uint32_t cand_geom_index,
                                                      int32_t function_table_offset,
                                                      int is_aabb,
                                                      rdna4rtr_ift_dispatch_result_t *out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    uint32_t hdr = rdna4rtr_ift_header_words_for_abi(abi_version);
    uint32_t fto_safe = (function_table_offset < 0) ? 0u : (uint32_t)function_table_offset;
    uint32_t slot = (is_instancing ? cand_sbt_offset : 0u) + cand_geom_index + fto_safe;
    out->computed_slot = slot;
    uint32_t slot_count = (words && word_count >= (size_t)hdr) ? words[0] : 0u;
    if (slot >= slot_count || (size_t)hdr + (size_t)slot >= word_count) {
        out->out_of_bounds = 1u;
        out->handle_id = RDNA4RTR_IFT_HANDLE_NULL;
    } else {
        out->out_of_bounds = 0u;
        out->handle_id = words[hdr + slot];
    }
    if (out->handle_id == RDNA4RTR_IFT_HANDLE_NULL) {
        out->action = is_aabb ? RDNA4RTR_DISPATCH_SKIP_AABB : RDNA4RTR_DISPATCH_CONFIRM_TRIANGLE;
    } else {
        out->action = RDNA4RTR_DISPATCH_CALL_HANDLE;
    }
}

static inline void rdna4rtr_ift_simulate_dispatch(const uint32_t *words,
                                                  size_t word_count,
                                                  int is_instancing,
                                                  uint32_t cand_sbt_offset,
                                                  uint32_t cand_geom_index,
                                                  int32_t function_table_offset,
                                                  int is_aabb,
                                                  rdna4rtr_ift_dispatch_result_t *out) {
    rdna4rtr_ift_simulate_dispatch_abi(words, word_count, RDNA4RTR_IFT_ABI_VERSION_V1,
                                       is_instancing, cand_sbt_offset, cand_geom_index,
                                       function_table_offset, is_aabb, out);
}

// =================================================================================================
// 7. AS Direct Descriptor-Binding vs Argument-Buffer 64-bit BDA Planners
// =================================================================================================

typedef struct {
    uint32_t dst_set;
    uint32_t dst_binding;          // BUFFER_BINDING_BASE + metal_buffer_index
    uint32_t descriptor_type;      // VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR
    uint64_t vk_accel_handle;
    rdna4rtr_as_kind_t kind;
} rdna4rtr_as_desc_plan_t;

static inline rdna4rtr_status_t rdna4rtr_plan_as_descriptor_bind(
    uint32_t metal_buffer_index,
    rdna4rtr_stage_t stage,
    uint64_t vk_accel_handle,
    rdna4rtr_as_kind_t as_kind,
    int as_is_built,
    rdna4rtr_as_kind_t expected_kind,
    rdna4rtr_as_desc_plan_t *out) {
    if (!out) return RDNA4RTR_ERR_INVALID;
    memset(out, 0, sizeof *out);
    if (metal_buffer_index >= RDNA4RTR_MAX_BUFFER_SLOTS) return RDNA4RTR_ERR_BOUNDS;
    if (vk_accel_handle == 0u) return RDNA4RTR_ERR_NULL_HANDLE;
    if (!as_is_built) return RDNA4RTR_ERR_UNBUILT;
    if (expected_kind != RDNA4RTR_AS_KIND_UNKNOWN &&
        as_kind != RDNA4RTR_AS_KIND_UNKNOWN &&
        as_kind != expected_kind)
        return RDNA4RTR_ERR_KIND_MISMATCH;
    out->dst_set = rdna4rtr_stage_descriptor_set(stage);
    out->dst_binding = RDNA4RTR_BUFFER_BINDING_BASE + metal_buffer_index;
    out->descriptor_type = RDNA4RTR_VK_DESC_TYPE_AS;
    out->vk_accel_handle = vk_accel_handle;
    out->kind = as_kind;
    return RDNA4RTR_OK;
}

typedef struct {
    uint64_t byte_offset;
    uint64_t encoded_u64;          // 64-bit VkDeviceAddress (0 for nil)
    uint8_t  is_null;
} rdna4rtr_arg_bda_plan_t;

static inline rdna4rtr_status_t rdna4rtr_plan_as_bda_encode(
    rdna4rtr_arg_kind_t field_kind,
    uint64_t field_offset,
    uint64_t field_size,
    uint64_t buf_length,
    int is_null_as,
    uint64_t as_device_address,
    rdna4rtr_as_kind_t as_kind,
    rdna4rtr_arg_bda_plan_t *out) {
    if (!out) return RDNA4RTR_ERR_INVALID;
    memset(out, 0, sizeof *out);
    if (field_kind != RDNA4RTR_ARG_KIND_PRIMITIVE_AS &&
        field_kind != RDNA4RTR_ARG_KIND_INSTANCE_AS)
        return RDNA4RTR_ERR_INVALID;
    if (field_size != sizeof(uint64_t)) return RDNA4RTR_ERR_ABI_MISMATCH;
    if ((field_offset & 7u) != 0u) return RDNA4RTR_ERR_ALIGNMENT;
    if (field_offset > buf_length || sizeof(uint64_t) > buf_length - field_offset)
        return RDNA4RTR_ERR_BOUNDS;
    out->byte_offset = field_offset;
    if (is_null_as) {
        out->encoded_u64 = 0u;
        out->is_null = 1u;
        return RDNA4RTR_OK;
    }
    rdna4rtr_as_kind_t want = (field_kind == RDNA4RTR_ARG_KIND_INSTANCE_AS)
        ? RDNA4RTR_AS_KIND_INSTANCE : RDNA4RTR_AS_KIND_PRIMITIVE;
    if (as_kind != RDNA4RTR_AS_KIND_UNKNOWN && as_kind != want)
        return RDNA4RTR_ERR_KIND_MISMATCH;
    if (as_device_address == 0u) return RDNA4RTR_ERR_NULL_HANDLE;
    out->encoded_u64 = as_device_address;
    out->is_null = 0u;
    return RDNA4RTR_OK;
}

static inline rdna4rtr_status_t rdna4rtr_plan_ift_bda_encode(
    rdna4rtr_arg_kind_t field_kind,
    uint64_t field_offset,
    uint64_t field_size,
    uint64_t buf_length,
    int is_null_ift,
    uint64_t backing_gpu_address,
    rdna4rtr_arg_bda_plan_t *out) {
    if (!out) return RDNA4RTR_ERR_INVALID;
    memset(out, 0, sizeof *out);
    if (field_kind != RDNA4RTR_ARG_KIND_IFT) return RDNA4RTR_ERR_INVALID;
    if (field_size != sizeof(uint64_t)) return RDNA4RTR_ERR_ABI_MISMATCH;
    if ((field_offset & 7u) != 0u) return RDNA4RTR_ERR_ALIGNMENT;
    if (field_offset > buf_length || sizeof(uint64_t) > buf_length - field_offset)
        return RDNA4RTR_ERR_BOUNDS;
    out->byte_offset = field_offset;
    if (is_null_ift) {
        out->encoded_u64 = 0u;
        out->is_null = 1u;
        return RDNA4RTR_OK;
    }
    if (backing_gpu_address == 0u) return RDNA4RTR_ERR_NULL_HANDLE;
    out->encoded_u64 = backing_gpu_address;
    out->is_null = 0u;
    return RDNA4RTR_OK;
}

static inline int rdna4rtr_arg_kind_is_rt(rdna4rtr_arg_kind_t k) {
    return k == RDNA4RTR_ARG_KIND_PRIMITIVE_AS ||
           k == RDNA4RTR_ARG_KIND_INSTANCE_AS ||
           k == RDNA4RTR_ARG_KIND_IFT;
}

// =================================================================================================
// 8. Wave 4.1 RT Per-Primitive Data Lookup Table ABI (Binding 676 + Device-Wide Open-Address Hash)
// =================================================================================================

typedef struct {
    uint64_t global_table_bda;
    uint64_t table_mask;
    uint64_t slot_as_bda[RDNA4RTR_MAX_BUFFER_SLOTS];
} rdna4rt_stage_prim_table_t;

typedef struct {
    uint64_t as_bda;       // 0 = empty, 1 = tombstone, else 256B-aligned VkDeviceAddress
    uint64_t payload_bda;  // BLAS: geom_table_bda; TLAS: VkAccelerationStructureInstanceKHR[] snapshot BDA
    uint32_t count;        // BLAS: geomCount; TLAS: instanceCount
    uint32_t entry_stride; // BLAS: 0; TLAS: 64
    uint32_t ref_offset;   // BLAS: 0; TLAS: 56
    uint32_t kind;         // 0 = BLAS, 1 = TLAS
} rdna4rt_prim_slot_t;

#ifndef RDNA4RT_GEOM_PRIM_ENTRY_DEFINED
#define RDNA4RT_GEOM_PRIM_ENTRY_DEFINED 1
typedef struct {
    uint64_t data_bda;
    uint32_t stride;
    uint32_t prim_count;
} rdna4rt_geom_prim_entry_t;
#endif

_Static_assert(sizeof(rdna4rt_stage_prim_table_t) == RDNA4RT_PRIM_DATA_STAGE_TABLE_BYTES,
               "rdna4rt_stage_prim_table_t must be 264 bytes");
_Static_assert(offsetof(rdna4rt_stage_prim_table_t, global_table_bda) == 0u,
               "stage prim table global_table_bda offset");
_Static_assert(offsetof(rdna4rt_stage_prim_table_t, table_mask) == 8u,
               "stage prim table table_mask offset");
_Static_assert(offsetof(rdna4rt_stage_prim_table_t, slot_as_bda) == 16u,
               "stage prim table slot_as_bda offset");
_Static_assert(sizeof(rdna4rt_prim_slot_t) == 32u, "rdna4rt_prim_slot_t must be 32 bytes");
_Static_assert(offsetof(rdna4rt_prim_slot_t, as_bda) == 0u, "prim slot as_bda offset");
_Static_assert(offsetof(rdna4rt_prim_slot_t, payload_bda) == 8u, "prim slot payload_bda offset");
_Static_assert(offsetof(rdna4rt_prim_slot_t, count) == 16u, "prim slot count offset");
_Static_assert(offsetof(rdna4rt_prim_slot_t, entry_stride) == 20u, "prim slot entry_stride offset");
_Static_assert(offsetof(rdna4rt_prim_slot_t, ref_offset) == 24u, "prim slot ref_offset offset");
_Static_assert(offsetof(rdna4rt_prim_slot_t, kind) == 28u, "prim slot kind offset");
_Static_assert(sizeof(rdna4rt_geom_prim_entry_t) == 16u, "rdna4rt_geom_prim_entry_t must be 16 bytes");
_Static_assert(offsetof(rdna4rt_geom_prim_entry_t, data_bda) == 0u, "geom prim entry data_bda offset");
_Static_assert(offsetof(rdna4rt_geom_prim_entry_t, stride) == 8u, "geom prim entry stride offset");
_Static_assert(offsetof(rdna4rt_geom_prim_entry_t, prim_count) == 12u, "geom prim entry prim_count offset");

#ifdef VK_KHR_acceleration_structure
_Static_assert(RDNA4RT_PRIM_TLAS_ENTRY_STRIDE == sizeof(VkAccelerationStructureInstanceKHR),
               "TLAS prim table entry_stride must equal sizeof(VkAccelerationStructureInstanceKHR)");
_Static_assert(RDNA4RT_PRIM_TLAS_REF_OFFSET == offsetof(VkAccelerationStructureInstanceKHR, accelerationStructureReference),
               "TLAS prim table ref_offset must equal offsetof(accelerationStructureReference)");
#endif

static inline uint32_t rdna4rt_prim_hash_slot(uint64_t key, uint32_t mask) {
    uint64_t h = (key >> 8) * RDNA4RT_PRIM_HASH_MULT;
    h ^= (h >> 32);
    return (uint32_t)h & mask;
}

static inline void rdna4rt_stage_prim_table_init(rdna4rt_stage_prim_table_t *t,
                                                 uint64_t global_table_bda,
                                                 uint64_t table_mask) {
    if (!t) return;
    memset(t, 0, sizeof *t);
    t->global_table_bda = global_table_bda;
    t->table_mask = table_mask;
}

// Inserts or updates `as_bda` in a power-of-two open-addressed hash table (caller holds CPU mutex).
// Writes payload fields before atomic-release storing `as_bda` so lock-free GPU readers never see torn state.
// Reuses the first tombstone encountered ONLY after confirming `as_bda` does not appear later in the chain.
// Optionally reports the chosen `slot_idx_out` and whether that slot transitioned from EMPTY (0) (`was_empty_out`).
static inline rdna4rtr_status_t rdna4rt_prim_table_upsert_slot(
    rdna4rt_prim_slot_t *slots,
    uint32_t cap,
    uint64_t as_bda,
    uint64_t payload_bda,
    uint32_t count,
    uint32_t entry_stride,
    uint32_t ref_offset,
    uint32_t kind,
    uint32_t *slot_idx_out,
    int *was_empty_out) {
    if (!slots || cap < 2u || (cap & (cap - 1u)) != 0u) return RDNA4RTR_ERR_INVALID;
    if (as_bda <= RDNA4RT_PRIM_SLOT_TOMBSTONE) return RDNA4RTR_ERR_INVALID;
    if (kind != RDNA4RT_PRIM_KIND_BLAS && kind != RDNA4RT_PRIM_KIND_TLAS) return RDNA4RTR_ERR_INVALID;

    const uint32_t mask = cap - 1u;
    const uint32_t start = rdna4rt_prim_hash_slot(as_bda, mask);
    const uint32_t limit = cap < RDNA4RT_PRIM_TABLE_MAX_PROBE ? cap : RDNA4RT_PRIM_TABLE_MAX_PROBE;
    uint32_t first_tomb = UINT32_MAX;
    uint32_t target = UINT32_MAX;
    int target_was_empty = 0;

    for (uint32_t p = 0u; p < limit; p++) {
        const uint32_t idx = (start + p) & mask;
        const uint64_t cur = __atomic_load_n(&slots[idx].as_bda, __ATOMIC_ACQUIRE);
        if (cur == as_bda) {
            target = idx;
            target_was_empty = 0;
            break;
        }
        if (cur == RDNA4RT_PRIM_SLOT_TOMBSTONE) {
            if (first_tomb == UINT32_MAX) first_tomb = idx;
            continue;
        }
        if (cur == RDNA4RT_PRIM_SLOT_EMPTY) {
            if (first_tomb != UINT32_MAX) {
                target = first_tomb;
                target_was_empty = 0;
            } else {
                target = idx;
                target_was_empty = 1;
            }
            break;
        }
    }
    if (target == UINT32_MAX) {
        if (first_tomb != UINT32_MAX) {
            target = first_tomb;
            target_was_empty = 0;
        } else {
            return RDNA4RTR_ERR_BOUNDS;
        }
    }

    __atomic_store_n(&slots[target].payload_bda, payload_bda, __ATOMIC_RELAXED);
    __atomic_store_n(&slots[target].count, count, __ATOMIC_RELAXED);
    __atomic_store_n(&slots[target].entry_stride, entry_stride, __ATOMIC_RELAXED);
    __atomic_store_n(&slots[target].ref_offset, ref_offset, __ATOMIC_RELAXED);
    __atomic_store_n(&slots[target].kind, kind, __ATOMIC_RELAXED);
    __atomic_store_n(&slots[target].as_bda, as_bda, __ATOMIC_RELEASE);
    if (slot_idx_out) *slot_idx_out = target;
    if (was_empty_out) *was_empty_out = target_was_empty;
    return RDNA4RTR_OK;
}

static inline rdna4rtr_status_t rdna4rt_prim_table_upsert(
    rdna4rt_prim_slot_t *slots,
    uint32_t cap,
    uint64_t as_bda,
    uint64_t payload_bda,
    uint32_t count,
    uint32_t entry_stride,
    uint32_t ref_offset,
    uint32_t kind) {
    return rdna4rt_prim_table_upsert_slot(
        slots, cap, as_bda, payload_bda, count, entry_stride, ref_offset, kind, NULL, NULL);
}

// Marks `as_bda` as a tombstone (1) via atomic release. When `expected_payload_bda != 0`, only
// tombstones if the slot's current `payload_bda` matches (safe under MTLHeap placement aliasing
// where a newer AS already re-registered the same BDA before the old AS's dealloc runs).
// Returns 1 if removed (storing the slot index in `slot_idx_out` if non-NULL), 0 otherwise.
static inline int rdna4rt_prim_table_remove_if_match_slot(
    rdna4rt_prim_slot_t *slots,
    uint32_t cap,
    uint64_t as_bda,
    uint64_t expected_payload_bda,
    uint32_t *slot_idx_out) {
    if (!slots || cap < 2u || (cap & (cap - 1u)) != 0u) return 0;
    if (as_bda <= RDNA4RT_PRIM_SLOT_TOMBSTONE) return 0;

    const uint32_t mask = cap - 1u;
    const uint32_t start = rdna4rt_prim_hash_slot(as_bda, mask);
    const uint32_t limit = cap < RDNA4RT_PRIM_TABLE_MAX_PROBE ? cap : RDNA4RT_PRIM_TABLE_MAX_PROBE;

    for (uint32_t p = 0u; p < limit; p++) {
        const uint32_t idx = (start + p) & mask;
        const uint64_t cur = __atomic_load_n(&slots[idx].as_bda, __ATOMIC_ACQUIRE);
        if (cur == RDNA4RT_PRIM_SLOT_EMPTY) return 0;
        if (cur == as_bda) {
            if (expected_payload_bda != 0u) {
                const uint64_t cur_payload = __atomic_load_n(&slots[idx].payload_bda, __ATOMIC_RELAXED);
                if (cur_payload != expected_payload_bda) return 0;
            }
            __atomic_store_n(&slots[idx].as_bda, RDNA4RT_PRIM_SLOT_TOMBSTONE, __ATOMIC_RELEASE);
            if (slot_idx_out) *slot_idx_out = idx;
            return 1;
        }
    }
    return 0;
}

static inline int rdna4rt_prim_table_remove_if_match(
    rdna4rt_prim_slot_t *slots,
    uint32_t cap,
    uint64_t as_bda,
    uint64_t expected_payload_bda) {
    return rdna4rt_prim_table_remove_if_match_slot(slots, cap, as_bda, expected_payload_bda, NULL);
}

static inline int rdna4rt_prim_table_remove(
    rdna4rt_prim_slot_t *slots, uint32_t cap, uint64_t as_bda) {
    return rdna4rt_prim_table_remove_if_match(slots, cap, as_bda, 0u);
}

// Lock-free reader mirror of GPU `@__m2v.rq.query.lookup_prim_data` hash probe.
static inline int rdna4rt_prim_table_lookup(
    const rdna4rt_prim_slot_t *slots,
    uint32_t cap,
    uint64_t as_bda,
    rdna4rt_prim_slot_t *out) {
    if (!slots || cap < 2u || (cap & (cap - 1u)) != 0u) return 0;
    if (as_bda <= RDNA4RT_PRIM_SLOT_TOMBSTONE) return 0;

    const uint32_t mask = cap - 1u;
    const uint32_t start = rdna4rt_prim_hash_slot(as_bda, mask);
    const uint32_t limit = cap < RDNA4RT_PRIM_TABLE_MAX_PROBE ? cap : RDNA4RT_PRIM_TABLE_MAX_PROBE;

    for (uint32_t p = 0u; p < limit; p++) {
        const uint32_t idx = (start + p) & mask;
        const uint64_t cur = __atomic_load_n(&slots[idx].as_bda, __ATOMIC_ACQUIRE);
        if (cur == RDNA4RT_PRIM_SLOT_EMPTY) return 0;
        if (cur == as_bda) {
            if (out) {
                out->as_bda = cur;
                out->payload_bda = __atomic_load_n(&slots[idx].payload_bda, __ATOMIC_RELAXED);
                out->count = __atomic_load_n(&slots[idx].count, __ATOMIC_RELAXED);
                out->entry_stride = __atomic_load_n(&slots[idx].entry_stride, __ATOMIC_RELAXED);
                out->ref_offset = __atomic_load_n(&slots[idx].ref_offset, __ATOMIC_RELAXED);
                out->kind = __atomic_load_n(&slots[idx].kind, __ATOMIC_RELAXED);
            }
            return 1;
        }
    }
    return 0;
}

#endif // RDNA4_RT_RUNTIME_H
