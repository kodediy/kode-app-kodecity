/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file r3d.c
 * @brief The software 3D renderer.
 *
 * See r3d.h.
 */
#include "r3d.h"
#include "font5x7.h"

#include <math.h>
#include <string.h>
#include <stdlib.h>

__attribute__((weak)) void *kc_fast_alloc(unsigned bytes)
{
    return malloc(bytes);
}

__attribute__((weak)) void *kc_slow_alloc(unsigned bytes)
{
    return malloc(bytes);
}

uint16_t *r3d_fb;
static uint16_t *s_zb;              /* GW*GH, 1/z scaled; 0 = nothing drawn */
static r3d_cam s_cam;
uint16_t r3d_fog = 0;
v3 r3d_sun = {0.0f, 0.4f, 1.0f};
int r3d_style;
uint32_t r3d_tris, r3d_pixels;

#define NEAR_Z  0.25f
#define ZSCALE  15000.0f            /* 1/z * ZSCALE fits 16 bits for z >= 0.25 */
#define FOG_LEVELS 32

uint16_t rgb_mix(uint16_t a, uint16_t b, int t)
{
    if ( 0 >= t )
    {
        return a;
    }
    if ( 32 <= t )
    {
        return b;
    }
    int ar = a >> 11, ag = (a >> 5) & 63, ab = a & 31;
    int br = b >> 11, bg = (b >> 5) & 63, bb = b & 31;
    int r = ar + (((br - ar) * t) >> 5);
    int g = ag + (((bg - ag) * t) >> 5);
    int bl = ab + (((bb - ab) * t) >> 5);
    return (uint16_t)((r << 11) | (g << 5) | bl);
}

static uint16_t shade565(uint16_t c, float k)
{
    int r = (int)((c >> 11) * k), g = (int)(((c >> 5) & 63) * k), b = (int)((c & 31) * k);
    if ( 31 < r )
    {
        r = 31;
    }
    if ( 63 < g )
    {
        g = 63;
    }
    if ( 31 < b )
    {
        b = 31;
    }
    if ( 0 > r )
    {
        r = 0;
    }
    if ( 0 > g )
    {
        g = 0;
    }
    if ( 0 > b )
    {
        b = 0;
    }
    return (uint16_t)((r << 11) | (g << 5) | b);
}

void r3d_set_fog(uint16_t fog)
{
    r3d_fog = fog;
}

const r3d_cam *r3d_camera(void)
{
    return &s_cam;
}

void r3d_begin(uint16_t *fb, const r3d_cam *cam)
{
    r3d_fb = fb;
    s_cam = *cam;
    if ( NULL == s_zb )
    {
        s_zb = kc_fast_alloc(GW * GH * 2);
    }
    memset(s_zb, 0, GW * GH * 2);
    r3d_tris = 0;
    r3d_pixels = 0;
}

static inline v3 to_view(v3 p)
{
    v3 d = v3sub(p, s_cam.pos);
    return v3m(v3dot(d, s_cam.right), v3dot(d, s_cam.up), v3dot(d, s_cam.fwd));
}

bool r3d_project(v3 p, float *sx, float *sy, float *depth)
{
    v3 v = to_view(p);
    if ( NEAR_Z > v.z )
    {
        return false;
    }
    float iz = s_cam.focal / v.z;
    *sx = s_cam.cx + v.x * iz;
    *sy = s_cam.cy - v.y * iz;
    if ( NULL != depth )
    {
        *depth = v.z;
    }
    return true;
}

bool r3d_visible(v3 c, float r)
{
    v3 v = to_view(c);
    if ( -r > v.z )
    {
        return false;
    }
    if ( v.z - r > s_cam.far_dist )
    {
        return false;
    }
    /* side planes: |x| <= z * (half width / focal) */
    float kx = (GW * 0.5f) / s_cam.focal, ky = (GH * 0.5f) / s_cam.focal;
    float lx = sqrtf(1.0f + kx * kx), ly = sqrtf(1.0f + ky * ky);
    if ( fabsf(v.x) - kx * v.z > r * lx )
    {
        return false;
    }
    if ( fabsf(v.y) - ky * v.z > r * ly )
    {
        return false;
    }
    return true;
}

/* ---- the rasteriser ---------------------------------------------------- */

typedef struct {
    float x, y;        /* screen */
    float iz;          /* 1/z */
    float fog;         /* 0..FOG_LEVELS */
    float uz, vz;      /* u/z, v/z */
} svtx;

typedef struct {
    uint16_t pal[5][FOG_LEVELS + 1];   /* base, window dark, window lit (facade: slab, frame, glass, glass lit) */
    int pattern;
    uint16_t color, color2;            /* filled in lazily, after culling */
    float k;
} face_t;

static void make_face_range(face_t *f, int lo, int hi);

static float fog_of(float z)
{
    float start = s_cam.far_dist * 0.45f;
    float f = (z - start) / (s_cam.far_dist - start);
    if ( 0 > f )
    {
        f = 0;
    }
    if ( 1 < f )
    {
        f = 1;
    }
    return f * FOG_LEVELS;
}

static inline uint32_t hash2(int a, int b)
{
    uint32_t h = (uint32_t)a * 73856093u ^ (uint32_t)b * 19349663u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return h;
}

static void raster_tri(const svtx *a, const svtx *b, const svtx *c, const face_t *f)
{
    /* sort by y */
    const svtx *t;
    if ( b->y < a->y )
    {
        t = a;
        a = b;
        b = t;
    }
    if ( c->y < a->y )
    {
        t = a;
        a = c;
        c = t;
    }
    if ( c->y < b->y )
    {
        t = b;
        b = c;
        c = t;
    }
    if ( 0.5f > c->y - a->y )
    {
        return;
    }
    r3d_tris++;

    int y0 = (int)ceilf(a->y - 0.5f), y2 = (int)ceilf(c->y - 0.5f);
    if ( 0 > y0 )
    {
        y0 = 0;
    }
    if ( GH < y2 )
    {
        y2 = GH;
    }

    float inv_ac = 1.0f / (c->y - a->y);
    for ( int y = y0; y < y2; y++ )
    {
        float py = y + 0.5f;
        /* long edge a-c */
        float tl = (py - a->y) * inv_ac;
        float xl = a->x + (c->x - a->x) * tl;
        float izl = a->iz + (c->iz - a->iz) * tl;
        float fl = a->fog + (c->fog - a->fog) * tl;
        float ul = a->uz + (c->uz - a->uz) * tl;
        float vl = a->vz + (c->vz - a->vz) * tl;
        /* short edge */
        const svtx *p, *q;
        if ( py < b->y )
        {
            p = a;
            q = b;
        }
        else
        {
            p = b;
            q = c;
        }
        float dy = q->y - p->y;
        float ts;
        if ( 1e-6f < dy )
        {
            ts = (py - p->y) / dy;
        }
        else
        {
            ts = 0.0f;
        }
        float xr = p->x + (q->x - p->x) * ts;
        float izr = p->iz + (q->iz - p->iz) * ts;
        float fr = p->fog + (q->fog - p->fog) * ts;
        float ur = p->uz + (q->uz - p->uz) * ts;
        float vr = p->vz + (q->vz - p->vz) * ts;
        if ( xr < xl )
        {
            float s;
            s = xl; xl = xr; xr = s;
            s = izl; izl = izr; izr = s;
            s = fl; fl = fr; fr = s;
            s = ul; ul = ur; ur = s;
            s = vl; vl = vr; vr = s;
        }
        int x0 = (int)ceilf(xl - 0.5f), x1 = (int)ceilf(xr - 0.5f);
        if ( 0 > x0 )
        {
            x0 = 0;
        }
        if ( GW < x1 )
        {
            x1 = GW;
        }
        if ( x1 <= x0 )
        {
            continue;
        }
        float w = xr - xl;
        float k;
        if ( 1e-6f < w )
        {
            k = 1.0f / w;
        }
        else
        {
            k = 0.0f;
        }
        float pre = (x0 + 0.5f - xl);
        float diz = (izr - izl) * k, df = (fr - fl) * k;
        float iz = izl + diz * pre, fg = fl + df * pre;
        uint16_t *row = r3d_fb + y * GW;
        uint16_t *zr = s_zb + y * GW;
        int32_t izq = (int32_t)(iz * ZSCALE), dizq = (int32_t)(diz * ZSCALE);
        int32_t fq = (int32_t)(fg * 256.0f), dfq = (int32_t)(df * 256.0f);
        r3d_pixels += (uint32_t)(x1 - x0);
        if ( 0 == f->pattern )
        {
            const uint16_t *pal = f->pal[0];
            for ( int x = x0; x < x1; x++ )
            {
                int32_t zc;
                if ( 65535 < izq )
                {
                    zc = 65535;
                }
                else if ( 1 > izq )
                {
                    zc = 1;
                }
                else
                {
                    zc = izq;
                }
                uint16_t z16 = (uint16_t)zc;
                if ( z16 > zr[x] )
                {
                    zr[x] = z16;
                    int fi = fq >> 8;
                    if ( 0 > fi )
                    {
                        fi = 0;
                    }
                    else if ( FOG_LEVELS < fi )
                    {
                        fi = FOG_LEVELS;
                    }
                    row[x] = pal[fi];
                }
                izq += dizq;
                fq += dfq;
            }
        }
        else
        {
            /* perspective-correct u, v every 8 pixels, straight lines between */
            float du = (ur - ul) * k, dv = (vr - vl) * k;
            float uz = ul + du * pre, vz = vl + dv * pre;
            /* u, v in 1/64 of a window cell: 3 m across, 3.2 m up */
            const float KU = 64.0f / 3.0f, KV = 64.0f / 3.2f;
            float zz = 1.0f / iz;
            int32_t U = (int32_t)(uz * zz * KU * 256.0f), V = (int32_t)(vz * zz * KV * 256.0f);
            int x = x0;
            while ( x < x1 )
            {
                int span;
                if ( 8 > x1 - x )
                {
                    span = x1 - x;
                }
                else
                {
                    span = 8;
                }
                float iz2 = iz + diz * span, uz2 = uz + du * span, vz2 = vz + dv * span;
                float zz2 = 1.0f / iz2;
                int32_t U2 = (int32_t)(uz2 * zz2 * KU * 256.0f), V2 = (int32_t)(vz2 * zz2 * KV * 256.0f);
                int32_t dU = (U2 - U) / span, dV = (V2 - V) / span;
                for ( int e = x + span; x < e; x++ )
                {
                    int32_t zc;
                    if ( 65535 < izq )
                    {
                        zc = 65535;
                    }
                    else if ( 1 > izq )
                    {
                        zc = 1;
                    }
                    else
                    {
                        zc = izq;
                    }
                    uint16_t z16 = (uint16_t)zc;
                    if ( z16 > zr[x] )
                    {
                        zr[x] = z16;
                        int fi = fq >> 8;
                        if ( 0 > fi )
                        {
                            fi = 0;
                        }
                        else if ( FOG_LEVELS < fi )
                        {
                            fi = FOG_LEVELS;
                        }
                        int which = 0;
                        int iu = U >> 8, iv = V >> 8;
                        if ( 1 == f->pattern )
                        {
                            int cu = iu >> 6, cv = iv >> 6, fu = iu & 63, fv = iv & 63;
                            if ( (64 <= iv) && (14 < fu) && (50 > fu) && (18 < fv) && (52 > fv) )
                            {
                                if ( 0 == (hash2(cu, cv) & 7) )
                                {
                                    which = 2;
                                }
                                else
                                {
                                    which = 1;
                                }
                            }
                        }
                        else if ( 2 == f->pattern )
                        {
                            which = (iv >> 4) & 1;
                        }
                        else if ( 3 == f->pattern )
                        {
                            which = ((iu ^ iv) >> 4) & 1;
                        }
                        else if ( 4 == f->pattern )
                        {
                            int fu = iu & 63, fv = iv & 63;
                            if ( 64 > iv )
                            {
                                /* the ground floor: shop windows */
                                if ( 3 > fv )
                                {
                                    which = 1;
                                }
                                else if ( (5 < fu) && (58 > fu) && (6 < fv) && (50 > fv) )
                                {
                                    if ( 30 < fv )
                                    {
                                        which = 4;
                                    }
                                    else
                                    {
                                        which = 3;
                                    }
                                }
                                else if ( 52 < fv )
                                {
                                    which = 1;
                                }
                            }
                            else if ( 5 > fv )
                            {
                                which = 1;                                     /* the floor slab */
                            }
                            else if ( (15 < fu) && (49 > fu) && (16 < fv) && (54 > fv) )
                            {
                                if ( (19 > fu) || (45 < fu) || (20 > fv) || (50 < fv) )
                                {
                                    which = 2;   /* the frame */
                                }
                                else if ( 38 < fv )
                                {
                                    which = 4;   /* sky in the top pane */
                                }
                                else
                                {
                                    which = 3;
                                }
                            }
                        }
                        row[x] = f->pal[which][fi];
                    }
                    izq += dizq;
                    fq += dfq;
                    U += dU;
                    V += dV;
                }
                iz = iz2;
                uz = uz2;
                vz = vz2;
                U = U2;
                V = V2;
            }
        }
    }
}

/* clip a view-space polygon against z >= NEAR_Z */
typedef struct { v3 v; float u, w; } cvtx;

static int clip_near(const cvtx *in, int n, cvtx *out)
{
    int m = 0;
    for ( int i = 0; i < n; i++ )
    {
        const cvtx *a = &in[i], *b = &in[(i + 1) % n];
        bool ain = NEAR_Z <= a->v.z, bin = NEAR_Z <= b->v.z;
        if ( ain )
        {
            out[m++] = *a;
        }
        if ( ain != bin )
        {
            float t = (NEAR_Z - a->v.z) / (b->v.z - a->v.z);
            cvtx c;
            c.v = v3add(a->v, v3scale(v3sub(b->v, a->v), t));
            c.u = a->u + (b->u - a->u) * t;
            c.w = a->w + (b->w - a->w) * t;
            out[m++] = c;
        }
    }
    return m;
}

static void draw_view_poly(const cvtx *vv, int n, const face_t *f)
{
    cvtx cl[12];
    bool need = false;
    for ( int i = 0; i < n; i++ )
    {
        if ( NEAR_Z > vv[i].v.z )
        {
            need = true;
            break;
        }
    }
    if ( need )
    {
        n = clip_near(vv, n, cl);
        if ( 3 > n )
        {
            return;
        }
        vv = cl;
    }
    svtx s[12];
    for ( int i = 0; i < n; i++ )
    {
        float iz = 1.0f / vv[i].v.z;
        s[i].x = s_cam.cx + vv[i].v.x * s_cam.focal * iz;
        s[i].y = s_cam.cy - vv[i].v.y * s_cam.focal * iz;
        s[i].iz = iz;
        s[i].fog = fog_of(vv[i].v.z);
        s[i].uz = vv[i].u * iz;
        s[i].vz = vv[i].w * iz;
    }
    /* back faces: clockwise on screen (y down) is front */
    float area = 0;
    for ( int i = 0; i < n; i++ )
    {
        const svtx *a = &s[i], *b = &s[(i + 1) % n];
        area += a->x * b->y - b->x * a->y;
    }
    if ( 0 >= area )
    {
        return;
    }
    /* quick reject off-screen */
    float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f;
    for ( int i = 0; i < n; i++ )
    {
        if ( s[i].x < minx )
        {
            minx = s[i].x;
        }
        if ( s[i].x > maxx )
        {
            maxx = s[i].x;
        }
        if ( s[i].y < miny )
        {
            miny = s[i].y;
        }
        if ( s[i].y > maxy )
        {
            maxy = s[i].y;
        }
    }
    if ( (0 > maxx) || (GW < minx) || (0 > maxy) || (GH < miny) )
    {
        return;
    }
    float fmin = 1e9f, fmax = -1e9f;
    for ( int i = 0; i < n; i++ )
    {
        if ( s[i].fog < fmin )
        {
            fmin = s[i].fog;
        }
        if ( s[i].fog > fmax )
        {
            fmax = s[i].fog;
        }
    }
    make_face_range((face_t *)f, (int)fmin, (int)fmax + 1);
    for ( int i = 1; i + 1 < n; i++ )
    {
        raster_tri(&s[0], &s[i], &s[i + 1], f);
    }
}

static float light_of(v3 n)
{
    float d = v3dot(n, r3d_sun);
    if ( 0 > d )
    {
        d = 0;
    }
    /* ambient from the sky, a little more from above */
    float sky;
    if ( 0 < n.y )
    {
        sky = 0.10f * n.y;
    }
    else
    {
        sky = 0.0f;
    }
    if ( 1 == r3d_style )
    {
        return 0.46f + 0.66f * d + sky;
    }
    float k = 0.62f + 0.48f * d + sky;
    return k;
}

static void make_face(face_t *f, uint16_t c, int pattern, uint16_t c2, float k)
{
    f->pattern = pattern;
    f->color = c;
    f->color2 = c2;
    f->k = k;
}

static void make_face_range(face_t *f, int lo, int hi)
{
    if ( 0 > lo )
    {
        lo = 0;
    }
    if ( FOG_LEVELS < hi )
    {
        hi = FOG_LEVELS;
    }
    uint16_t base = shade565(f->color, f->k);
    for ( int i = lo; i <= hi; i++ )
    {
        f->pal[0][i] = rgb_mix(base, r3d_fog, i);
    }
    if ( 4 == f->pattern )
    {
        uint16_t slab = shade565(f->color, f->k * 0.8f);
        uint16_t frame = shade565(rgb_mix(f->color, rgb(245, 245, 240), 18), f->k);
        uint16_t glass = shade565(f->color2, 0.7f + 0.3f * f->k);
        uint16_t sky = rgb_mix(glass, rgb(170, 195, 220), 14);
        for ( int i = lo; i <= hi; i++ )
        {
            f->pal[1][i] = rgb_mix(slab, r3d_fog, i);
            f->pal[2][i] = rgb_mix(frame, r3d_fog, i);
            f->pal[3][i] = rgb_mix(glass, r3d_fog, i);
            f->pal[4][i] = rgb_mix(sky, r3d_fog, i);
        }
    }
    else if ( 0 != f->pattern )
    {
        uint16_t w1 = shade565(f->color2, 0.55f + 0.45f * f->k);
        uint16_t w2 = rgb(255, 214, 120);
        for ( int i = lo; i <= hi; i++ )
        {
            f->pal[1][i] = rgb_mix(w1, r3d_fog, i);
            f->pal[2][i] = rgb_mix(w2, r3d_fog, i);
        }
    }
}

void r3d_poly(const r3d_vtx *v, int n, uint16_t color, int pattern, uint16_t color2, bool lit)
{
    cvtx vv[8];
    for ( int i = 0; i < n; i++ )
    {
        vv[i].v = to_view(v[i].p);
        vv[i].u = v[i].u;
        vv[i].w = v[i].v;
    }
    float k = 1.0f;
    if ( lit )
    {
        v3 nrm = v3cross(v3sub(v[1].p, v[0].p), v3sub(v[2].p, v[0].p));
        float l = sqrtf(v3dot(nrm, nrm));
        if ( 1e-6f < l )
        {
            nrm = v3scale(nrm, -1.0f / l);
        }
        k = light_of(nrm);
    }
    face_t f;
    make_face(&f, color, pattern, color2, k);
    draw_view_poly(vv, n, &f);
}

void r3d_poly_flat(const v3 *p, int n, uint16_t color)
{
    cvtx vv[8];
    for ( int i = 0; i < n; i++ )
    {
        vv[i].v = to_view(p[i]);
        vv[i].u = vv[i].w = 0;
    }
    face_t f;
    make_face(&f, color, 0, 0, 1.0f);
    draw_view_poly(vv, n, &f);
}

void r3d_box(v3 c, float sx, float sy, float sz, float yaw, uint16_t side, uint16_t top,
             int pattern, uint16_t color2)
{
    float cs = cosf(yaw), sn = sinf(yaw);
    float hx = sx * 0.5f, hz = sz * 0.5f;
    /* corners, counter-clockwise seen from above */
    float lx[4] = {-hx, hx, hx, -hx}, lz[4] = {-hz, -hz, hz, hz};
    v3 b[4], t[4];
    for ( int i = 0; 4 > i; i++ )
    {
        float wx = c.x + lx[i] * cs + lz[i] * sn;
        float wz = c.z - lx[i] * sn + lz[i] * cs;
        b[i] = v3m(wx, c.y, wz);
        t[i] = v3m(wx, c.y + sy, wz);
    }
    float len[4] = {sx, sz, sx, sz};
    for ( int i = 0; 4 > i; i++ )
    {
        int j = (i + 1) & 3;
        r3d_vtx q[4] = {
            {b[i], 0, 0}, {t[i], 0, sy}, {t[j], len[i], sy}, {b[j], len[i], 0},
        };
        r3d_poly(q, 4, side, pattern, color2, true);
    }
    r3d_vtx tp[4] = {{t[0], 0, 0}, {t[3], 0, 0}, {t[2], 0, 0}, {t[1], 0, 0}};
    r3d_poly(tp, 4, top, 0, 0, true);
}

void r3d_cabin(v3 c, float sx, float sy, float lbot, float ltop, float shift, float yaw,
               uint16_t side, uint16_t top)
{
    float cs = cosf(yaw), sn = sinf(yaw);
    float hx = sx * 0.5f, hb = lbot * 0.5f, ht = ltop * 0.5f, htx = hx * 0.92f;
    float blx[4] = {-hx, hx, hx, -hx}, blz[4] = {-hb, -hb, hb, hb};
    float tlx[4] = {-htx, htx, htx, -htx}, tlz[4] = {-ht + shift, -ht + shift, ht + shift, ht + shift};
    v3 b[4], t[4];
    for ( int i = 0; 4 > i; i++ )
    {
        b[i] = v3m(c.x + blx[i] * cs + blz[i] * sn, c.y, c.z - blx[i] * sn + blz[i] * cs);
        t[i] = v3m(c.x + tlx[i] * cs + tlz[i] * sn, c.y + sy, c.z - tlx[i] * sn + tlz[i] * cs);
    }
    for ( int i = 0; 4 > i; i++ )
    {
        int j = (i + 1) & 3;
        r3d_vtx q[4] = {{b[i], 0, 0}, {t[i], 0, 0}, {t[j], 0, 0}, {b[j], 0, 0}};
        r3d_poly(q, 4, side, 0, 0, true);
    }
    r3d_vtx tp[4] = {{t[0], 0, 0}, {t[3], 0, 0}, {t[2], 0, 0}, {t[1], 0, 0}};
    r3d_poly(tp, 4, top, 0, 0, true);
}

void r3d_billboard(v3 c, float w, float h, uint16_t color)
{
    v3 r = v3scale(s_cam.right, w * 0.5f);
    v3 u = v3m(0, h * 0.5f, 0);
    v3 p[4] = {
        v3sub(v3sub(c, r), u), v3sub(v3add(c, r), u),
        v3add(v3add(c, r), u), v3add(v3sub(c, r), u),
    };
    /* order so it faces the camera */
    v3 q[4] = {p[0], p[3], p[2], p[1]};
    r3d_poly_flat(q, 4, color);
}

/* ---- 2D ------------------------------------------------------------------ */

void r2d_rect(int x, int y, int w, int h, uint16_t c)
{
    if ( 0 > x )
    {
        w += x;
        x = 0;
    }
    if ( 0 > y )
    {
        h += y;
        y = 0;
    }
    if ( GW < x + w )
    {
        w = GW - x;
    }
    if ( GH < y + h )
    {
        h = GH - y;
    }
    for ( int j = 0; j < h; j++ )
    {
        uint16_t *p = r3d_fb + (y + j) * GW + x;
        for ( int i = 0; i < w; i++ )
        {
            p[i] = c;
        }
    }
}

void r2d_rect_blend(int x, int y, int w, int h, uint16_t c, int t)
{
    if ( 0 > x )
    {
        w += x;
        x = 0;
    }
    if ( 0 > y )
    {
        h += y;
        y = 0;
    }
    if ( GW < x + w )
    {
        w = GW - x;
    }
    if ( GH < y + h )
    {
        h = GH - y;
    }
    for ( int j = 0; j < h; j++ )
    {
        uint16_t *p = r3d_fb + (y + j) * GW + x;
        for ( int i = 0; i < w; i++ )
        {
            p[i] = rgb_mix(p[i], c, t);
        }
    }
}

static inline void pset(int x, int y, uint16_t c)
{
    if ( (GW > (unsigned)x) && (GH > (unsigned)y) )
    {
        r3d_fb[y * GW + x] = c;
    }
}

void r2d_line(int x0, int y0, int x1, int y1, uint16_t c)
{
    int dx, sx;
    if ( x1 > x0 )
    {
        dx = x1 - x0;
    }
    else
    {
        dx = x0 - x1;
    }
    if ( x0 < x1 )
    {
        sx = 1;
    }
    else
    {
        sx = -1;
    }
    int dy, sy;
    if ( y1 > y0 )
    {
        dy = -(y1 - y0);
    }
    else
    {
        dy = -(y0 - y1);
    }
    if ( y0 < y1 )
    {
        sy = 1;
    }
    else
    {
        sy = -1;
    }
    int err = dx + dy;
    for ( int guard = 0; 2000 > guard; guard++ )
    {
        pset(x0, y0, c);
        if ( (x0 == x1) && (y0 == y1) )
        {
            break;
        }
        int e2 = 2 * err;
        if ( e2 >= dy )
        {
            err += dy;
            x0 += sx;
        }
        if ( e2 <= dx )
        {
            err += dx;
            y0 += sy;
        }
    }
}

void r2d_disc(int cx, int cy, int r, uint16_t c)
{
    for ( int y = -r; y <= r; y++ )
    {
        int w = (int)sqrtf((float)(r * r - y * y));
        r2d_rect(cx - w, cy + y, 2 * w + 1, 1, c);
    }
}

void r3d_line3(v3 a, v3 b, uint16_t color)
{
    v3 va = to_view(a), vb = to_view(b);
    if ( (NEAR_Z > va.z) && (NEAR_Z > vb.z) )
    {
        return;
    }
    if ( NEAR_Z > va.z )
    {
        float t = (NEAR_Z - va.z) / (vb.z - va.z);
        va = v3add(va, v3scale(v3sub(vb, va), t));
    }
    if ( NEAR_Z > vb.z )
    {
        float t = (NEAR_Z - vb.z) / (va.z - vb.z);
        vb = v3add(vb, v3scale(v3sub(va, vb), t));
    }
    int x0 = (int)(s_cam.cx + va.x * s_cam.focal / va.z), y0 = (int)(s_cam.cy - va.y * s_cam.focal / va.z);
    int x1 = (int)(s_cam.cx + vb.x * s_cam.focal / vb.z), y1 = (int)(s_cam.cy - vb.y * s_cam.focal / vb.z);
    if ( (-500 > x0) || (800 < x0) || (-500 > x1) || (800 < x1) ||
         (-500 > y0) || (800 < y0) || (-500 > y1) || (800 < y1) )
    {
        return;
    }
    r2d_line(x0, y0, x1, y1, color);
    r2d_line(x0 + 1, y0, x1 + 1, y1, color);
}

/* '$' is not in the font: one of our own */
static const uint8_t GLYPH_DOLLAR[7] = {0x04, 0x0F, 0x14, 0x0E, 0x05, 0x1E, 0x04};

void r2d_text(int x, int y, int scale, uint16_t c, const char *s)
{
    for ( ; *s; s++, x += 6 * scale )
    {
        const uint8_t *g;
        if ( '$' == *s )
        {
            g = GLYPH_DOLLAR;
        }
        else
        {
            g = font5x7[font5x7_index(*s)];
        }
        for ( int r = 0; 7 > r; r++ )
        {
            for ( int col = 0; 5 > col; col++ )
            {
                if ( g[r] & (0x10 >> col) )
                {
                    r2d_rect(x + col * scale, y + r * scale, scale, scale, c);
                }
            }
        }
    }
}

int r2d_text_w(int scale, const char *s)
{
    return (int)strlen(s) * 6 * scale - scale;
}

void r2d_text_shadow(int x, int y, int scale, uint16_t c, const char *s)
{
    r2d_text(x + 1, y + 1, scale, 0, s);
    r2d_text(x, y, scale, c, s);
}
