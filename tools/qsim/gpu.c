//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * gpu.c -- qsim software raster model of the openfpgaOS os25 gpu_core.
 *
 * See gpu.h.  Every rule below cites the gpu_core.v construct it mirrors
 * (RTL snapshot: qsim/rtlsim/rtl/gpu_core.v, sha256 in SNAPSHOT.sha256).
 * The RTL's micro-architecture (fragment pipe, fb/z accumulators, write
 * queue/combiner, z/blend read windows, PSS sub-FSM scheduling) is
 * architecturally transparent -- its exactness contracts make the memory
 * result equal to strictly sequential per-pixel processing -- so it is
 * modelled as a sequential loop.  What is NOT transparent and IS modelled:
 *   - the texture/colormap line cache (stale lines until GPU_TEX_FLUSH),
 *   - persistent staging registers reused across commands (lane banks,
 *     clear-rect registers, pending fence/flip payload),
 *   - all fixed-point truncation/rounding/saturation.
 */
#include "gpu.h"
#include "lint.h"

#include <stdlib.h>
#include <string.h>

#define ADDR26(x) ((uint32_t)(x) & GPU_ADDR_MASK)

enum {
    COV_SLOPE_POW2, COV_SLOPE_DIVMAG, COV_SLOPE_DIV_CORR, COV_SLOPE_FULL16,
    COV_SLOPE_NEG_DELTA, COV_CLAMP_SING_Q29, COV_CLAMP_SING_Q16, COV_CLAMP_RATIO_Q16,
    COV_RECIP_BIAS, COV_RECIP_ZERO, COV_TAIL_SEG, COV_TEXCLAMP_MIN,
    COV_TEXCLAMP_MAX, COV_MIRROR_REV, COV_STALE_TEXEL, COV_ZRESTORE_SAT,
    COV_ZADV_SAT, COV_HDR_UNSUPPORTED, COV_CHUNK_CONT, COV_COMPACT_ODD_SIZE,
    COV_LONG_RECORDS_CUT, COV_COLUMN_ZERO_S, COV_CLEAR_PARTIAL_WORD, COV_CLEAR_STRIDE_FALLBACK,
    COV_ZTEST_PASS_WRITE, COV_ZWRITE_ONLY, COV_TRANSLUC_CMAP, COV_AXIS_Y,
    COV_CMAP_PAGE_WRAP, COV_TEXWIDTH_ZERO, COV_ODD_ZADDR, COV_PERSP_Q16,
    COV_CONSTZ_Q29, COV_NEG_UV, COV_SPAN_CONT_EMU, COV_PARTIAL_CMD,
    COV_N
};
const char *const gpu_cov_names[40] = {
    "slope_pow2", "slope_divmag", "slope_div_corr", "slope_full16",
    "slope_neg_delta", "clamp_singular_q29", "clamp_signflip_q16", "clamp_ratio_q16",
    "recip_constz_bias", "recip_zero", "q29_tail_segment", "texclamp_min",
    "texclamp_max", "mirror_reversed", "stale_texel_served", "zrestore_saturated",
    "zadvance_saturated", "header_unsupported", "record_chunk_cont", "compact_odd_size",
    "long_records_cut", "column_zeroed_s", "clear_partial_word", "clear_stride_fallback",
    "ztest_pass_write", "zwrite_only", "transluc_colormap", "axis_y",
    "cmap_page_wrap", "tex_width_zero", "odd_z_addr", "persp_q16",
    "constz_q29", "negative_uv", "span_cont_emulated", "partial_cmd_buffered",
    "", "", "", "",
};
#define COV(g, k) ((g)->st.cov[k]++)

/* span flag bits after span_flags_from_wire() */
#define SPF_COLORMAP 0x1u
#define SPF_SKIP0    0x2u
#define SPF_PERSP    0x4u
#define SPF_TRANSLUC 0x8u

enum {
    CMD_FENCE = 0x02, CMD_CLEAR_RECT = 0x11, CMD_SET_TEXTURE = 0x20,
    CMD_SET_FB = 0x23, CMD_FLIP = 0x42, CMD_PARAM_SPAN_LIST = 0x48,
    CMD_SET_TRI_STATE = 0x4A, CMD_COLUMN_LIST = 0x4C, CMD_SPAN_CONT = 0x58,
};

typedef struct tex_line {
    uint32_t tag;      /* addr[25:14] */
    uint8_t  valid;
    uint8_t  data[16];
} tex_line_t;

struct gpu {
    gpu_config_t cfg;
    gpu_mem_if_t mem;
    gpu_stats_t  st;
    gpu_flip_cb_t flip_cb;
    void *flip_ctx;
    gpu_cmd_cb_t cmd_cb;
    void *cmd_ctx;
    gpu_cmd_cb_t pub_cb;
    void *pub_ctx;
    struct qsim_lint *lint;

    /* published, not yet executed command words (ring BRAM rdptr..wrptr) */
    uint32_t *q;
    uint32_t  q_head, q_tail, q_scan, q_cap;
    uint64_t  obs_words, obs_px;    /* stats at the previous CPU observation */

    /* ---- MMIO-visible state ---- */
    uint32_t palookup_base;         /* 26 bits */
    uint32_t fence_reached;
    uint32_t transluc_wr_addr;      /* 15 bits */
    uint32_t transluc[8192];        /* SRAM words, byte key = {src[7:1], dst} */
    uint8_t  cpu_ring_mode, cpu_ring_overflow;
    uint32_t ring_wr_addr;          /* 12-bit append pointer (words) */
    uint32_t ring_wrptr;            /* published pointer (words) */
    uint32_t ring_words[4096];      /* CPU-ring staged words since last publish */
    uint32_t ring_staged;
    uint32_t dma_src, dma_len;

    /* partial command carried across batches */
    uint32_t *pend;
    uint32_t  pend_n, pend_cap;

    /* ---- command-FSM persistent registers ---- */
    uint16_t st_fb_stride;
    uint32_t pending_fence_token;
    uint8_t  pending_swap_idx;
    /* clear rect */
    uint32_t cr_addr, cr_row_addr;
    uint16_t cr_w_total, cr_w_remaining, cr_y_remaining, cr_stride;
    uint8_t  cr_color;

    /* ---- shared spanprod staging (load_param_span_list_payload_word) ---- */
    uint32_t sp_fb_base;            /* 26 */
    uint32_t sp_fb_major, sp_fb_minor;  /* 26-bit signed, stored raw */
    uint32_t sp_tex_addr;           /* 26 */
    uint16_t sp_tex_width;
    uint8_t  sp_tex_width_nz;
    uint16_t sp_w_mask, sp_h_mask;
    uint8_t  sp_flags;              /* SPF_* */
    uint8_t  sp_colormap_id;
    uint8_t  sp_mirror_s, sp_mirror_t;
    uint8_t  sp_attr_persp, sp_attr_q29, sp_q29_shift, sp_axis;
    uint8_t  sp_header_supported, sp_z_write, sp_z_test, sp_direct_affine;
    int32_t  pl_origin[16], pl_du[16], pl_dv[16];
    int32_t  attr2_du, attr2_dv;
    int32_t  light_du, light_dv;    /* 24-bit, stored sign-extended */
    int32_t  clamp0_min, clamp0_max, clamp1_min, clamp1_max;
    uint32_t sp_z_base, sp_z_major, sp_z_minor;
    uint8_t  record_count;          /* 0..4 */
    uint16_t records_left;
    uint8_t  cnt_valid, cmap_valid, s_zeroed, sstep_zeroed;
    int16_t  lane_u[4], lane_v[4];
    uint16_t lane_count[4];
    uint32_t d_fb[4], d_tex[4];
    int32_t  d_s[4], d_t[4], d_sstep[4], d_tstep[4];
    uint8_t  d_cmap[4], d_light[4];
    uint8_t  compact_direct;

    /* 0x58 emulation (optional, non-RTL) */
    uint32_t cont_hdr[29];
    uint8_t  cont_hdr_valid;

    /* texture / palookup line cache */
    tex_line_t cache[1024];
};

/* ------------------------------------------------------------------ */
/* memory                                                               */
/* ------------------------------------------------------------------ */
static inline uint8_t mem_peek(gpu_t *g, uint32_t a)      /* diagnostics only: not a GPU read */
{
    a &= g->mem.mask;
    if (g->mem.base) return g->mem.base[a];
    return g->mem.read8(g->mem.ctx, a);
}

static inline uint8_t mem_rd(gpu_t *g, uint32_t a)
{
    a &= g->mem.mask;
    if (g->lint) lint_gpu_rd(g->lint, a);
    if (g->mem.base) return g->mem.base[a];
    return g->mem.read8(g->mem.ctx, a);
}

static inline void mem_wr(gpu_t *g, uint32_t a, uint8_t v)
{
    a &= g->mem.mask;
    if (g->mem.base) g->mem.base[a] = v;
    else g->mem.write8(g->mem.ctx, a, v);
    if (g->lint) lint_gpu_wr(g->lint, a);
}

/* gpu_tex_cache.v: addr[3:0] byte in line, addr[13:4] set, addr[25:14] tag.
 * Fill = one 4-word burst of the aligned line; never updated by GPU writes. */
static uint8_t cache_rd(gpu_t *g, uint32_t a)
{
    a = ADDR26(a);
    if (!g->cfg.tex_cache)
        return mem_rd(g, a);
    tex_line_t *l = &g->cache[(a >> 4) & 0x3FF];
    uint32_t tag = a >> 14;
    if (l->valid && l->tag == tag) {
        g->st.tex_cache_hits++;
        if (l->data[a & 0xF] != mem_peek(g, a)) COV(g, COV_STALE_TEXEL);
    } else {
        uint32_t line = a & ~0xFu;
        for (int i = 0; i < 16; i++)
            l->data[i] = mem_rd(g, line + (uint32_t)i);
        l->tag = tag;
        l->valid = 1;
        g->st.tex_cache_fills++;
    }
    return l->data[a & 0xF];
}

static void cache_flush(gpu_t *g)
{
    for (int i = 0; i < 1024; i++) g->cache[i].valid = 0;
}

/* ------------------------------------------------------------------ */
/* fixed-point helpers (exact RTL arithmetic)                           */
/* ------------------------------------------------------------------ */
static inline int32_t sext26(uint32_t v) { return (int32_t)(v << 6) >> 6; }
static inline int32_t sext24(uint32_t v) { return (int32_t)(v << 8) >> 8; }
static inline int32_t mul32(int32_t a, int32_t b)   /* dsp_p[31:0] */
{
    return (int32_t)(uint32_t)((int64_t)a * (int64_t)b);
}
static inline int32_t add32(int32_t a, int32_t b) { return (int32_t)((uint32_t)a + (uint32_t)b); }

/* span_flags_from_wire() */
static uint8_t flags_from_wire(const gpu_t *g, uint32_t f)
{
    uint8_t r = 0;
    if (g->cfg.palette && (f & 0x01)) r |= SPF_COLORMAP;
    if (f & 0x04) r |= SPF_SKIP0;
    if (f & 0x20) r |= SPF_PERSP;
    if (g->cfg.transluc && (f & 0x40)) r |= SPF_TRANSLUC;
    return r;
}

/* mirror_idx() */
static inline uint16_t mirror_idx(uint16_t raw, uint16_t mask, uint16_t octave, int en)
{
    uint16_t w = raw & mask;
    if (en && (raw & octave)) return mask ^ w;
    return w;
}

/* q29_restore_z_saturating(): value << shift, saturating on lost sign bits */
static int32_t q29_restore(int32_t value, unsigned shift)
{
    shift &= 31;
    if (shift == 0) return value;
    int64_t r = (int64_t)value * ((int64_t)1 << shift);
    if (r > INT32_MAX || r < INT32_MIN)
        return value < 0 ? INT32_MIN : INT32_MAX;   /* saturated */
    return (int32_t)r;
}

/* advance_z_value() */
static inline int32_t advance_z(int32_t v, int32_t step, int sat)
{
    uint32_t sum = (uint32_t)v + (uint32_t)step;
    if (sat && ((v < 0) == (step < 0)) && (((int32_t)sum < 0) != (v < 0)))
        return v < 0 ? INT32_MIN : INT32_MAX;
    return (int32_t)sum;
}

/* clz32_fn(): 0 and 1 both return 31 */
static inline unsigned clz32_rtl(uint32_t v)
{
    if (v <= 1) return 31;
    return (unsigned)__builtin_clz(v);
}

/* PSS reciprocal: recip_lut + funnel (PSS_RECIP_SHIFT) + one Newton-Raphson
 * step (PSS_NR_*).  Q29 keeps 4 extra fractional bits.  The Q29 recip cache
 * (GPU_PSS_RECIP_CACHE) returns exactly this value, so it is transparent. */
static int32_t pss_recip(uint32_t x, int q29, int bias)
{
    unsigned clz = clz32_rtl(x);
    uint32_t norm = x << clz;
    uint32_t top10 = (norm >> 21) & 0x3FF;
    uint32_t lut = 16777216u / (1024u + top10);
    unsigned shamt = q29 ? clz + 4 : clz;
    uint64_t funnel = ((uint64_t)lut << shamt) & ((1ull << 45) - 1);
    int32_t y0 = (int32_t)(uint32_t)(funnel >> 13);
    int64_t xy = (int64_t)(int32_t)x * (int64_t)y0;
    int32_t xy_q16 = q29 ? (int32_t)(uint32_t)(uint64_t)(xy >> 20)
                         : (int32_t)(uint32_t)(uint64_t)(xy >> 16);
    int32_t two_minus = (int32_t)(0x20000u - (uint32_t)xy_q16);
    int64_t p = (int64_t)y0 * (int64_t)two_minus;
    int32_t y1 = (int32_t)(uint32_t)(uint64_t)(p >> 16);
    if (!q29 && bias) y1 = add32(y1, 1);
    return y1;
}

/* PSS_FINAL / PSS_CONSTZ_STEP_CAPTURE rounding projection */
static inline int32_t pss_proj(int32_t v, int32_t recip, int q29)
{
    int64_t p = (int64_t)v * (int64_t)recip;
    p += q29 ? (1ll << 19) : (1ll << 15);
    return (int32_t)(uint32_t)(uint64_t)(p >> (q29 ? 20 : 16));
}

static const uint32_t pss_div_recip32[16] = {
    0, 0, 0, 1431655766u, 0, 858993460u, 715827883u, 613566757u,
    0, 477218589u, 429496730u, 390451573u, 357913942u, 330382100u,
    306783379u, 286331154u,
};

/* PSS_SLOPE_PREP .. PSS_SLOPE_DIV_STEP_COMMIT */
static int32_t pss_slope(gpu_t *g, int32_t delta, unsigned div)
{
    if (delta < 0 && div != 16) COV(g, COV_SLOPE_NEG_DELTA);
    if (div == 1 || div == 2 || div == 4 || div == 8) {
        COV(g, COV_SLOPE_POW2);
        unsigned sh = div == 1 ? 0 : div == 2 ? 1 : div == 4 ? 2 : 3;
        int32_t bias = delta < 0 ? (int32_t)((1u << sh) - 1u) : 0;
        return add32(delta, bias) >> sh;       /* pss_div_pow2_trunc */
    }
    if (div < 16) {
        int neg = delta < 0;
        COV(g, COV_SLOPE_DIVMAG);
        uint32_t mag = neg ? (uint32_t)0 - (uint32_t)delta : (uint32_t)delta;
        int64_t p = (int64_t)(int32_t)mag * (int64_t)(int32_t)pss_div_recip32[div];
        uint32_t q = (uint32_t)((uint64_t)p >> 32);
        int64_t c = (int64_t)(int32_t)q * (int64_t)div;
        uint64_t c37 = (uint64_t)c & ((1ull << 37) - 1);
        if (c37 > (uint64_t)mag) { q -= 1; COV(g, COV_SLOPE_DIV_CORR); }
        return neg ? (int32_t)((uint32_t)0 - q) : (int32_t)q;
    }
    COV(g, COV_SLOPE_FULL16);
    return delta >> 4;                         /* full segment: >>> 4 */
}

/* ------------------------------------------------------------------ */
/* per-pixel fragment processing                                        */
/* ------------------------------------------------------------------ */
typedef struct span_ctx {
    uint32_t tex_base;
    uint16_t tex_width;
    uint16_t w_mask, h_mask, w_oct, h_oct;
    uint8_t  mirror_s, mirror_t;
    uint8_t  flags;
    uint8_t  cmap_id;
    uint8_t  clamp_en0, clamp_en1;
    int16_t  cmin0, cmax0, cmin1, cmax1;
    uint8_t  z_write, z_test, q29z;
    int32_t  fb_step, z_step;
    int32_t  light_step;   /* 24-bit */
    int32_t  z_value_step;
    uint8_t  opcode;
} span_ctx_t;

typedef struct pix_state {
    uint32_t fb;       /* 26 */
    uint32_t zaddr;    /* 26 */
    int32_t  zvalue;
    int32_t  light_q;  /* 24-bit */
} pix_state_t;

#ifdef GPU_TRACE
FILE *gpu_trace_file;   /* per-pixel source trace for RTL differential debug */
#endif

static void fragment(gpu_t *g, const span_ctx_t *c, const pix_state_t *ps,
                     int32_t s, int32_t t)
{
#ifdef GPU_TRACE
    if (gpu_trace_file)
        fprintf(gpu_trace_file, "%07x %08x %08x %08x %06x\n", ps->fb, (uint32_t)s, (uint32_t)t,
                (uint32_t)ps->zvalue, (uint32_t)ps->light_q & 0xFFFFFF);
#endif
    /* source snapshot: 16-bit clamps on the integer parts */
    int16_t s16 = (int16_t)((uint32_t)s >> 16);
    int16_t t16 = (int16_t)((uint32_t)t >> 16);
    if (c->clamp_en0) {
        if (s16 < c->cmin0) { s16 = c->cmin0; COV(g, COV_TEXCLAMP_MIN); }
        else if (s16 > c->cmax0) { s16 = c->cmax0; COV(g, COV_TEXCLAMP_MAX); }
    }
    if (c->clamp_en1) {
        if (t16 < c->cmin1) { t16 = c->cmin1; COV(g, COV_TEXCLAMP_MIN); }
        else if (t16 > c->cmax1) { t16 = c->cmax1; COV(g, COV_TEXCLAMP_MAX); }
    }
    if ((c->mirror_s && ((uint16_t)s16 & c->w_oct)) || (c->mirror_t && ((uint16_t)t16 & c->h_oct)))
        COV(g, COV_MIRROR_REV);
    uint16_t s_int = mirror_idx((uint16_t)s16, c->w_mask, c->w_oct, c->mirror_s);
    uint16_t t_y   = mirror_idx((uint16_t)t16, c->h_mask, c->h_oct, c->mirror_t);
    int32_t  txmul = (int32_t)(int16_t)t_y * (int32_t)c->tex_width;
    uint32_t off   = ADDR26((uint32_t)txmul + (uint32_t)(int32_t)(int16_t)s_int);
    uint32_t taddr = ADDR26(c->tex_base + off);

    g->st.pixels++;
    g->st.texels_fetched++;
    uint8_t texel = cache_rd(g, taddr);
    uint8_t light6 = (uint8_t)(((uint32_t)ps->light_q >> 16) & 0x3F);
    uint16_t z_half = c->q29z ? (uint16_t)((uint32_t)ps->zvalue >> 14)
                              : (uint16_t)((uint32_t)ps->zvalue >> 1);

    /* write-only z (z_src_pending path): every source pixel, before colour */
    if (c->z_write && !c->z_test) {
        uint32_t za = ps->zaddr & ~1u;
        COV(g, COV_ZWRITE_ONLY);
        if (ps->zaddr & 1) COV(g, COV_ODD_ZADDR);
        mem_wr(g, za, (uint8_t)z_half);
        mem_wr(g, za + 1, (uint8_t)(z_half >> 8));
        g->st.z_writes++;
    }

    int discard = (c->flags & SPF_SKIP0) && texel == 0xFF;
    uint8_t color = texel;
    if (c->flags & SPF_COLORMAP) {
        /* p1->p2: request issued regardless of discard */
        uint32_t page = ((g->palookup_base >> 14) + c->cmap_id) & 0xFFF;
        if ((g->palookup_base >> 14) + c->cmap_id > 0xFFF) COV(g, COV_CMAP_PAGE_WRAP);
        uint32_t ca = (page << 14) | ((uint32_t)light6 << 8) | texel;
        g->st.cmap_lookups++;
        color = cache_rd(g, ca);
    }
    if (discard) {
        g->st.skip_zero_discards++;
        return;
    }
    if (c->flags & SPF_TRANSLUC) {
        /* FBSS_BLEND_*: dst read, transluc[{src[7:1], dst}]; z ignored */
        uint8_t dst = mem_rd(g, ps->fb);
        uint32_t key = ((uint32_t)(color >> 1) << 8) | dst;
        uint8_t out = (uint8_t)(g->transluc[key >> 2] >> ((key & 3) * 8));
        mem_wr(g, ps->fb, out);
        g->st.transluc_blends++;
        if (c->flags & SPF_COLORMAP) COV(g, COV_TRANSLUC_CMAP);
        g->st.pixels_written++;
        return;
    }
    if (c->z_test) {
        uint32_t za = ps->zaddr & ~1u;
        uint16_t old = (uint16_t)(mem_rd(g, za) | (mem_rd(g, za + 1) << 8));
        g->st.z_tests++;
        if (!(z_half >= old)) {
            g->st.z_test_fail++;
            return;
        }
        if (c->z_write) {
            mem_wr(g, za, (uint8_t)z_half);
            mem_wr(g, za + 1, (uint8_t)(z_half >> 8));
            g->st.z_writes++;
            COV(g, COV_ZTEST_PASS_WRITE);
        }
    }
    mem_wr(g, ps->fb, color);
    g->st.pixels_written++;
}

static inline void pix_advance(gpu_t *g, const span_ctx_t *c, pix_state_t *ps)
{
    if (c->z_write || c->z_test) {
        ps->zaddr = ADDR26(ps->zaddr + (uint32_t)c->z_step);
        int32_t nz = advance_z(ps->zvalue, c->z_value_step, c->q29z);
        if (c->q29z && nz != add32(ps->zvalue, c->z_value_step)) COV(g, COV_ZADV_SAT);
        ps->zvalue = nz;
    }
    ps->fb = ADDR26(ps->fb + (uint32_t)c->fb_step);
    ps->light_q = sext24((uint32_t)ps->light_q + (uint32_t)c->light_step);
}

static void affine_loop(gpu_t *g, const span_ctx_t *c, pix_state_t *ps,
                        uint32_t count, int32_t s, int32_t t,
                        int32_t sstep, int32_t tstep)
{
    for (uint32_t i = 0; i < count; i++) {
        fragment(g, c, ps, s, t);
        pix_advance(g, c, ps);
        s = add32(s, sstep);
        t = add32(t, tstep);
    }
}

/* Perspective span: PSS anchor pass, slot-A pass, then one slot-B pass per
 * following 16-pixel segment (gpu_core.v PSS_* sub-FSM). */
typedef struct persp_st {
    int32_t sZ, tZ, zinv;
    int32_t sZstep, tZstep, zstep;
    int32_t anchor_s, anchor_t;
    int     q29;
} persp_st_t;

static void pss_pass(gpu_t *g, persp_st_t *p, int fc,
                     int32_t *start_s, int32_t *start_t,
                     int32_t *step_s, int32_t *step_t)
{
    unsigned adv, div;
    g->st.persp_passes++;
    if (!p->q29) {
        adv = 16; div = 16;
    } else if (fc > 16) {
        adv = 16; div = 16;
    } else if (fc > 1) {
        adv = (unsigned)fc - 1; div = adv;
    } else {
        adv = 0; div = 1;
    }
    int prev_neg = p->zinv < 0;
    int prev_nz  = p->zinv != 0;
    uint32_t abs_na = p->zinv < 0 ? (uint32_t)0 - (uint32_t)p->zinv : (uint32_t)p->zinv;
    p->sZ   = add32(p->sZ,   mul32(p->sZstep, (int32_t)adv));
    p->tZ   = add32(p->tZ,   mul32(p->tZstep, (int32_t)adv));
    p->zinv = add32(p->zinv, mul32(p->zstep,  (int32_t)adv));
    uint32_t absz = p->zinv < 0 ? (uint32_t)0 - (uint32_t)p->zinv : (uint32_t)p->zinv;
    int flip = ((p->zinv < 0) != prev_neg) && prev_nz && p->zinv != 0;
    int clamp = p->q29 ? (absz == 0 || flip) : (flip || absz < (abs_na >> 2));
    if (clamp) COV(g, p->q29 ? COV_CLAMP_SING_Q29 : (flip ? COV_CLAMP_SING_Q16 : COV_CLAMP_RATIO_Q16));
    if (p->q29 && adv != 16) COV(g, COV_TAIL_SEG);
    if (absz == 0) COV(g, COV_RECIP_ZERO);
    int32_t recip = pss_recip(absz, p->q29, 0);
    g->st.recip_computes++;
    int32_t end_s = clamp ? p->anchor_s : pss_proj(p->sZ, recip, p->q29);
    int32_t end_t = clamp ? p->anchor_t : pss_proj(p->tZ, recip, p->q29);
    int32_t ds = (int32_t)((uint32_t)end_s - (uint32_t)p->anchor_s);
    int32_t dt = (int32_t)((uint32_t)end_t - (uint32_t)p->anchor_t);
    *start_s = p->anchor_s;
    *start_t = p->anchor_t;
    *step_s = pss_slope(g, ds, div);
    *step_t = pss_slope(g, dt, div);
    p->anchor_s = end_s;
    p->anchor_t = end_t;
}

static void persp_loop(gpu_t *g, span_ctx_t *c, pix_state_t *ps, uint32_t count,
                       int32_t sZ, int32_t tZ, int32_t zinv,
                       int32_t sZstep, int32_t tZstep, int32_t zstep, int q29)
{
    persp_st_t p = { sZ, tZ, zinv, sZstep, tZstep, zstep, 0, 0, q29 };
    g->st.spans_persp++;
    if (q29) g->st.spans_q29++;

    /* pass 1: anchor at pixel 0 (no advance, no clamp) */
    uint32_t absz = zinv < 0 ? (uint32_t)0 - (uint32_t)zinv : (uint32_t)zinv;
    int step_zero = (zstep == 0);
    int32_t recip = pss_recip(absz, q29, step_zero && zinv != 0x10000);
    if (!q29 && step_zero && zinv != 0x10000) COV(g, COV_RECIP_BIAS);
    if (!q29) COV(g, COV_PERSP_Q16);
    if (absz == 0) COV(g, COV_RECIP_ZERO);
    g->st.recip_computes++;
    g->st.persp_passes++;
    int32_t s_end = pss_proj(sZ, recip, q29);
    int32_t t_end = pss_proj(tZ, recip, q29);
    if (step_zero) {
        /* constant-Z: one reciprocal converts the span to affine */
        g->st.spans_persp_constz++;
        if (q29) COV(g, COV_CONSTZ_Q29);
        affine_loop(g, c, ps, count, s_end, t_end,
                    pss_proj(sZstep, recip, q29), pss_proj(tZstep, recip, q29));
        return;
    }
    p.anchor_s = s_end;
    p.anchor_t = t_end;

    uint32_t done = 0;
    int32_t s, t, ss, ts;
    pss_pass(g, &p, (int)count, &s, &t, &ss, &ts);          /* slot A */
    for (uint32_t seg = 0; done < count; seg++) {
        if (seg > 0) {
            int fc = (int)count - 16 * (int)seg;             /* slot B */
            pss_pass(g, &p, fc < 0 ? 0 : fc, &s, &t, &ss, &ts);
        }
        uint32_t npix = count - done;
        if (npix > 16) npix = 16;
        for (uint32_t j = 0; j < npix; j++) {
            fragment(g, c, ps, s, t);
            pix_advance(g, c, ps);
            if (j != 15) {           /* sp_seg_left != 0: advance within segment */
                s = add32(s, ss);
                t = add32(t, ts);
            }
        }
        done += npix;
    }
}

/* ------------------------------------------------------------------ */
/* span producer (S_SPANPROD_SELECT .. S_SPANPROD_EMIT)                  */
/* ------------------------------------------------------------------ */
static void draw_record(gpu_t *g, unsigned idx, uint8_t opcode)
{
    uint16_t count = (g->cnt_valid >> idx) & 1 ? g->lane_count[idx] : 0;
    if (count == 0)
        return;
    g->st.records++;
    g->st.cmd_pixels[opcode] += count;

    span_ctx_t c;
    memset(&c, 0, sizeof(c));
    pix_state_t ps;
    memset(&ps, 0, sizeof(ps));
    c.opcode = opcode;
    c.tex_width = g->sp_tex_width_nz ? g->sp_tex_width : 1;
    if (!g->sp_tex_width_nz) COV(g, COV_TEXWIDTH_ZERO);
    c.w_mask = g->sp_w_mask;
    c.h_mask = g->sp_h_mask;
    c.w_oct = (uint16_t)(g->sp_w_mask + 1);
    c.h_oct = (uint16_t)(g->sp_h_mask + 1);
    c.fb_step = sext26(g->sp_fb_minor);

    if (g->sp_direct_affine) {
        c.tex_base = g->d_tex[idx];
        c.cmap_id = (g->cmap_valid >> idx) & 1 ? g->d_cmap[idx] : 0;
        c.flags = g->sp_flags & (uint8_t)~SPF_PERSP;
        ps.fb = g->d_fb[idx];
        ps.light_q = (int32_t)((uint32_t)g->d_light[idx] << 16);
        int32_t s  = (g->s_zeroed >> idx) & 1 ? 0 : g->d_s[idx];
        if ((g->s_zeroed >> idx) & 1) COV(g, COV_COLUMN_ZERO_S);
        int32_t ss = (g->sstep_zeroed >> idx) & 1 ? 0 : g->d_sstep[idx];
        g->st.spans_affine++;
        affine_loop(g, &c, &ps, count, s, g->d_t[idx], ss, g->d_tstep[idx]);
        return;
    }

    int32_t u = g->lane_u[idx], v = g->lane_v[idx];
    int axis = g->sp_axis;
    if (axis) COV(g, COV_AXIS_Y);
    if (u < 0 || v < 0) COV(g, COV_NEG_UV);
    int32_t major = sext26(g->sp_fb_major), minor = sext26(g->sp_fb_minor);
    ps.fb = ADDR26(g->sp_fb_base + (uint32_t)mul32(axis ? u : v, major)
                                 + (uint32_t)mul32(axis ? v : u, minor));
    c.z_write = g->sp_z_write;
    c.z_test = g->sp_z_test;
    if (c.z_write || c.z_test) {
        int32_t zmaj = sext26(g->sp_z_major), zmin = sext26(g->sp_z_minor);
        ps.zaddr = ADDR26(g->sp_z_base + (uint32_t)mul32(axis ? u : v, zmaj)
                                       + (uint32_t)mul32(axis ? v : u, zmin));
    }
    int32_t attr[3];
    for (int i = 0; i < 3; i++)
        attr[i] = add32(add32(g->pl_origin[i + 1], mul32(u, g->pl_du[i + 1])),
                        mul32(v, g->pl_dv[i + 1]));
    if (!g->sp_attr_persp)
        attr[2] = 0;
    if (g->sp_flags & SPF_COLORMAP)
        ps.light_q = sext24((uint32_t)add32(add32(g->pl_origin[4], mul32(u, g->pl_du[4])),
                                            mul32(v, g->pl_dv[4])));
    c.light_step = axis ? g->light_dv : g->light_du;
    c.tex_base = g->sp_tex_addr;
    c.cmap_id = g->sp_colormap_id;
    c.mirror_s = g->sp_mirror_s;
    c.mirror_t = g->sp_mirror_t;
    c.flags = g->sp_attr_persp ? (g->sp_flags | SPF_PERSP) : (g->sp_flags & (uint8_t)~SPF_PERSP);
    c.clamp_en0 = g->clamp0_min != 0 || g->clamp0_max != 0;
    c.clamp_en1 = g->clamp1_min != 0 || g->clamp1_max != 0;
    c.cmin0 = (int16_t)((uint32_t)g->clamp0_min >> 16);
    c.cmax0 = (int16_t)((uint32_t)g->clamp0_max >> 16);
    c.cmin1 = (int16_t)((uint32_t)g->clamp1_min >> 16);
    c.cmax1 = (int16_t)((uint32_t)g->clamp1_max >> 16);
    c.z_step = sext26(g->sp_z_minor);
    int32_t a2step = axis ? g->attr2_dv : g->attr2_du;
    c.q29z = g->cfg.param_q29 && g->sp_attr_q29 && (c.z_write || c.z_test);
    ps.zvalue = c.q29z ? q29_restore(attr[2], g->sp_q29_shift) : attr[2];
    if (c.q29z && (q29_restore(attr[2], g->sp_q29_shift) != (int32_t)((uint32_t)attr[2] << (g->sp_q29_shift & 31))))
        COV(g, COV_ZRESTORE_SAT);
    c.z_value_step = c.q29z ? q29_restore(a2step, g->sp_q29_shift) : a2step;

    int32_t a0step = axis ? g->pl_dv[1] : g->pl_du[1];
    int32_t a1step = axis ? g->pl_dv[2] : g->pl_du[2];
    if (g->sp_attr_persp) {
        persp_loop(g, &c, &ps, count, attr[0], attr[1], attr[2],
                   a0step, a1step, a2step, g->sp_attr_q29);
    } else {
        g->st.spans_affine++;
        affine_loop(g, &c, &ps, count, attr[0], attr[1], a0step, a1step);
    }
}

/* ------------------------------------------------------------------ */
/* payload loaders                                                      */
/* ------------------------------------------------------------------ */
static void load_span_word(gpu_t *g, unsigned idx, uint32_t d)
{
    if (g->cfg.compact_span && g->compact_direct) {
        if (idx == 0) {
            unsigned nib = d >> 28;
            unsigned rc = nib >= 4 ? 3 : nib == 3 ? 2 : nib == 2 ? 1 : 0;
            g->sp_direct_affine = 1;
            g->record_count = (uint8_t)(rc + 1);
            g->records_left = (uint16_t)(rc + 1);
            g->sp_flags = flags_from_wire(g, (d >> 20) & 0xFF);
            g->sp_mirror_s = g->sp_mirror_t = 0;
            g->sp_attr_persp = g->sp_attr_q29 = 0;
            g->sp_q29_shift = 0;
            g->sp_axis = 0;
            g->sp_header_supported = 1;
            g->sp_z_write = g->sp_z_test = 0;
            g->clamp0_min = g->clamp0_max = g->clamp1_min = g->clamp1_max = 0;
            g->cnt_valid = 0;
            g->cmap_valid = 0;
            return;
        }
        if (idx == 1) { g->sp_tex_width = (uint16_t)d; g->sp_tex_width_nz = (uint16_t)d != 0; return; }
        if (idx == 2) {
            g->sp_w_mask = (uint16_t)d ? (uint16_t)d : 0xFFFF;
            g->sp_h_mask = (uint16_t)(d >> 16) ? (uint16_t)(d >> 16) : 0xFFFF;
            return;
        }
        if (idx == 3) { g->sp_fb_minor = ADDR26(d); return; }
        if (idx >= 4 && idx <= 31) {
            unsigned lane = (idx - 4) / 7, f = (idx - 4) % 7;
            switch (f) {
            case 0: g->d_fb[lane] = ADDR26(d); break;
            case 1: g->d_tex[lane] = ADDR26(d); break;
            case 2:
                g->lane_count[lane] = (uint16_t)d;
                g->d_light[lane] = (uint8_t)((d >> 16) & 0x3F);
                g->d_cmap[lane] = (uint8_t)(d >> 28);
                g->cnt_valid |= (uint8_t)(1u << lane);
                g->cmap_valid |= (uint8_t)(1u << lane);
                break;
            case 3: g->d_s[lane] = (int32_t)d; g->s_zeroed &= (uint8_t)~(1u << lane); break;
            case 4: g->d_t[lane] = (int32_t)d; break;
            case 5: g->d_sstep[lane] = (int32_t)d; g->sstep_zeroed &= (uint8_t)~(1u << lane); break;
            case 6: g->d_tstep[lane] = (int32_t)d; break;
            }
        }
        return;
    }

    switch (idx) {
    case 0: g->sp_fb_base = ADDR26(d); break;
    case 1: g->sp_fb_major = ADDR26(d); break;
    case 2: g->sp_fb_minor = ADDR26(d); break;
    case 3: g->sp_tex_addr = ADDR26(d); break;
    case 4: g->sp_tex_width = (uint16_t)d; g->sp_tex_width_nz = (uint16_t)d != 0; break;
    case 5: g->sp_w_mask = (uint16_t)d ? (uint16_t)d : 0xFFFF; break;
    case 6: g->sp_h_mask = (uint16_t)d ? (uint16_t)d : 0xFFFF; break;
    case 7: {
        int fmt0 = ((d >> 20) & 0xF) == 0;
        g->sp_direct_affine = 0;
        g->sp_flags = flags_from_wire(g, d & 0xFF);
        g->sp_colormap_id = (uint8_t)((d >> 8) & 0xF);
        g->sp_mirror_s = (d >> 28) & 1;
        g->sp_mirror_t = (d >> 29) & 1;
        g->sp_attr_persp = ((d >> 12) & 1) && fmt0;
        g->sp_attr_q29 = g->cfg.param_q29 && ((d >> 13) & 1) && ((d >> 12) & 1) && fmt0;
        g->sp_q29_shift = 0;
        g->sp_axis = (d >> 16) & 1;
        g->sp_z_write = ((d >> 24) & 1) && fmt0;
        g->sp_z_test = ((d >> 25) & 1) && fmt0;
        {
            int b = fmt0 && ((d >> 17) & 7) == 0 && !((d >> 15) & 1) && !((d >> 14) & 1)
                 && (!((d >> 13) & 1) || ((d >> 12) & 1))
                 && !((d >> 27) & 1) && !((d >> 26) & 1);
            if (b && (((d >> 25) & 1) || ((d >> 24) & 1))) {
                b = ((d >> 12) & 1) && !((d >> 6) & 1)
                 && (!((d >> 24) & 1) || ((d >> 25) & 1) || !((d >> 2) & 1));
            }
            g->sp_header_supported = (uint8_t)b;
        }
        break;
    }
    case 8:  g->pl_origin[1] = (int32_t)d; break;
    case 9:  g->pl_du[1] = (int32_t)d; break;
    case 10: g->pl_dv[1] = (int32_t)d; break;
    case 11: g->pl_origin[2] = (int32_t)d; break;
    case 12: g->pl_du[2] = (int32_t)d; break;
    case 13: g->pl_dv[2] = (int32_t)d; break;
    case 14: g->pl_origin[3] = (int32_t)d; break;
    case 15: g->pl_du[3] = (int32_t)d; g->attr2_du = (int32_t)d; break;
    case 16: g->pl_dv[3] = (int32_t)d; g->attr2_dv = (int32_t)d; break;
    case 17: g->pl_origin[4] = sext24(d); break;
    case 18: g->pl_du[4] = sext24(d); g->light_du = sext24(d); break;
    case 19: g->pl_dv[4] = sext24(d); g->light_dv = sext24(d); break;
    case 20: g->clamp0_min = (int32_t)d; break;
    case 21: g->clamp0_max = (int32_t)d; break;
    case 22: g->clamp1_min = (int32_t)d; break;
    case 23: g->clamp1_max = (int32_t)d; break;
    case 26: g->sp_z_base = ADDR26(d); break;
    case 27: g->sp_z_major = ADDR26(d); break;
    case 28: g->sp_z_minor = ADDR26(d); break;
    case 29: {
        uint16_t n = (uint16_t)d;
        if (n == 0) { g->record_count = 0; g->records_left = 0; }
        else if (n >= 4) { g->record_count = 4; g->records_left = n; }
        else { g->record_count = (uint8_t)(n & 3); g->records_left = n; }
        g->cnt_valid = 0;
        g->cmap_valid = 0;
        break;
    }
    case 30:
        g->sp_q29_shift = g->sp_attr_q29 ? (uint8_t)(d & 31) : 0;
        if ((d >> 5) != 0 || (!g->sp_attr_q29 && (d & 31) != 0))
            g->sp_header_supported = 0;
        break;
    case 31: g->lane_u[0] = (int16_t)d; g->lane_v[0] = (int16_t)(d >> 16); break;
    case 32: g->lane_count[0] = (uint16_t)d; g->cnt_valid |= 1; g->lane_u[1] = (int16_t)(d >> 16); break;
    case 33: g->lane_v[1] = (int16_t)d; g->lane_count[1] = (uint16_t)(d >> 16); g->cnt_valid |= 2; break;
    case 34: g->lane_u[2] = (int16_t)d; g->lane_v[2] = (int16_t)(d >> 16); break;
    case 35: g->lane_count[2] = (uint16_t)d; g->cnt_valid |= 4; g->lane_u[3] = (int16_t)(d >> 16); break;
    case 36: g->lane_v[3] = (int16_t)d; g->lane_count[3] = (uint16_t)(d >> 16); g->cnt_valid |= 8; break;
    default: break;
    }
}

/* column_compact_idx_remap() */
static unsigned column_remap(unsigned idx)
{
    static const uint8_t map[24] = {
        0, 1, 2, 3, 4, 5, 6, 8, 10, 11, 12, 13, 15, 17,
        18, 19, 20, 22, 24, 25, 26, 27, 29, 31,
    };
    return idx < 24 ? map[idx] : 62;
}

static unsigned last_idx(const gpu_t *g)
{
    return g->record_count >= 4 ? 3 : g->record_count == 3 ? 2 : g->record_count == 2 ? 1 : 0;
}

/* S_PAY_DATA / S_EXECUTE / record walk / spanprod_prepare_next_record_chunk
 * for 0x48 (both forms) and 0x4C. */
static void exec_span_col(gpu_t *g, uint8_t op, const uint32_t *p, uint32_t n)
{
    int is_col = (op == CMD_COLUMN_LIST);
    int is_psl = (op == CMD_PARAM_SPAN_LIST);   /* cmd_is_draw_param_span_list */
    unsigned pay_idx = 0;
    uint32_t remaining = n, wi = 0;
    int drew = 0;

    for (;;) {
        /* S_PAY_DATA */
        int to_exec;
        for (;;) {
            uint32_t d = p[wi++];
            unsigned cur = pay_idx;
            load_span_word(g, is_col ? column_remap(cur) : cur, d);
            if (is_col && cur == 0) {
                g->s_zeroed = 0xF;
                g->sstep_zeroed = 0xF;
            }
            if (pay_idx != 63) pay_idx++;
            uint32_t rem = remaining--;
            if (rem <= 1) { to_exec = 1; break; }
            if (is_psl && !g->sp_direct_affine && g->sp_header_supported
                && g->records_left > g->record_count && cur == 36) {
                to_exec = 1;
                break;
            }
        }
        (void)to_exec;
        /* S_EXECUTE (CMDCLS_SPAN_COL) */
        if (!g->sp_header_supported) COV(g, COV_HDR_UNSUPPORTED);
        if (!(g->sp_header_supported && g->record_count != 0))
            break;
        drew = 1;
        unsigned li = last_idx(g);
        for (unsigned idx = 0; idx <= li; idx++)
            draw_record(g, idx, op);
        if (g->records_left > g->record_count && remaining != 0) {
            uint16_t left = (uint16_t)(g->records_left - g->record_count);
            g->records_left = left;
            g->record_count = left >= 4 ? 4 : (uint8_t)(left & 3);
            g->cnt_valid = 0;
            pay_idx = 31;
            COV(g, COV_CHUNK_CONT);
            continue;
        }
        if (g->records_left > g->record_count) COV(g, COV_LONG_RECORDS_CUT);
        break;
    }
    if (!drew) g->st.cmd_nop[op]++;
    /* drain whatever payload the RTL would still consume (none: the loop
     * above always runs to remaining == 0 or stops at a chunk end with
     * no more records, where the RTL retires to IDLE leaving no payload). */
    (void)wi;
}

/* S_CLEAR_RECT / S_CLEAR_RECT_WORD */
static void exec_clear_rect(gpu_t *g)
{
    g->st.clear_rects++;
    if (g->cr_y_remaining == 0 || g->cr_w_total == 0)
        return;
    for (;;) {
        unsigned lane = g->cr_addr & 3;
        unsigned bytes;
        if (g->cr_w_remaining >= 4 && lane == 0) bytes = 4;
        else if (g->cr_w_remaining >= 4 - lane) bytes = 4 - lane;
        else bytes = g->cr_w_remaining & 7;
        /* strobe ((1<<bytes)-1)<<lane on a 4-bit bus */
        uint32_t word = g->cr_addr & ~3u;
        unsigned strobe = (((1u << bytes) - 1u) << lane) & 0xF;
        if (bytes != 4) COV(g, COV_CLEAR_PARTIAL_WORD);
        for (unsigned b = 0; b < 4; b++)
            if (strobe & (1u << b)) {
                mem_wr(g, word + b, g->cr_color);
                g->st.clear_bytes++;
            }
        if (g->cr_w_remaining <= bytes) {
            if (g->cr_y_remaining == 1)
                return;
            uint16_t stride = g->cr_stride ? g->cr_stride : g->st_fb_stride;
            if (!g->cr_stride) COV(g, COV_CLEAR_STRIDE_FALLBACK);
            g->cr_row_addr = ADDR26(g->cr_row_addr + stride);
            g->cr_addr = g->cr_row_addr;
            g->cr_w_remaining = g->cr_w_total;
            g->cr_y_remaining--;
        } else {
            g->cr_addr = ADDR26(g->cr_addr + bytes);
            g->cr_w_remaining = (uint16_t)(g->cr_w_remaining - bytes);
        }
    }
}

/* 0x4A payload into the shared staging (decoded on every variant). */
static void exec_set_tri_state(gpu_t *g, const uint32_t *p, uint32_t n)
{
    static const int8_t map[17] = { 0, 1, 2, 3, 4, -2, 7, 20, 21, 22, 23, 26, 27, 28, -1, -1, -1 };
    g->compact_direct = 0;
    for (uint32_t i = 0; i < n && i < 17; i++) {
        if (map[i] >= 0) load_span_word(g, (unsigned)map[i], p[i]);
        else if (map[i] == -2) {
            g->sp_w_mask = (uint16_t)p[i] ? (uint16_t)p[i] : 0xFFFF;
            g->sp_h_mask = (uint16_t)(p[i] >> 16) ? (uint16_t)(p[i] >> 16) : 0xFFFF;
        }
    }
}

static void exec_command(gpu_t *g, uint32_t hdr, const uint32_t *p)
{
    uint8_t op = (uint8_t)(hdr >> 24);
    uint32_t n = hdr & 0x1FFF;
    if (g->cmd_cb) g->cmd_cb(g->cmd_ctx, hdr, p);
    g->st.cmd_count[op]++;
    g->st.cmd_words[op] += n;

    switch (op) {
    case CMD_FENCE:
        if (n >= 1) g->pending_fence_token = p[0];
        g->fence_reached = g->pending_fence_token;
        g->st.fences++;
        return;
    case CMD_FLIP:
        if (n >= 1) g->pending_swap_idx = (uint8_t)(p[0] & 3);
        if (n >= 2) g->pending_fence_token = p[1];
        g->fence_reached = g->pending_fence_token;
        g->st.flips++;
        if (g->flip_cb) g->flip_cb(g->flip_ctx, g->pending_swap_idx, g->pending_fence_token);
        return;
    case CMD_CLEAR_RECT:
        if (n >= 1) { g->cr_addr = ADDR26(p[0]); g->cr_row_addr = g->cr_addr; }
        if (n >= 2) {
            g->cr_w_total = (uint16_t)(p[1] >> 16);
            g->cr_w_remaining = g->cr_w_total;
            g->cr_y_remaining = (uint16_t)p[1];
        }
        if (n >= 3) { g->cr_stride = (uint16_t)(p[2] >> 16); g->cr_color = (uint8_t)p[2]; }
        exec_clear_rect(g);
        return;
    case CMD_SET_FB:
        if (n >= 2) g->st_fb_stride = (uint16_t)p[1];
        return;
    case CMD_SET_TEXTURE:
        return;                              /* drained; no datapath state */
    case CMD_PARAM_SPAN_LIST:
        g->compact_direct = g->cfg.compact_span && n <= 32;
        if (n >= 11 && n <= 32 && (n - 4) % 7) COV(g, COV_COMPACT_ODD_SIZE);
        if (n < (g->cfg.compact_span ? 11u : 33u)) { g->st.cmd_nop[op]++; return; }
        if (n >= 33) {
            for (unsigned i = 0; i < 29; i++) g->cont_hdr[i] = p[i];
            g->cont_hdr_valid = 1;
        } else {
            g->cont_hdr_valid = 0;
        }
        exec_span_col(g, op, p, n);
        return;
    case CMD_COLUMN_LIST:
        g->compact_direct = (uint8_t)(g->cfg.compact_span != 0);
        g->cont_hdr_valid = 0;
        if (!(g->cfg.compact_span && g->cfg.column_list && n >= 9 && n <= 24)) {
            g->st.cmd_nop[op]++;
            return;
        }
        exec_span_col(g, op, p, n);
        return;
    case CMD_SET_TRI_STATE:
        g->cont_hdr_valid = 0;
        if (n == 16 || n == 17) exec_set_tri_state(g, p, n);
        else g->st.cmd_nop[op]++;
        return;
    case CMD_SPAN_CONT:
        if (g->cfg.emulate_span_cont && g->cont_hdr_valid && n >= 2) {
            uint32_t pairs = n - 2, tot = 31 + pairs;
            COV(g, COV_SPAN_CONT_EMU);
            uint32_t *tmp = (uint32_t *)malloc(sizeof(uint32_t) * tot);
            if (tmp) {
                memcpy(tmp, g->cont_hdr, sizeof(g->cont_hdr));
                tmp[29] = p[0];
                tmp[30] = p[1];
                memcpy(tmp + 31, p + 2, sizeof(uint32_t) * pairs);
                g->compact_direct = 0;
                if (tot >= 33) exec_span_col(g, CMD_PARAM_SPAN_LIST, tmp, tot);
                free(tmp);
            }
            return;
        }
        g->st.cmd_nop[op]++;                 /* os25: CMDCLS_NONE drain */
        return;
    case 0x49:                               /* PARAM_TRI: clears sticky state only */
    default:
        /* 0x49/0x4B/0x4D/0x4E/0x4F/0x50-0x57 are gated out on os25
         * (INCLUDE_PARAM_TRI=0, INCLUDE_VERT_TRI=0, ...): CMDCLS_NONE. */
        g->st.cmd_nop[op]++;
        return;
    }
}

/* ------------------------------------------------------------------ */
/* public API                                                           */
/* ------------------------------------------------------------------ */
void gpu_default_config(gpu_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->compact_span = 1;
    cfg->column_list = 1;
    cfg->transluc = 1;
    cfg->palette = 1;
    cfg->param_q29 = 1;
    cfg->cpu_ring = 1;
    cfg->command_dma = 0;
    cfg->tex_cache = 1;
    cfg->emulate_span_cont = 0;
}

gpu_t *gpu_create(const gpu_config_t *cfg, const gpu_mem_if_t *mem)
{
    gpu_t *g = (gpu_t *)calloc(1, sizeof(*g));
    if (!g) return NULL;
    if (cfg) g->cfg = *cfg; else gpu_default_config(&g->cfg);
    g->mem = *mem;
    if (!g->mem.mask) g->mem.mask = GPU_ADDR_MASK;
    gpu_reset(g);
    return g;
}

void gpu_destroy(gpu_t *g)
{
    if (!g) return;
    free(g->pend);
    free(g->q);
    free(g);
}

void gpu_reset(gpu_t *g)
{
    gpu_config_t cfg = g->cfg;
    gpu_mem_if_t mem = g->mem;
    gpu_stats_t st = g->st;
    gpu_flip_cb_t cb = g->flip_cb;
    void *cbctx = g->flip_ctx;
    gpu_cmd_cb_t ccb = g->cmd_cb, pcb = g->pub_cb;
    void *ccbctx = g->cmd_ctx, *pcbctx = g->pub_ctx;
    struct qsim_lint *lint = g->lint;
    uint32_t *pend = g->pend, *q = g->q;
    uint32_t pend_cap = g->pend_cap, q_cap = g->q_cap;
    uint64_t obs_words = g->obs_words, obs_px = g->obs_px;
    memset(g, 0, sizeof(*g));
    g->cfg = cfg; g->mem = mem; g->st = st; g->flip_cb = cb; g->flip_ctx = cbctx;
    g->cmd_cb = ccb; g->cmd_ctx = ccbctx; g->pub_cb = pcb; g->pub_ctx = pcbctx;
    g->lint = lint;
    g->pend = pend; g->pend_cap = pend_cap; g->pend_n = 0;
    g->q = q; g->q_cap = q_cap;                 /* queue emptied (q_head = q_tail = 0) */
    g->obs_words = obs_words; g->obs_px = obs_px;
    g->palookup_base = 0x3FC0000u;           /* PALOOKUP_BASE_DEFAULT */
    g->st_fb_stride = 320;
    /* Staging RAMs/regs without reset power up 0 (Verilator/FF default);
     * masks 0 at power-up behave as literal zero masks until loaded. */
    cache_flush(g);
}

void gpu_set_flip_callback(gpu_t *g, gpu_flip_cb_t cb, void *ctx)
{
    g->flip_cb = cb;
    g->flip_ctx = ctx;
}

void gpu_set_cmd_callback(gpu_t *g, gpu_cmd_cb_t cb, void *ctx)
{
    g->cmd_cb = cb;
    g->cmd_ctx = ctx;
}

void gpu_set_publish_callback(gpu_t *g, gpu_cmd_cb_t cb, void *ctx)
{
    g->pub_cb = cb;
    g->pub_ctx = ctx;
}

void gpu_set_lint(gpu_t *g, struct qsim_lint *lint) { g->lint = lint; }

static void pend_reserve(gpu_t *g, uint32_t need)
{
    if (need <= g->pend_cap) return;
    uint32_t cap = g->pend_cap ? g->pend_cap : 1024;
    while (cap < need) cap *= 2;
    g->pend = (uint32_t *)realloc(g->pend, sizeof(uint32_t) * cap);
    g->pend_cap = cap;
}

void gpu_execute(gpu_t *g, const uint32_t *words, uint32_t n)
{
    if (n == 0) return;
    g->st.batches++;
    const uint32_t *w = words;
    uint32_t avail = n;
    if (g->pend_n) {
        pend_reserve(g, g->pend_n + n);
        memcpy(g->pend + g->pend_n, words, sizeof(uint32_t) * n);
        g->pend_n += n;
        w = g->pend;
        avail = g->pend_n;
    }
    uint32_t pos = 0;
    while (pos < avail) {
        uint32_t hdr = w[pos];
        uint32_t len = hdr & 0x1FFF;
        if (avail - pos < 1 + len) break;       /* incomplete: wait for more */
        exec_command(g, hdr, w + pos + 1);
        g->st.words += 1 + len;
        pos += 1 + len;
    }
    uint32_t left = avail - pos;
    if (left) {
        COV(g, COV_PARTIAL_CMD);
        if (w == g->pend) {
            memmove(g->pend, g->pend + pos, sizeof(uint32_t) * left);
        } else {
            pend_reserve(g, left);
            memcpy(g->pend, w + pos, sizeof(uint32_t) * left);
        }
    }
    g->pend_n = left;
}

/* ---- command queue (ring BRAM between rdptr and wrptr) ---- */
uint32_t gpu_queued_words(const gpu_t *g) { return g->q_tail - g->q_head; }

const char *gpu_sched_name(int sched)
{
    return sched == GPU_SCHED_LAZY ? "lazy" : sched == GPU_SCHED_DRAIN ? "drain" : "eager";
}

static uint32_t ring_rdptr_words(const gpu_t *g)
{
    return (g->ring_wrptr - (g->q_tail - g->q_head)) & 0xFFFu;
}

/* Run queued commands.  max_cmds 0 = unlimited; stop_fence: stop after the
 * first FENCE or FLIP.  Returns the number of commands executed. */
static uint32_t q_run(gpu_t *g, uint32_t max_cmds, int stop_fence)
{
    uint32_t done = 0;
    while (g->q_head < g->q_tail) {
        uint32_t hdr = g->q[g->q_head];
        uint32_t len = hdr & 0x1FFF, op = hdr >> 24;
        if (g->q_tail - g->q_head < 1 + len) { COV(g, COV_PARTIAL_CMD); break; }
        exec_command(g, hdr, g->q + g->q_head + 1);
        g->st.words += 1 + len;
        g->q_head += 1 + len;
        done++;
        if (max_cmds && done >= max_cmds) break;
        if (stop_fence && (op == CMD_FENCE || op == CMD_FLIP)) break;
    }
    if (g->q_head == g->q_tail) {
        g->q_head = g->q_tail = g->q_scan = 0;
    } else if (g->q_head >= 4096 && g->q_head * 2 >= g->q_tail) {
        uint32_t n = g->q_tail - g->q_head;
        memmove(g->q, g->q + g->q_head, sizeof(uint32_t) * n);
        g->q_scan -= g->q_head;
        g->q_tail = n;
        g->q_head = 0;
    }
    return done;
}

/* Is a complete FENCE or FLIP queued? */
static int q_has_fence(const gpu_t *g)
{
    for (uint32_t h = g->q_head; h < g->q_tail;) {
        uint32_t hdr = g->q[h], len = hdr & 0x1FFF, op = hdr >> 24;
        if (g->q_tail - h < 1 + len) return 0;
        if (op == CMD_FENCE || op == CMD_FLIP) return 1;
        h += 1 + len;
    }
    return 0;
}

static void q_publish(gpu_t *g, const uint32_t *words, uint32_t n)
{
    if (n == 0) return;
    g->st.batches++;
    g->st.publishes++;
    g->st.published_words += n;
    if (g->q_tail + n > g->q_cap) {
        uint32_t cap = g->q_cap ? g->q_cap : 16384;
        while (cap < g->q_tail + n) cap *= 2;
        g->q = (uint32_t *)realloc(g->q, sizeof(uint32_t) * cap);
        g->q_cap = cap;
    }
    memcpy(g->q + g->q_tail, words, sizeof(uint32_t) * n);
    g->q_tail += n;
    if (g->q_tail - g->q_head > g->st.queue_max_words) g->st.queue_max_words = g->q_tail - g->q_head;
    /* publish-order notification of complete FLIP commands */
    while (g->q_scan < g->q_tail) {
        uint32_t hdr = g->q[g->q_scan], len = hdr & 0x1FFF;
        if (g->q_tail - g->q_scan < 1 + len) break;
        if ((hdr >> 24) == CMD_FLIP && g->pub_cb) g->pub_cb(g->pub_ctx, hdr, g->q + g->q_scan + 1);
        g->q_scan += 1 + len;
    }
    if (g->cfg.sched == GPU_SCHED_EAGER) q_run(g, 0, 0);
}

/* A CPU observation of GPU progress.  Call q_obs_begin() before running
 * anything for it and q_observe() after. */
static void q_obs_begin(gpu_t *g, uint64_t *w0, uint64_t *px0)
{
    *w0 = g->st.words;
    *px0 = g->st.pixels;
}

static void q_observe(gpu_t *g, int kind, uint64_t w0, uint64_t px0)
{
    uint64_t dw = g->st.words - g->obs_words, dpx = g->st.pixels - g->obs_px;
    g->st.observations[kind]++;
    if (dw || g->q_head < g->q_tail) {
        g->st.barriers[kind]++;
        g->st.barrier_words += dw;
        g->st.barrier_px += dpx;
    }
    g->st.obs_words += g->st.words - w0;
    g->st.obs_px += g->st.pixels - px0;
    g->obs_words = g->st.words;
    g->obs_px = g->st.pixels;
}

void gpu_drain(gpu_t *g, int obs_kind)
{
    uint64_t w0, px0;
    q_obs_begin(g, &w0, &px0);
    q_run(g, 0, 0);
    if (obs_kind >= 0) q_observe(g, obs_kind, w0, px0);
}

void gpu_wait_fence(gpu_t *g, uint32_t token)
{
    uint64_t w0, px0;
    q_obs_begin(g, &w0, &px0);
    if (g->cfg.sched == GPU_SCHED_LAZY) {
        while ((int32_t)(g->fence_reached - token) < 0 && g->q_head < g->q_tail)
            if (!q_run(g, 0, 1)) break;
    } else {
        q_run(g, 0, 0);
    }
    q_observe(g, GPU_OBS_WAIT, w0, px0);
}

void gpu_mmio_write(gpu_t *g, uint32_t offset, uint32_t value)
{
    switch (offset & 0x3C) {
    case 0x00: {                                          /* GPU_CTRL */
        uint8_t mode = g->cpu_ring_mode;
        if (value & 2) {                                  /* soft reset drops a partial command */
            g->pend_n = 0;
            g->sp_q29_shift = 0;
            uint32_t h = g->q_head;
            while (h < g->q_tail && g->q_tail - h >= 1 + (g->q[h] & 0x1FFF)) h += 1 + (g->q[h] & 0x1FFF);
            g->q_tail = h;
            if (g->q_scan > h) g->q_scan = h;
        }
        if (g->cfg.cpu_ring && g->ring_wr_addr == g->ring_wrptr) {
            if (value & 0x10) g->cpu_ring_mode = 1;
            if (g->cfg.command_dma && (value & 0x20)) g->cpu_ring_mode = 0;
        }
        if (g->cfg.cpu_ring && mode && (value & 8) && !(value & 4)) {
            uint32_t nstaged = g->ring_staged;
            g->ring_wrptr = g->ring_wr_addr;
            g->ring_staged = 0;
            q_publish(g, g->ring_words, nstaged);
        }
        if (value & 4) {                                  /* ring reset: queued words are gone */
            g->ring_wr_addr = g->ring_wrptr = 0;
            g->ring_staged = 0;
            g->cpu_ring_mode = 0;
            g->cpu_ring_overflow = 0;
            g->pend_n = 0;
            g->q_head = g->q_tail = g->q_scan = 0;
        }
        break;
    }
    case 0x04:                                            /* append (CPU ring) */
        if (g->cfg.cpu_ring && g->cpu_ring_mode) {
            if (((g->ring_wr_addr + 1) & 0xFFF) == ring_rdptr_words(g)) {   /* gpu_core.v cpu_ring_full */
                g->cpu_ring_overflow = 1;
                g->st.ring_overflows++;
            } else {
                g->ring_words[g->ring_staged++] = value;
                g->ring_wr_addr = (g->ring_wr_addr + 1) & 0xFFF;
            }
        }
        break;
    case 0x0C: g->dma_src = ADDR26(value); break;
    case 0x1C: g->dma_len = value & 0x1FFF; break;
    case 0x2C:
        if (g->cfg.command_dma && (value & 1) && g->dma_len && !(g->cfg.cpu_ring && g->cpu_ring_mode)) {
            uint32_t nw = g->dma_len;
            uint32_t *tmp = (uint32_t *)malloc(sizeof(uint32_t) * nw);
            if (tmp) {
                for (uint32_t i = 0; i < nw; i++) {
                    uint32_t a = g->dma_src + i * 4;
                    tmp[i] = (uint32_t)mem_rd(g, a) | ((uint32_t)mem_rd(g, a + 1) << 8)
                           | ((uint32_t)mem_rd(g, a + 2) << 16) | ((uint32_t)mem_rd(g, a + 3) << 24);
                }
                g->ring_wrptr = g->ring_wr_addr = (g->ring_wr_addr + nw) & 0xFFF;
                q_publish(g, tmp, nw);
                free(tmp);
            }
        }
        break;
    case 0x20:                                            /* GPU_TRANSLUC_ADDR */
        g->transluc_wr_addr = value & 0x7FFF;
        break;
    case 0x24:                                            /* GPU_TRANSLUC_DATA */
        if (g->cfg.transluc) {
            g->transluc[(g->transluc_wr_addr >> 2) & 0x1FFF] = value;
            g->transluc_wr_addr = (g->transluc_wr_addr + 4) & 0x7FFF;
        }
        break;
    case 0x28:                                            /* GPU_TEX_FLUSH */
        cache_flush(g);
        g->st.tex_flushes++;
        break;
    case 0x30:                                            /* GPU_PALOOKUP_BASE */
        g->palookup_base = value & GPU_ADDR_MASK;
        break;
    default:
        break;                                            /* 0x08/0x34/0x38/0x3C: no-op on os25 */
    }
}

uint32_t gpu_mmio_read(gpu_t *g, uint32_t offset)
{
    switch (offset & 0x3C) {
    case 0x04: return (g->ring_wrptr << 2) & 0xFFFF;
    case 0x10: {                                           /* GPU_RING_RDPTR: CPU observes progress */
        uint64_t w0, px0;
        q_obs_begin(g, &w0, &px0);
        if (g->cfg.sched == GPU_SCHED_LAZY) q_run(g, 1, 0);
        else q_run(g, 0, 0);
        q_observe(g, GPU_OBS_RDPTR, w0, px0);
        return (ring_rdptr_words(g) << 2) & 0xFFFF;
    }
    case 0x14: {
        int empty = g->q_head == g->q_tail;                /* no side effects: not a drain point */
        return (empty ? 0x2u : 0x1u)                       /* ring_empty / busy */
             | (g->cfg.cpu_ring ? 0x80u : 0)
             | ((g->cfg.cpu_ring && g->cpu_ring_mode) ? 0x100u : 0)
             | ((g->cfg.cpu_ring && g->cpu_ring_overflow) ? 0x200u : 0);
    }
    case 0x18: {                                           /* GPU_FENCE_REACHED: CPU observes progress */
        uint64_t w0, px0;
        q_obs_begin(g, &w0, &px0);
        if (g->cfg.sched != GPU_SCHED_LAZY) q_run(g, 0, 0);
        else if (q_has_fence(g)) q_run(g, 0, 1);
        q_observe(g, GPU_OBS_FENCE, w0, px0);
        return g->fence_reached;
    }
    case 0x30: return g->palookup_base;
    default: return 0;
    }
}

uint32_t gpu_fence_reached(const gpu_t *g) { return g->fence_reached; }
const gpu_stats_t *gpu_get_stats(const gpu_t *g) { return &g->st; }
void gpu_reset_stats(gpu_t *g) { memset(&g->st, 0, sizeof(g->st)); }

void gpu_print_stats(const gpu_t *g, FILE *f)
{
    const gpu_stats_t *s = &g->st;
    fprintf(f, "gpu: batches=%llu words=%llu\n",
            (unsigned long long)s->batches, (unsigned long long)s->words);
    for (int op = 0; op < 256; op++) {
        if (!s->cmd_count[op]) continue;
        fprintf(f, "gpu:   op 0x%02x count=%llu payload_words=%llu nop=%llu pixels=%llu\n", op,
                (unsigned long long)s->cmd_count[op], (unsigned long long)s->cmd_words[op],
                (unsigned long long)s->cmd_nop[op], (unsigned long long)s->cmd_pixels[op]);
    }
    fprintf(f, "gpu: records=%llu affine=%llu persp=%llu (constz=%llu q29=%llu) passes=%llu recips=%llu\n",
            (unsigned long long)s->records, (unsigned long long)s->spans_affine,
            (unsigned long long)s->spans_persp, (unsigned long long)s->spans_persp_constz,
            (unsigned long long)s->spans_q29, (unsigned long long)s->persp_passes,
            (unsigned long long)s->recip_computes);
    fprintf(f, "gpu: pixels=%llu texels=%llu cmap=%llu cache_hits=%llu cache_fills=%llu\n",
            (unsigned long long)s->pixels, (unsigned long long)s->texels_fetched,
            (unsigned long long)s->cmap_lookups, (unsigned long long)s->tex_cache_hits,
            (unsigned long long)s->tex_cache_fills);
    fprintf(f, "gpu: written=%llu skip0=%llu ztests=%llu zfail=%llu zwrites=%llu transluc=%llu\n",
            (unsigned long long)s->pixels_written, (unsigned long long)s->skip_zero_discards,
            (unsigned long long)s->z_tests, (unsigned long long)s->z_test_fail,
            (unsigned long long)s->z_writes, (unsigned long long)s->transluc_blends);
    for (int k = 0; k < COV_N; k++)
        if (s->cov[k]) fprintf(f, "gpu:   cov %-24s %llu\n", gpu_cov_names[k], (unsigned long long)s->cov[k]);
    fprintf(f, "gpu: sched=%s publishes=%llu observations fence/rdptr/wait=%llu/%llu/%llu barriers=%llu/%llu/%llu "
               "barrier_px=%llu wait_px=%llu queue_max_words=%llu\n", gpu_sched_name(g->cfg.sched),
            (unsigned long long)s->publishes, (unsigned long long)s->observations[0],
            (unsigned long long)s->observations[1], (unsigned long long)s->observations[2],
            (unsigned long long)s->barriers[0], (unsigned long long)s->barriers[1], (unsigned long long)s->barriers[2],
            (unsigned long long)s->barrier_px, (unsigned long long)s->obs_px, (unsigned long long)s->queue_max_words);
    fprintf(f, "gpu: clears=%llu clear_bytes=%llu fences=%llu flips=%llu tex_flushes=%llu overflows=%llu\n",
            (unsigned long long)s->clear_rects, (unsigned long long)s->clear_bytes,
            (unsigned long long)s->fences, (unsigned long long)s->flips,
            (unsigned long long)s->tex_flushes, (unsigned long long)s->ring_overflows);
}
