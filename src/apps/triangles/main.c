//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * triangles: the canonical "drive this GPU well" example.
 *
 * A grid of spinning brick-textured cubes rendered through the OS30 hardware
 * vertex-triangle path (GPU_CMD_SET_TRI_STATE 0x4A + GPU_CMD_DRAW_VERT_TRI
 * 0x4B): the CPU only transforms vertices; the GPU derives the perspective
 * planes, edge-walks, perspective-divides, clips, lights via palookup, and
 * depth-tests.  See Quake2's sw_polyset.c for the same path in a real engine.
 *
 * Built to scale to hundreds of cubes.  The optimization that makes that cheap:
 * every cube shares ONE orientation and size, so the rotation matrix, the eight
 * rotated corner offsets, the six face normals, and the six face light levels
 * are computed ONCE per frame.  A cube then costs only: one transform of its
 * centre, a frustum test, and — per visible face — a back-face cull plus a
 * projection of four vertices (one divide each, fixed-point UVs precomputed).
 * Off-screen and behind-camera cubes are skipped whole.  All framebuffer
 * writes (incl. the HUD) go through the GPU; the frame is paced by the flip
 * fence with triple buffering, so CPU geometry for frame N+1 overlaps GPU
 * raster of frame N — no blocking fence wait.
 *
 * Controls:
 *   d-pad UP / DOWN   more / fewer cubes (grid grows N x N)
 *   d-pad LEFT/RIGHT  320x240 / 640x480
 *   A  pause/resume       B  toggle HUD
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <of.h>
#include <of_cache.h>
#include <of_gpu.h>

#include "brick_tex.h"

#define TEX_W BRICK_W            /* 64 */
#define TEX_H BRICK_H            /* 64 */
#define CMAP_ROWS 64

#define MAX_W 640                /* buffers sized for the largest mode */
#define MAX_H 480

#define MIN_SIDE 1
#define MAX_SIDE 16              /* up to 16x16 = 256 cubes */
#define HALF     0.7f           /* cube half-extent */
#define SPACING  2.2f
#define CAM_NEAR 6.0f           /* camera distance = CAM_NEAR + extent*CAM_K */
#define CAM_K    2.2f
#define SCENE_PITCH 0.55f

#define HUD_W    128            /* power-of-two: wraps cleanly */
#define HUD_H    16
#define HUD_X    3
#define HUD_Y    3

#define ZBUF_ELEM 2             /* RTL-hardwired 16-bit z entries */

static uint8_t  *cube_tex;
static uint8_t  *hud_tex;
static uint16_t *zbuf;
static uint8_t   colormap[CMAP_ROWS * 256];
static uint32_t  pal_rgb[256];

/* Active video mode (runtime). */
static int   vw = 320, vh = 240, fb_stride = 320, hires = 0;
static float cx = 160.0f, cy = 120.0f, focal = 300.0f, zi_k = 65536.0f / 300.0f;

static int  has_zbuffer;
static int  grid_side = 4;       /* N x N cubes */
static int  paused = 0;
static int  show_hud = 1;

static unsigned stat_frames, stat_tris_last;
static unsigned last_fps_x10;
static char hud_l1[40], hud_l2[40];

static inline int clamp_int(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* ================================================================
 * Assets — palette (from the photo), palookup colormap, texture
 * ================================================================ */

static void load_palette(void)
{
    for (int i = 0; i < 256; ++i) {
        pal_rgb[i] = brick_pal[i];
        of_video_palette((uint8_t)i, brick_pal[i]);
    }
}

static int nearest_pal(int r, int g, int b, int lo, int hi)
{
    int best = lo, bestd = 0x7fffffff;
    for (int i = lo; i <= hi; ++i) {
        int pr = (int)((pal_rgb[i] >> 16) & 255);
        int pg = (int)((pal_rgb[i] >> 8) & 255);
        int pb = (int)(pal_rgb[i] & 255);
        int dr = pr - r, dg = pg - g, db = pb - b;
        int d = dr * dr + dg * dg + db * db;
        if (d < bestd) { bestd = d; best = i; }
    }
    return best;
}

/* palookup row r: 0 = brightest, 63 = darkest.  Built by darkening each palette
 * color in RGB and snapping to the nearest image color — works for any palette. */
static void build_colormap(void)
{
    for (int row = 0; row < CMAP_ROWS; ++row) {
        float s = (float)(63 - row) / 63.0f;
        if (s < 0.06f) s = 0.06f;
        for (int c = 0; c < 256; ++c) {
            int r = (int)(((pal_rgb[c] >> 16) & 255) * s);
            int g = (int)(((pal_rgb[c] >> 8) & 255) * s);
            int b = (int)((pal_rgb[c] & 255) * s);
            colormap[row * 256 + c] = (uint8_t)nearest_pal(r, g, b, 0, PAL_IMAGE_HI);
        }
    }
}

static void load_texture(void)
{
    memcpy(cube_tex, brick_pix, TEX_W * TEX_H);
    of_cache_flush_range(cube_tex, TEX_W * TEX_H);
}

/* ================================================================
 * GPU-composited HUD
 * ================================================================ */

static uint16_t glyph_bits(char ch)
{
    switch (ch) {
    case '0': return 0x7b6f; case '1': return 0x2492; case '2': return 0x73e7;
    case '3': return 0x73cf; case '4': return 0x5bc9; case '5': return 0x79cf;
    case '6': return 0x79ef; case '7': return 0x7249; case '8': return 0x7bef;
    case '9': return 0x7bcf;
    case 'A': return 0x5bef; case 'B': return 0x7bcf; case 'C': return 0x7927;
    case 'E': return 0x79e7; case 'F': return 0x79e4; case 'G': return 0x79af;
    case 'I': return 0x7497; case 'L': return 0x4927; case 'N': return 0x5fed;
    case 'P': return 0x7be4; case 'R': return 0x7bed; case 'S': return 0x79cf;
    case 'T': return 0x7484; case 'U': return 0x5b6f; case 'X': return 0x5aad;
    case '%': return 0x5265; case ':': return 0x0410; case '.': return 0x0002;
    case ' ': return 0x0000;
    default:  return 0x0000;
    }
}

static void hud_text(int x, int y, const char *s, uint8_t color)
{
    for (const char *p = s; *p; ++p, x += 4) {
        uint16_t bits = glyph_bits(*p);
        for (int row = 0; row < 5; ++row)
            for (int col = 0; col < 3; ++col)
                if ((bits & (1u << (14 - (row * 3 + col)))) &&
                    (unsigned)(x + col) < HUD_W && (unsigned)(y + row) < HUD_H)
                    hud_tex[(y + row) * HUD_W + (x + col)] = color;
    }
}

static void hud_update(const char *l1, const char *l2)
{
    if (!strcmp(l1, hud_l1) && !strcmp(l2, hud_l2))
        return;
    memset(hud_tex, PAL_HUD_PLATE, HUD_W * HUD_H);
    hud_text(2, 1, l1, PAL_HUD_TEXT);
    hud_text(2, 9, l2, PAL_HUD_TEXT);
    of_cache_flush_range(hud_tex, HUD_W * HUD_H);
    snprintf(hud_l1, sizeof(hud_l1), "%s", l1);
    snprintf(hud_l2, sizeof(hud_l2), "%s", l2);
}

/* ================================================================
 * Math (single precision; rv32imafc has no hardware doubles)
 * ================================================================ */

typedef struct { float x, y, z; } vec3;

static inline vec3 vadd(vec3 a, vec3 b) { return (vec3){ a.x+b.x, a.y+b.y, a.z+b.z }; }
static inline vec3 vsub(vec3 a, vec3 b) { return (vec3){ a.x-b.x, a.y-b.y, a.z-b.z }; }
static inline vec3 vneg(vec3 a)         { return (vec3){ -a.x, -a.y, -a.z }; }
static inline float vdot(vec3 a, vec3 b){ return a.x*b.x + a.y*b.y + a.z*b.z; }

static void mat_mul(const float a[9], const float b[9], float o[9])
{
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            o[r * 3 + c] = a[r * 3 + 0] * b[0 * 3 + c]
                         + a[r * 3 + 1] * b[1 * 3 + c]
                         + a[r * 3 + 2] * b[2 * 3 + c];
}
static void rot_x(float a, float m[9])
{
    float c = cosf(a), s = sinf(a);
    m[0]=1; m[1]=0; m[2]=0;  m[3]=0; m[4]=c; m[5]=-s;  m[6]=0; m[7]=s; m[8]=c;
}
static void rot_y(float a, float m[9])
{
    float c = cosf(a), s = sinf(a);
    m[0]=c; m[1]=0; m[2]=s;  m[3]=0; m[4]=1; m[5]=0;  m[6]=-s; m[7]=0; m[8]=c;
}

/* Unit cube: 8 corners, 6 faces (CCW from outside) with model normals. */
static const float cube_v[8][3] = {
    {-1,-1,-1},{1,-1,-1},{1,1,-1},{-1,1,-1},
    {-1,-1,1},{1,-1,1},{1,1,1},{-1,1,1},
};
static const int   cube_idx[6][4] = {
    {0,1,2,3},{5,4,7,6},{4,0,3,7},{1,5,6,2},{3,2,6,7},{4,5,1,0},
};
/* Per-face texel UVs, precomputed Q16.16 (constant — never recomputed). */
static const int32_t UVF[4][2] = {
    {0, 0}, {TEX_W << 16, 0}, {TEX_W << 16, TEX_H << 16}, {0, TEX_H << 16},
};

static const vec3 LIGHT_DIR = { -0.40f, 0.55f, -0.73f };

/* ================================================================
 * Per-vertex pack + triangle emit
 * ================================================================ */

typedef struct {
    int16_t x, y;        /* Q12.4 subpixel X, integer scanline Y */
    int32_t s, t;        /* Q16.16 RAW texel coords */
    int32_t zi;          /* Q16.16 of 1/z */
    uint8_t light;       /* palookup row 0..63 */
} tvert_t;

/* One divide (focal/z), reused for x, y and zi (= inv * 65536/focal). */
static inline tvert_t project(vec3 v, int32_t s, int32_t t, uint8_t light)
{
    float inv = focal / v.z;
    tvert_t o;
    o.x = (int16_t)lrintf((cx + v.x * inv) * 16.0f);
    o.y = (int16_t)lrintf(cy - v.y * inv);
    o.s = s; o.t = t;
    o.zi = (int32_t)lrintf(inv * zi_k);
    o.light = light;
    return o;
}

static void emit_tri(const tvert_t *a, const tvert_t *b, const tvert_t *c)
{
    int16_t x[3]  = { a->x, b->x, c->x };
    int16_t y[3]  = { a->y, b->y, c->y };
    int32_t s[3]  = { a->s, b->s, c->s };
    int32_t t[3]  = { a->t, b->t, c->t };
    int32_t zi[3] = { a->zi, b->zi, c->zi };
    uint8_t l[3]  = { a->light, b->light, c->light };
    of_gpu_draw_vert_tri(x, y, s, t, zi, l);
}

/* Per-frame shared geometry: the rotation is identical for every cube, so the
 * eight corner offsets, six face normals and six face light rows are computed
 * ONCE here.  rvc0/rvc2 are the view-rotation columns used to place each cube's
 * centre (grid is on the XZ plane, y = 0). */
static void build_geom(float spin_y, float spin_x, float orbit,
                       vec3 voff[8], vec3 vn[6], uint8_t flit[6],
                       vec3 *rvc0, vec3 *rvc2)
{
    float rs[9], rv[9], m[9], ta[9], tb[9];
    rot_y(spin_y, ta); rot_x(spin_x, tb); mat_mul(ta, tb, rs);     /* R_spin */
    rot_x(SCENE_PITCH, ta); rot_y(orbit, tb); mat_mul(ta, tb, rv); /* R_view */
    mat_mul(rv, rs, m);                                            /* M = Rv*Rs */

    vec3 c0 = { m[0], m[3], m[6] };       /* columns of M */
    vec3 c1 = { m[1], m[4], m[7] };
    vec3 c2 = { m[2], m[5], m[8] };
    vec3 hx = { c0.x*HALF, c0.y*HALF, c0.z*HALF };
    vec3 hy = { c1.x*HALF, c1.y*HALF, c1.z*HALF };
    vec3 hz = { c2.x*HALF, c2.y*HALF, c2.z*HALF };
    for (int i = 0; i < 8; ++i) {
        vec3 o = (cube_v[i][0] > 0) ? hx : vneg(hx);
        o = (cube_v[i][1] > 0) ? vadd(o, hy) : vsub(o, hy);
        o = (cube_v[i][2] > 0) ? vadd(o, hz) : vsub(o, hz);
        voff[i] = o;
    }
    vn[0] = vneg(c2); vn[1] = c2;     /* front(-z) / back(+z)  */
    vn[2] = vneg(c0); vn[3] = c0;     /* left(-x)  / right(+x) */
    vn[4] = c1;       vn[5] = vneg(c1);/* top(+y)   / bottom(-y)*/
    for (int f = 0; f < 6; ++f) {
        float d = vdot(vn[f], LIGHT_DIR);
        if (d < 0.0f) d = 0.0f;
        float inten = 0.30f + 0.70f * d;
        flit[f] = (uint8_t)clamp_int((int)((1.0f - inten) * 48.0f), 0, 56);
    }
    *rvc0 = (vec3){ rv[0], rv[3], rv[6] };
    *rvc2 = (vec3){ rv[2], rv[5], rv[8] };
}

/* ================================================================
 * Scene
 * ================================================================ */

static void bind_cube_surface(uint32_t fb_addr)
{
    of_gpu_tri_state_t st;
    memset(&st, 0, sizeof(st));
    st.fb_base       = fb_addr;
    st.fb_major_step = fb_stride;
    st.fb_minor_step = 1;
    st.tex_addr      = (uint32_t)(uintptr_t)cube_tex;
    st.tex_width     = TEX_W;
    st.tex_w_mask    = TEX_W - 1;
    st.tex_h_mask    = TEX_H - 1;
    st.flags         = OF_GPU_SPAN_COLORMAP;
    st.colormap_id   = 0;
    st.clip_x0 = 0;  st.clip_x1 = (int16_t)vw;
    st.clip_y0 = 0;  st.clip_y1 = (int16_t)vh;
    if (has_zbuffer) {
        st.z_mode       = OF_GPU_PARAM_Z_TEST_WRITE;
        st.z_base       = (uint32_t)(uintptr_t)zbuf;
        st.z_major_step = vw * ZBUF_ELEM;
        st.z_minor_step = ZBUF_ELEM;
    } else {
        st.z_mode = OF_GPU_PARAM_Z_NONE;
    }
    of_gpu_set_tri_state(&st);
}

static void emit_hud_quad(uint32_t fb_addr)
{
    of_gpu_tri_state_t st;
    memset(&st, 0, sizeof(st));
    st.fb_base       = fb_addr;
    st.fb_major_step = fb_stride;
    st.fb_minor_step = 1;
    st.tex_addr      = (uint32_t)(uintptr_t)hud_tex;
    st.tex_width     = HUD_W;
    st.tex_w_mask    = HUD_W - 1;
    st.tex_h_mask    = HUD_H - 1;
    st.flags         = 0;
    st.z_mode        = OF_GPU_PARAM_Z_NONE;
    st.clip_x0 = 0;  st.clip_x1 = (int16_t)vw;
    st.clip_y0 = 0;  st.clip_y1 = (int16_t)vh;
    of_gpu_set_tri_state(&st);

    const int x0 = HUD_X, y0 = HUD_Y, x1 = HUD_X + HUD_W, y1 = HUD_Y + HUD_H;
    const int32_t zi = 1 << 16;
    tvert_t q[4] = {
        { (int16_t)(x0 * 16), (int16_t)y0, 0,           0,           zi, 0 },
        { (int16_t)(x1 * 16), (int16_t)y0, HUD_W << 16, 0,           zi, 0 },
        { (int16_t)(x1 * 16), (int16_t)y1, HUD_W << 16, HUD_H << 16, zi, 0 },
        { (int16_t)(x0 * 16), (int16_t)y1, 0,           HUD_H << 16, zi, 0 },
    };
    emit_tri(&q[0], &q[1], &q[2]);
    emit_tri(&q[0], &q[2], &q[3]);
}

static int build_frame(uint32_t fb_addr, float orbit, float spin)
{
    of_gpu_clear_rect_strided(fb_addr, (uint16_t)vw, (uint16_t)vh,
                              (uint16_t)fb_stride, PAL_BG);
    if (has_zbuffer)
        of_gpu_clear_rect_strided((uint32_t)(uintptr_t)zbuf,
                                  (uint16_t)(vw * ZBUF_ELEM), (uint16_t)vh,
                                  (uint16_t)(vw * ZBUF_ELEM), 0);
    bind_cube_surface(fb_addr);

    vec3 voff[8], vn[6], rvc0, rvc2;
    uint8_t flit[6];
    build_geom(spin * 0.9f, spin * 0.6f, orbit, voff, vn, flit, &rvc0, &rvc2);

    float half     = (grid_side - 1) * 0.5f * SPACING;
    float cam_dist = CAM_NEAR + half * CAM_K;
    float xslope   = (float)vw / (2.0f * focal);   /* on-screen if |x| < z*slope */
    float yslope   = (float)vh / (2.0f * focal);
    float cube_r   = HALF * 1.74f;                  /* bounding radius (margin)  */
    float near_z   = cube_r + 0.4f;                 /* keeps every vert in front */

    int tris = 0, drawn = 0;
    for (int j = 0; j < grid_side; ++j) {
        float gz = j * SPACING - half;
        for (int i = 0; i < grid_side; ++i) {
            float gx = i * SPACING - half;
            /* cube centre in view space = R_view * (gx,0,gz) + (0,0,cam_dist) */
            vec3 c = { gx * rvc0.x + gz * rvc2.x,
                       gx * rvc0.y + gz * rvc2.y,
                       gx * rvc0.z + gz * rvc2.z + cam_dist };

            if (c.z < near_z)                                  continue;
            if (c.x < -(c.z * xslope + cube_r) ||
                c.x >  (c.z * xslope + cube_r))                continue;
            if (c.y < -(c.z * yslope + cube_r) ||
                c.y >  (c.z * yslope + cube_r))                continue;

            for (int f = 0; f < 6; ++f) {
                if (vdot(vn[f], c) >= -HALF)                   continue; /* backface */
                tvert_t pv[4];
                for (int k = 0; k < 4; ++k) {
                    int vi = cube_idx[f][k];
                    vec3 v = { c.x + voff[vi].x, c.y + voff[vi].y, c.z + voff[vi].z };
                    int dr = clamp_int((int)((v.z - cam_dist + 6.0f) * 2.2f), 0, 12);
                    pv[k] = project(v, UVF[k][0], UVF[k][1],
                                    (uint8_t)clamp_int(flit[f] + dr, 0, 63));
                }
                emit_tri(&pv[0], &pv[1], &pv[2]);
                emit_tri(&pv[0], &pv[2], &pv[3]);
                tris += 2;
            }
            if ((++drawn & 7) == 0)
                of_gpu_kick();      /* keep the GPU busy while the CPU continues */
        }
    }

    if (show_hud)
        emit_hud_quad(fb_addr);
    return tris;
}

/* ================================================================
 * Bring-up + present loop
 * ================================================================ */

static void no_vert_tri(const char *why)
{
    of_video_set_display_mode(OF_DISPLAY_TERMINAL);
    printf("\033[2J\033[H");
    printf("  triangles: this core has no hardware vertex-triangle path\n");
    printf("  (%s)\n", why);
    printf("  Needs OF_HW_GPU_VERT_TRI + PARAM_TRI + PERSP + PARAM_SPAN_LIST\n");
    printf("  -- the Pocket OS30 / Quake2 GPU bitstream.\n");
    for (;;) { of_input_poll_p0(); usleep(16 * 1000); }
}

static int probe_gpu(int draw_idx)
{
    uint32_t fb = (uint32_t)(uintptr_t)of_video_buffer_addr(draw_idx);
    of_gpu_clear_rect_strided(fb, (uint16_t)vw, (uint16_t)vh, (uint16_t)fb_stride, PAL_BG);
    uint32_t token = of_gpu_submit();
    uint32_t t0 = of_time_us();
    while (!of_gpu_fence_reached(token)) {
        if ((uint32_t)(of_time_us() - t0) > 500000u) {
            of_gpu_debug_snapshot_t s; of_gpu_debug_snapshot(&s, 0);
            printf("[triangles] GPU probe STALL fence=%u reached=%u status=%08x\n",
                   (unsigned)token, (unsigned)s.fence_reached, (unsigned)s.status);
            return 0;
        }
    }
    return 1;
}

static void adopt_mode(void)
{
    of_video_mode_t cur;
    of_video_get_mode(&cur);
    vw = cur.width; vh = cur.height;
    fb_stride = cur.stride ? cur.stride : cur.width;
    focal = (float)vh * 1.25f;
    cx = (float)vw * 0.5f;
    cy = (float)vh * 0.5f;
    zi_k = 65536.0f / focal;
    hires = (vw >= 640);
}

/* d-pad LEFT/RIGHT select a resolution directly.  Returns 1 if it changed
 * (so the caller re-acquires the buffer rotation). */
static int set_resolution(int want_hires)
{
    if (want_hires == hires)
        return 0;
    of_video_mode_t want, norm;
    memset(&want, 0, sizeof want);
    want.width      = want_hires ? 640 : 320;
    want.height     = want_hires ? 480 : 240;
    want.color_mode = OF_VIDEO_MODE_8BIT;
    if (of_video_check_mode(&want, &norm) != 0 || of_video_set_mode(&norm) != 0) {
        printf("[triangles] %dx%d not available on this core\n", want.width, want.height);
        return 0;
    }
    adopt_mode();
    of_video_set_display_mode(OF_DISPLAY_FRAMEBUFFER);
    printf("[triangles] resolution -> %dx%d\n", vw, vh);
    return 1;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    cube_tex = malloc(TEX_W * TEX_H);
    hud_tex  = malloc(HUD_W * HUD_H);
    zbuf     = malloc((size_t)MAX_W * MAX_H * ZBUF_ELEM);
    if (!cube_tex || !hud_tex || !zbuf) {
        printf("[triangles] allocation failed\n");
        return 1;
    }

    load_palette();
    build_colormap();
    load_texture();
    of_cache_flush_range(zbuf, (uint32_t)((size_t)MAX_W * MAX_H * ZBUF_ELEM));

    const struct of_capabilities *caps = of_get_caps();
    if (!(caps->hw_features & OF_HW_GPU_SPAN) || caps->gpu_base == 0)
        no_vert_tri("no GPU span unit / gpu_base");

    of_gpu_init();
    of_gpu_palookup_upload(0, colormap, sizeof(colormap));

    const uint32_t need = OF_HW_GPU_VERT_TRI | OF_HW_GPU_PARAM_TRI |
                          OF_HW_GPU_PERSP   | OF_HW_GPU_PARAM_SPAN_LIST;
    if ((caps->hw_features & need) != need) {
        char buf[96];
        snprintf(buf, sizeof(buf), "VERT_TRI=%d PARAM_TRI=%d PERSP=%d SPAN_LIST=%d",
                 of_has_feature(OF_HW_GPU_VERT_TRI), of_has_feature(OF_HW_GPU_PARAM_TRI),
                 of_has_feature(OF_HW_GPU_PERSP), of_has_feature(OF_HW_GPU_PARAM_SPAN_LIST));
        no_vert_tri(buf);
    }
    has_zbuffer = of_has_feature(OF_HW_GPU_PARAM_SPAN_ZTEST);

    adopt_mode();
    int draw_idx = of_video_acquire_next(-1, 0);
    if (!probe_gpu(draw_idx))
        no_vert_tri("GPU fence never retired (OS/bitstream issue, not the demo)");
    of_video_set_display_mode(OF_DISPLAY_FRAMEBUFFER);

    printf("[triangles] ready (HW vert-tri, %dx%d, z-buffer=%s)\n",
           vw, vh, has_zbuffer ? "on" : "off");

    float orbit = 0.5f, spin = 0.0f;
    uint32_t stat_tick = of_time_us();

    while (1) {
        of_input_poll_p0();
        if (of_btn_pressed(OF_BTN_UP)    && grid_side < MAX_SIDE) grid_side++;
        if (of_btn_pressed(OF_BTN_DOWN)  && grid_side > MIN_SIDE) grid_side--;
        if (of_btn_pressed(OF_BTN_LEFT)  && set_resolution(0))    draw_idx = of_video_acquire_next(-1, 0);
        if (of_btn_pressed(OF_BTN_RIGHT) && set_resolution(1))    draw_idx = of_video_acquire_next(-1, 0);
        if (of_btn_pressed(OF_BTN_A))    paused = !paused;
        if (of_btn_pressed(OF_BTN_B))    show_hud = !show_hud;

        if (!paused) {
            orbit += 0.006f;
            spin  += 0.03f;
        }

        if (show_hud) {
            char l1[40], l2[40];
            snprintf(l1, sizeof(l1), "%dX%d %d CUBES", vw, vh, grid_side * grid_side);
            snprintf(l2, sizeof(l2), "%u.%u FPS %u TRI",
                     last_fps_x10 / 10, last_fps_x10 % 10, stat_tris_last);
            hud_update(l1, l2);
        }

        uint32_t fb_addr = (uint32_t)(uintptr_t)of_video_buffer_addr(draw_idx);
        stat_tris_last = (unsigned)build_frame(fb_addr, orbit, spin);

        uint32_t token = of_gpu_flip_to(draw_idx);
        of_gpu_kick();
        draw_idx = of_video_acquire_next(draw_idx, token);

        ++stat_frames;
        uint32_t now = of_time_us(), elapsed = now - stat_tick;
        if (elapsed >= 1000000) {
            last_fps_x10 = (unsigned)((uint64_t)stat_frames * 10000000ull / elapsed);
            stat_frames = 0;
            stat_tick = now;
        }
    }
}
