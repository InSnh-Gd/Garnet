#pragma once
/* garnet_spvrefl.h - minimal SPIR-V reflection for GarnetMetal pipeline creation (native #11 step 11e).
 * Ported from tools/native/shader-compile-census/compile-census.c (validated on 274/274 compositor-set modules):
 * descriptor sets/bindings from OpVariable + Decoration, push-constant size from member Offsets, vertex inputs from Location,
 * fragment colour outputs from Location/FragDepth. Names are prefixed rdna4s_. Header-only, static. */
#ifndef RDNA4_SPVREFL_H
#define RDNA4_SPVREFL_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#include "garnet_null_shape.h"

#define RDNA4S_NONUNIFORM_SAMPLED 1u
#define RDNA4S_NONUNIFORM_STORAGE 2u
#define RDNA4S_NONUNIFORM_BUFFERS 4u
#define RDNA4S_NONUNIFORM_OTHER 8u
#define RDNA4S_RUNTIME_ARRAY 16u
#define RDNA4S_IMAGE_GATHER_EXTENDED 32u
#define RDNA4S_NONUNIFORM_UNIFORM_TEXEL 64u
#define RDNA4S_NONUNIFORM_STORAGE_TEXEL 128u

/* ------------------------------------------------------------------------------------------------ SPIR-V model */
typedef struct {
   uint32_t op;        /* defining opcode (0 = unknown) */
   uint32_t width, n;  /* int/float width; vector/matrix/struct member count; array length id */
   uint32_t elem;      /* element / pointee / sampled type */
   uint32_t sc;        /* storage class (variable, pointer) */
   uint32_t dim, depth, arrayed, ms, sampled, signedness;
   uint32_t cval;      /* OpConstant low word */
   uint32_t *mem;      /* struct members */
   uint32_t *moff, *mstride; /* member Offset / MatrixStride decorations (0xffffffff = none) */
   int set, bind, loc, builtin; /* -1 = none */
   int block, bufblock, patch;
   uint32_t stride;    /* ArrayStride */
} RDNA4sId;

typedef struct {
   uint32_t bound;
   RDNA4sId *id;
   uint32_t ep_model, ep_id; char ep_name[128]; int has_ep;
   uint32_t tess_domain;
   uint32_t descriptor_features;
} RDNA4sMod;


static inline int rdna4s_parse(const uint32_t *w, size_t nw, RDNA4sMod *m, char *why, size_t whyn)
{
   memset(m, 0, sizeof *m);
   if (w[0] != 0x07230203) { snprintf(why, whyn, "bad SPIR-V magic"); return 1; }
   m->bound = w[3];
   if (m->bound > 1000000) { snprintf(why, whyn, "id bound %u", m->bound); return 1; }
   m->id = calloc(m->bound + 1, sizeof(RDNA4sId));
   for (uint32_t i = 0; i <= m->bound; i++) { m->id[i].set = m->id[i].bind = m->id[i].loc = m->id[i].builtin = -1; }
   /* pass 1: decorations and definitions (decorations may precede type defs, so member arrays are sized lazily) */
   size_t p = 5;
   while (p < nw) {
      uint32_t op = w[p] & 0xffff, wc = w[p] >> 16;
      if (!wc || p + wc > nw) { snprintf(why, whyn, "truncated instruction at word %zu", p); return 1; }
      const uint32_t *a = w + p + 1;
      #define ID(x) ((x) <= m->bound ? &m->id[x] : &m->id[0])
      switch (op) {
      case 17: /* OpCapability */
         if (wc != 2) { snprintf(why, whyn, "invalid capability instruction"); return 1; }
         switch (a[0]) {
         case 25: m->descriptor_features |= RDNA4S_IMAGE_GATHER_EXTENDED; break;
         case 5302: m->descriptor_features |= RDNA4S_RUNTIME_ARRAY; break;
         case 5307: m->descriptor_features |= RDNA4S_NONUNIFORM_SAMPLED; break;
         case 5309: m->descriptor_features |= RDNA4S_NONUNIFORM_STORAGE; break;
         case 5306: case 5308: m->descriptor_features |= RDNA4S_NONUNIFORM_BUFFERS; break;
         case 5310: m->descriptor_features |= RDNA4S_NONUNIFORM_OTHER; break;
         case 5311: m->descriptor_features |= RDNA4S_NONUNIFORM_UNIFORM_TEXEL; break;
         case 5312: m->descriptor_features |= RDNA4S_NONUNIFORM_STORAGE_TEXEL; break;
         }
         break;
      case 15: /* OpEntryPoint model id name... */
         if (!m->has_ep) {
            m->has_ep = 1; m->ep_model = a[0]; m->ep_id = a[1];
            strncpy(m->ep_name, (const char *)&a[2], sizeof m->ep_name - 1);
         }
         break;
      case 16: /* OpExecutionMode fn mode [operands...] */
         if (wc >= 3 && (a[1] == 22 || a[1] == 24 || a[1] == 25))
            m->tess_domain = a[1];
         break;
      case 71: { /* OpDecorate target decoration [args] */
         RDNA4sId *t = ID(a[0]);
         switch (a[1]) {
         case 11: t->builtin = (int)a[2]; break;
         case 15: t->patch = 1; break;
         case 33: t->bind = (int)a[2]; break;
         case 34: t->set = (int)a[2]; break;
         case 30: t->loc = (int)a[2]; break;
         case 2: t->block = 1; break;
         case 3: t->bufblock = 1; break;
         case 6: t->stride = a[2]; break;
         }
         break; }
      case 72: { /* OpMemberDecorate struct member decoration [args] */
         RDNA4sId *t = ID(a[0]); uint32_t mi = a[1];
         if (mi < 4096) {
            if (!t->moff) { t->moff = malloc(4096 * 4); t->mstride = malloc(4096 * 4); memset(t->moff, 0xff, 4096 * 4); memset(t->mstride, 0xff, 4096 * 4); }
            if (a[2] == 35) t->moff[mi] = a[3];
            if (a[2] == 7) t->mstride[mi] = a[3]; /* MatrixStride */
         }
         break; }
      case 21: { RDNA4sId *t = ID(a[0]); t->op = op; t->width = a[1]; t->signedness = a[2]; break; }
      case 22: { RDNA4sId *t = ID(a[0]); t->op = op; t->width = a[1]; break; }
      case 20: { RDNA4sId *t = ID(a[0]); t->op = op; t->width = 32; break; }
      case 23: case 24: { RDNA4sId *t = ID(a[0]); t->op = op; t->elem = a[1]; t->n = a[2]; break; }
      case 25: { RDNA4sId *t = ID(a[0]); t->op = op; t->elem = a[1]; t->dim = a[2]; t->depth = a[3]; t->arrayed = a[4]; t->ms = a[5]; t->sampled = a[6]; break; }
      case 26: { RDNA4sId *t = ID(a[0]); t->op = op; break; }
      case 27: { RDNA4sId *t = ID(a[0]); t->op = op; t->elem = a[1]; break; }
      case 28: { RDNA4sId *t = ID(a[0]); t->op = op; t->elem = a[1]; t->n = a[2]; break; }
      case 29: { RDNA4sId *t = ID(a[0]); t->op = op; t->elem = a[1]; break; }
      case 30: { RDNA4sId *t = ID(a[0]); t->op = op; t->n = wc - 2; t->mem = malloc(4 * (t->n + 1)); for (uint32_t k = 0; k < t->n; k++) t->mem[k] = a[1 + k]; break; }
      case 32: { RDNA4sId *t = ID(a[0]); t->op = op; t->sc = a[1]; t->elem = a[2]; break; }
      case 43: /* OpConstant type id value */ { RDNA4sId *t = ID(a[1]); t->op = op; t->cval = a[2]; break; }
      case 59: /* OpVariable type id sc */ { RDNA4sId *t = ID(a[1]); t->op = op; t->elem = a[0]; t->sc = a[2]; break; }
      case 5341: /* OpTypeAccelerationStructureKHR */ { RDNA4sId *t = ID(a[0]); t->op = op; break; }
      }
      p += wc;
   }
   /* struct member decorations were attached to the struct id before its definition: nothing more to do (same RDNA4sId slot). */
   return 0;
}

static inline uint32_t rdna4s_tsize(RDNA4sMod *m, uint32_t t, int depth)
{
   if (depth > 32 || t > m->bound) return 0;
   RDNA4sId *x = &m->id[t];
   switch (x->op) {
   case 21: case 22: return (x->width + 7) / 8;
   case 20: return 4;
   case 23: return x->n * rdna4s_tsize(m, x->elem, depth + 1);
   case 24: { uint32_t col = rdna4s_tsize(m, x->elem, depth + 1); RDNA4sId *v = &m->id[x->elem]; (void)v; return x->n * col; }
   case 28: { RDNA4sId *len = &m->id[x->n]; uint32_t es = x->stride ? x->stride : rdna4s_tsize(m, x->elem, depth + 1); return len->cval * es; }
   case 29: return 0;
   case 32: return 8;
   case 30: {
      uint32_t mx = 0;
      for (uint32_t k = 0; k < x->n; k++) {
         uint32_t off = (x->moff && x->moff[k] != 0xffffffffu) ? x->moff[k] : 0;
         uint32_t s = rdna4s_tsize(m, x->mem[k], depth + 1);
         /* a matrix member with a MatrixStride occupies n * stride */
         RDNA4sId *mt = &m->id[x->mem[k]];
         if (mt->op == 24 && x->mstride && x->mstride[k] != 0xffffffffu) s = mt->n * x->mstride[k];
         if (off + s > mx) mx = off + s;
      }
      return mx; }
   }
   return 0;
}

/* ------------------------------------------------------------------------------------------------ layout */
#define RDNA4S_MAXSET 32
#define RDNA4S_MAXBIND 1024
typedef struct { int used; VkDescriptorType type; uint32_t count; int runtime; uint8_t imageSampleKinds, texelComponentKinds; } RDNA4sBind;
typedef struct {
   RDNA4sBind bind[RDNA4S_MAXSET][RDNA4S_MAXBIND]; int maxset;
   uint32_t pc_size;
   uint32_t descriptor_features;
   uint64_t argumentNullImages;   /* ABI 5: 2*dim + arrayed + 8*depth + 16*component (float/uint/int). */
   uint32_t argumentNullShapes[RDNA4NS_MAX_SHAPES];
   uint16_t argumentNullShapeCount;
   uint8_t argumentNullShapesInvalid;
   VkVertexInputAttributeDescription va[32]; VkVertexInputBindingDescription vb[32]; int nva;
   int nout; int frag_depth; uint32_t outmask;   /* outmask: bit n set = the fragment stage writes output Location n (11e-2: an unwritten attachment keeps its contents, Metal semantics) */
   char why[256];
} RDNA4sRefl;

static inline int rdna4s_null_shape_from_image(RDNA4sMod *m, const RDNA4sId *image,
                                        rdna4ns_shape *shape)
{
   if (!m || !image || !shape || image->op != 25 ||
       image->elem > m->bound)
      return 0;
   RDNA4sId *component = &m->id[image->elem];
   unsigned access;
   if (image->dim == 5) {
      if (image->arrayed || image->depth || image->ms ||
          (image->sampled != 1 && image->sampled != 2))
         return 0;
      access = image->sampled == 2 ? RDNA4NS_ACCESS_STORAGE_TEXEL
                                   : RDNA4NS_ACCESS_UNIFORM_TEXEL;
   } else if (image->dim <= 3) {
      if (image->sampled != 1 && image->sampled != 2)
         return 0;
      access = image->sampled == 2 ? RDNA4NS_ACCESS_STORAGE
                                   : RDNA4NS_ACCESS_SAMPLED;
   } else {
      return 0;
   }
   unsigned kind;
   if (component->op == 22)
      kind = RDNA4NS_COMP_FLOAT;
   else if (component->op == 21 && component->signedness <= 1)
      kind = component->signedness ? RDNA4NS_COMP_SINT : RDNA4NS_COMP_UINT;
   else
      return 0;
   *shape = (rdna4ns_shape){
      .dim = (uint8_t)(image->dim == 5 ? RDNA4NS_DIM_BUFFER : image->dim),
      .arrayed = (uint8_t)image->arrayed,
      .component = (uint8_t)kind,
      .depth = (uint8_t)image->depth,
      .multisampled = (uint8_t)image->ms,
      .access = (uint8_t)access,
   };
   return rdna4ns_shape_valid(shape);
}

static inline void rdna4s_null_shape_add(RDNA4sRefl *r, const rdna4ns_shape *shape)
{
   if (!r || !shape) return;
   uint32_t word = rdna4ns_shape_encode(shape);
   size_t insert = 0;
   while (insert < r->argumentNullShapeCount &&
          r->argumentNullShapes[insert] < word)
      ++insert;
   if (insert < r->argumentNullShapeCount &&
       r->argumentNullShapes[insert] == word)
      return;
   if (r->argumentNullShapeCount >= RDNA4NS_MAX_SHAPES) {
      r->argumentNullShapesInvalid = 1;
      return;
   }
   memmove(&r->argumentNullShapes[insert + 1],
           &r->argumentNullShapes[insert],
           (r->argumentNullShapeCount - insert) *
               sizeof r->argumentNullShapes[0]);
   r->argumentNullShapes[insert] = word;
   ++r->argumentNullShapeCount;
}

static inline VkFormat rdna4s_fmt(RDNA4sMod *m, uint32_t ty, uint32_t *ncomp)
{
   RDNA4sId *x = &m->id[ty]; uint32_t n = 1;
   if (x->op == 23) { n = x->n; x = &m->id[x->elem]; }
   *ncomp = n;
   int flt = x->op == 22, sgn = 0;
   (void)sgn;
   /* Vertex input dummy formats retain the existing unsigned integer convention. */
   if (flt && x->width == 32) { static const VkFormat f[] = { 0, VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT }; return f[n > 4 ? 4 : n]; }
   if (flt && x->width == 16) { static const VkFormat f[] = { 0, VK_FORMAT_R16_SFLOAT, VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R16G16B16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT }; return f[n > 4 ? 4 : n]; }
   if (flt && x->width == 64) { static const VkFormat f[] = { 0, VK_FORMAT_R64_SFLOAT, VK_FORMAT_R64G64_SFLOAT, VK_FORMAT_R64G64B64_SFLOAT, VK_FORMAT_R64G64B64A64_SFLOAT }; return f[n > 4 ? 4 : n]; }
   if (x->op == 21 && x->width == 32) { static const VkFormat f[] = { 0, VK_FORMAT_R32_UINT, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32B32_UINT, VK_FORMAT_R32G32B32A32_UINT }; return f[n > 4 ? 4 : n]; }
   if (x->op == 21 && x->width == 16) { static const VkFormat f[] = { 0, VK_FORMAT_R16_UINT, VK_FORMAT_R16G16_UINT, VK_FORMAT_R16G16B16_UINT, VK_FORMAT_R16G16B16A16_UINT }; return f[n > 4 ? 4 : n]; }
   if (x->op == 21 && x->width == 8) { static const VkFormat f[] = { 0, VK_FORMAT_R8_UINT, VK_FORMAT_R8G8_UINT, VK_FORMAT_R8G8B8_UINT, VK_FORMAT_R8G8B8A8_UINT }; return f[n > 4 ? 4 : n]; }
   return VK_FORMAT_R32G32B32A32_SFLOAT;
}

/* returns 0 ok, 1 = layout failure (reason in r->why) */
static inline int rdna4s_reflect(RDNA4sMod *m, int stage /* 0 v, 1 f, 2 k */, RDNA4sRefl *r)
{
   memset(r, 0, sizeof *r);
   r->descriptor_features = m->descriptor_features;
   r->maxset = -1;
   for (uint32_t i = 1; i <= m->bound; i++) {
      RDNA4sId *v = &m->id[i];
      if (v->op != 59) continue;
      RDNA4sId *ptr = &m->id[v->elem];
      uint32_t pointee = ptr->elem;
      if (v->sc == 9) { /* PushConstant */
         uint32_t s = rdna4s_tsize(m, pointee, 0); s = (s + 3) & ~3u;
         if (s > r->pc_size) r->pc_size = s;
      } else if (v->sc == 0 || v->sc == 2 || v->sc == 12) { /* UniformConstant, Uniform, StorageBuffer */
         uint32_t count = 1, t = pointee; int runtime = 0;
         while (m->id[t].op == 28 || m->id[t].op == 29) {
            if (m->id[t].op == 28) count *= m->id[m->id[t].n].cval ? m->id[m->id[t].n].cval : 1;
            else runtime = 1;
            t = m->id[t].elem;
         }
         RDNA4sId *x = &m->id[t];
         VkDescriptorType dt;
         if (x->op == 30) dt = (v->sc == 12 || x->bufblock) ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
         else if (x->op == 25) {
            if (x->dim == 6) dt = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
            else if (x->dim == 5) dt = x->sampled == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
            else dt = x->sampled == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
         }
         else if (x->op == 26) dt = VK_DESCRIPTOR_TYPE_SAMPLER;
         else if (x->op == 27) dt = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
         else if (x->op == 5341) dt = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
         else { snprintf(r->why, sizeof r->why, "unrecognised descriptor variable %%%u (sc %u, pointee op %u)", i, v->sc, x->op); return 1; }
         int set = v->set < 0 ? 0 : v->set, b = v->bind < 0 ? 0 : v->bind;
         if (set >= RDNA4S_MAXSET) { snprintf(r->why, sizeof r->why, "set %d >= %d", set, RDNA4S_MAXSET); return 1; }
         if (b >= RDNA4S_MAXBIND) { snprintf(r->why, sizeof r->why, "binding %d >= %d", b, RDNA4S_MAXBIND); return 1; }
         RDNA4sBind *bd = &r->bind[set][b];
         if (bd->used && bd->type != dt) { snprintf(r->why, sizeof r->why, "set %d binding %d used as two descriptor types", set, b); return 1; }
         if (!bd->used || count > bd->count) bd->count = count;
         bd->runtime |= runtime;
         bd->used = 1; bd->type = dt;
         if (dt == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE)
            bd->imageSampleKinds |= x->ms ? 2u : 1u;
         if (dt == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || dt == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) {
            rdna4ns_shape shape;
            if (!rdna4s_null_shape_from_image(m, x, &shape)) {
               snprintf(r->why, sizeof r->why, "invalid texel-buffer component type"); return 1;
            }
            bd->texelComponentKinds |= 1u << shape.component;
         }
         if (x->op == 25 && x->dim != 6) {
            rdna4ns_shape shape;
            if (rdna4s_null_shape_from_image(m, x, &shape))
               rdna4s_null_shape_add(r, &shape);
            else
               r->argumentNullShapesInvalid = 1;
         }
         if (set == 2 && b == 0 && dt == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE && x->sampled == 1 &&
             x->dim <= 3 && x->depth <= 1 && x->arrayed <= 1 && x->ms == 0 &&
             x->elem <= m->bound && m->id[x->elem].width == 32) {
            RDNA4sId *component = &m->id[x->elem];
            if (component->op == 22 || (component->op == 21 && component->signedness <= 1 && !x->depth)) {
               unsigned kind = component->op == 22 ? 0 : component->signedness ? 2 : 1;
               r->argumentNullImages |= 1ull << ((x->dim * 2 + x->arrayed) + 8 * x->depth + 16 * kind);
            }
         }
         if (set > r->maxset) r->maxset = set;
      } else if (v->sc == 1 && v->builtin < 0) { /* Input */
         if (stage != 0 || m->ep_model == 2) continue;
         if (v->loc < 0) continue;
         uint32_t t = pointee, reps = 1;
         while (m->id[t].op == 28) { reps *= m->id[m->id[t].n].cval ? m->id[m->id[t].n].cval : 1; t = m->id[t].elem; }
         uint32_t cols = 1;
         if (m->id[t].op == 24) { cols = m->id[t].n; t = m->id[t].elem; }
         uint32_t nc; VkFormat f = rdna4s_fmt(m, t, &nc);
         uint32_t slots = (m->id[m->id[t].op == 23 ? m->id[t].elem : t].width == 64 && nc > 2) ? 2 : 1;
         for (uint32_t k = 0; k < reps * cols; k++) {
            int loc = v->loc + (int)(k * slots);
            if (loc >= 32 || r->nva >= 32) { snprintf(r->why, sizeof r->why, "vertex input location %d >= 32", loc); return 1; }
            r->va[r->nva] = (VkVertexInputAttributeDescription){ (uint32_t)loc, (uint32_t)loc, f, 0 };
            r->vb[r->nva] = (VkVertexInputBindingDescription){ (uint32_t)loc, 32, VK_VERTEX_INPUT_RATE_VERTEX };
            r->nva++;
         }
      } else if (v->sc == 3 && stage == 1) { /* Output of a fragment shader */
         if (v->builtin == 22) { r->frag_depth = 1; continue; }
         if (v->builtin >= 0 || v->loc < 0) continue;
         uint32_t t = pointee, reps = 1;
         while (m->id[t].op == 28) { reps *= m->id[m->id[t].n].cval ? m->id[m->id[t].n].cval : 1; t = m->id[t].elem; }
         if ((int)(v->loc + reps) > r->nout) r->nout = v->loc + (int)reps;
         for (uint32_t k = 0; k < reps && v->loc + k < 32; k++) r->outmask |= 1u << (v->loc + k);
      }
   }
   if (r->nout > 8) { snprintf(r->why, sizeof r->why, "fragment outputs need %d color attachments (> 8)", r->nout); return 1; }
   return 0;
}


static inline void rdna4s_free(RDNA4sMod *m)
{
   if (!m->id) return;
   for (uint32_t k = 0; k <= m->bound; k++) { free(m->id[k].mem); free(m->id[k].moff); free(m->id[k].mstride); }
   free(m->id); m->id = NULL;
}
#endif
