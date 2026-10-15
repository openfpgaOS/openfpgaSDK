//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * Quake-flavoured benchmark: float vertex transform + perspective divide,
 * fixed-point edge walking, textured spans into an 8-bit framebuffer with a
 * 16-bit z-buffer, a colormap lookup, and a BSP-ish tree walk.  Deterministic;
 * prints a framebuffer hash so host and guest builds can be compared.
 *   frames = PROG_FRAMES (default 60)
 */
#include "progio.h"

#define W 320
#define H 200
#ifndef PROG_FRAMES
#define PROG_FRAMES 60
#endif

static uint8_t fb[W * H];
static uint16_t zb[W * H];
static uint8_t tex[64 * 64];
static uint8_t cmap[64 * 256];

typedef struct { float x, y, z; } vec3;
typedef struct { int32_t x, y; uint32_t z; float u, v; } pvert;

static vec3 verts[512];
static uint16_t tris[900][3];

typedef struct node { int16_t plane, front, back, first, count; } node;
static node nodes[256];

static void span(int y, int32_t xl, int32_t xr, float ul, float vl, float ur, float vr, uint32_t zl, uint32_t zr, int light)
{
    if (xr <= xl || y < 0 || y >= H) return;
    int32_t dx = xr - xl;
    float du = (ur - ul) / (float)dx, dv = (vr - vl) / (float)dx;
    int32_t dz = (int32_t)(zr - zl) / dx;
    int32_t x0 = xl < 0 ? 0 : xl, x1 = xr > W ? W : xr;
    float u = ul + du * (float)(x0 - xl), v = vl + dv * (float)(x0 - xl);
    uint32_t z = zl + (uint32_t)dz * (uint32_t)(x0 - xl);
    uint8_t *row = fb + y * W;
    uint16_t *zrow = zb + y * W;
    const uint8_t *cm = cmap + (light & 63) * 256;
    int32_t fu = (int32_t)(u * 65536.0f), fv = (int32_t)(v * 65536.0f);
    int32_t fdu = (int32_t)(du * 65536.0f), fdv = (int32_t)(dv * 65536.0f);
    for (int32_t x = x0; x < x1; x++) {
        uint16_t zz = (uint16_t)(z >> 16);
        if (zz >= zrow[x]) {
            zrow[x] = zz;
            row[x] = cm[tex[((fv >> 16) & 63) * 64 + ((fu >> 16) & 63)]];
        }
        fu += fdu; fv += fdv; z += (uint32_t)dz;
    }
}

static void tri(pvert *a, pvert *b, pvert *c, int light)
{
    pvert *t;
    if (a->y > b->y) { t = a; a = b; b = t; }
    if (a->y > c->y) { t = a; a = c; c = t; }
    if (b->y > c->y) { t = b; b = c; c = t; }
    if (c->y == a->y) return;
    for (int y = a->y; y < c->y; y++) {
        float tl = (float)(y - a->y) / (float)(c->y - a->y);
        int32_t xl = a->x + (int32_t)((float)(c->x - a->x) * tl);
        float ul = a->u + (c->u - a->u) * tl, vl = a->v + (c->v - a->v) * tl;
        uint32_t zl = a->z + (uint32_t)(int32_t)((float)((int32_t)(c->z - a->z) >> 8) * tl) * 256u;
        pvert *p = y < b->y ? a : b, *q = y < b->y ? b : c;
        if (q->y == p->y) continue;
        float ts = (float)(y - p->y) / (float)(q->y - p->y);
        int32_t xr = p->x + (int32_t)((float)(q->x - p->x) * ts);
        float ur = p->u + (q->u - p->u) * ts, vr = p->v + (q->v - p->v) * ts;
        uint32_t zr = p->z + (uint32_t)(int32_t)((float)((int32_t)(q->z - p->z) >> 8) * ts) * 256u;
        if (xl <= xr) span(y, xl, xr, ul, vl, ur, vr, zl, zr, light);
        else span(y, xr, xl, ur, vr, ul, vl, zr, zl, light);
    }
}

static int visit_count;
static void walk(int n, float side)
{
    if (n < 0) return;
    node *nd = &nodes[n];
    visit_count += nd->count;
    float d = side * (float)(nd->plane - 128);
    if (d >= 0) { walk(nd->front, -side); walk(nd->back, side); }
    else { walk(nd->back, side); walk(nd->front, -side); }
}

int main(void)
{
    for (int i = 0; i < 64 * 64; i++) tex[i] = (uint8_t)(((i >> 6) ^ i) * 7 + (prng() & 3));
    for (int l = 0; l < 64; l++)
        for (int c = 0; c < 256; c++) cmap[l * 256 + c] = (uint8_t)((c * (64 - l)) >> 6);
    for (int i = 0; i < 512; i++) {
        verts[i].x = (float)((int32_t)(prng() % 2000) - 1000) * 0.01f;
        verts[i].y = (float)((int32_t)(prng() % 2000) - 1000) * 0.01f;
        verts[i].z = (float)(prng() % 1000) * 0.01f + 4.0f;
    }
    for (int i = 0; i < 900; i++) {
        uint16_t b0 = (uint16_t)(prng() % 512);
        tris[i][0] = b0; tris[i][1] = (uint16_t)((b0 + 1 + prng() % 8) % 512); tris[i][2] = (uint16_t)((b0 + 9 + prng() % 8) % 512);
    }
    for (int i = 0; i < 256; i++) {
        nodes[i].plane = (int16_t)(prng() % 256);
        nodes[i].front = (int16_t)(2 * i + 1 < 256 ? 2 * i + 1 : -1);
        nodes[i].back = (int16_t)(2 * i + 2 < 256 ? 2 * i + 2 : -1);
        nodes[i].count = (int16_t)(prng() % 5);
    }
    uint64_t h = 0;
    static pvert pv[512];
    for (int f = 0; f < PROG_FRAMES; f++) {
        float ang = (float)(f % 40) * 0.05f;
        float ca = 1.0f - ang * ang * 0.5f + ang * ang * ang * ang / 24.0f;
        float sa = ang - ang * ang * ang / 6.0f;
        for (int i = 0; i < W * H; i++) { fb[i] = 0; zb[i] = 0; }
        for (int i = 0; i < 512; i++) {
            vec3 *v = &verts[i];
            float x = v->x * ca - v->z * sa * 0.1f, z = v->z + v->x * sa * 0.1f, y = v->y;
            if (z < 1.0f) z = 1.0f;
            float iz = 1.0f / z;
            pv[i].x = (int32_t)(160.0f + x * 160.0f * iz);
            pv[i].y = (int32_t)(100.0f + y * 160.0f * iz);
            pv[i].z = (uint32_t)(iz * 65535.0f * 65536.0f * 0.25f);
            pv[i].u = (float)(i & 63);
            pv[i].v = (float)((i >> 3) & 63);
        }
        for (int i = 0; i < 900; i++) {
            pvert *a = &pv[tris[i][0]], *b = &pv[tris[i][1]], *c = &pv[tris[i][2]];
            if (a->x < -400 || a->x > 720 || b->x < -400 || b->x > 720 || c->x < -400 || c->x > 720) continue;
            if (a->y < -400 || a->y > 600 || b->y < -400 || b->y > 600 || c->y < -400 || c->y > 600) continue;
            tri(a, b, c, (i + f) & 63);
        }
        visit_count = 0;
        walk(0, (f & 1) ? 1.0f : -1.0f);
        uint32_t fh = 2166136261u;
        for (int i = 0; i < W * H; i += 3) fh = (fh ^ fb[i]) * 16777619u;
        h = mix64(mix64(h, fh), (uint64_t)visit_count);
    }
    out_kv("bench", h);
    return 0;
}
