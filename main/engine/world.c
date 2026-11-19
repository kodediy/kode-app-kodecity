/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file world.c
 * @brief The two cities: Neon Bay and a joke Albacete, made from a seed.
 *
 * The two cities: Neon Bay (a coast at sunset) and a joke Albacete
 * (La Mancha: no sea, a canal, the fairground with its big wheel, the park,
 * the cathedral, windmills in the fields). Everything is made from a seed at
 * start. Two looks: NEON (sunset, pastel, neon) and REAL (daylight, sober
 * colours, facades with floors and frames, the buildings' shadows on the
 * ground).
 */
#include "world.h"


#include <math.h>
#include <string.h>
#include <stdlib.h>

uint8_t (*world_map)[MAP_N];

#define MAX_BUILDINGS 1500   /**< buildings in a city at most */
#define MAX_PALMS     900    /**< palms and trees at most */
#define MAX_PROPS     40     /**< windmills and wheels at most */

static building_t *s_b;          /* MAX_BUILDINGS, cold: PSRAM on the board */
static int s_nb;
static uint16_t s_block_first[BLOCKS][BLOCKS], s_block_count[BLOCKS][BLOCKS];
static palm_t *s_palm;           /* MAX_PALMS */
static int s_np;
static uint16_t s_palm_first[BLOCKS][BLOCKS], s_palm_count[BLOCKS][BLOCKS];
static uint16_t *s_overview;     /* MAP_N * MAP_N */

/* the buildings' shadows on the ground, one bit per 2 m (REAL look only) */
#define SH_N 512   /**< shadow cells on a side */
static uint8_t *s_shadow;
static bool s_has_shadow;

enum { PROP_WINDMILL, PROP_WHEEL };
typedef struct { int kind; float x, z, yaw; } prop_t;
static prop_t s_prop[MAX_PROPS];
static int s_nprop;

static int s_map, s_style;

static uint32_t s_rng;
static void fix_places(void);
static uint32_t rnd(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}
static float frand(float a, float b)
{
    return a + (b - a) * (float)(rnd() & 0xFFFF) / 65535.0f;
}

static uint32_t hash2(int a, int b)
{
    uint32_t h = (uint32_t)a * 73856093u ^ (uint32_t)b * 19349663u ^ 0x9e3779b9u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return h;
}

int world_map_id(void)
{
    return s_map;
}
int world_style(void)
{
    return s_style;
}

/* ---- Neon Bay's coast --------------------------------------------------- */

static float coast_x(float z)
{
    float x = 150.0f + 34.0f * sinf(z * 0.0075f) + 18.0f * sinf(z * 0.021f + 1.3f);
    if ( 300.0f > z )
    {
        x += (300.0f - z) * 0.55f;   /* the bay */
    }
    return x;
}
static bool is_water(float x, float z)
{
    if ( MAP_ALBACETE == s_map )
    {
        return (296.0f < x) && (304.0f > x);   /* the canal */
    }
    if ( coast_x(z) > x )
    {
        return true;
    }
    if ( (40.0f + 20.0f * sinf(x * 0.01f)) > z )
    {
        return true; /* the south shore */
    }
    return false;
}
static bool is_sand(float x, float z)
{
    if ( MAP_ALBACETE == s_map )
    {
        return false;
    }
    return ((coast_x(z) + 46.0f) > x) || ((92.0f + 20.0f * sinf(x * 0.01f)) > z);
}

/* ---- palettes ---------------------------------------------------------------- */

static const uint8_t PASTEL[][3] = {
    {245, 150, 175}, {120, 220, 200}, {255, 190, 140}, {190, 160, 235},
    {250, 236, 200}, {140, 200, 245}, {255, 120, 140}, {210, 240, 170},
};
static const uint8_t GLASS[][3] = {
    {70, 120, 160}, {60, 90, 130}, {110, 150, 180}, {80, 100, 120}, {50, 70, 110},
};
static const uint8_t NEON[][3] = {
    {255, 60, 200}, {40, 240, 255}, {255, 200, 40}, {150, 80, 255},
};
/* a Spanish town in daylight: cream, ochre, brick, white, salmon, grey */
static const uint8_t TOWN[][3] = {
    {222, 208, 178}, {205, 170, 120}, {168, 96, 74}, {232, 230, 222},
    {214, 150, 120}, {176, 176, 172}, {198, 186, 160}, {150, 84, 66},
};
static const uint8_t TOWER[][3] = {
    {150, 160, 170}, {120, 135, 150}, {190, 186, 176}, {100, 112, 125},
};

static int add_building(float x0, float z0, float x1, float z1, float h, int district)
{
    if ( MAX_BUILDINGS <= s_nb )
    {
        return -1;
    }
    building_t *b = &s_b[s_nb];
    b->x0 = x0; b->z0 = z0; b->x1 = x1; b->z1 = z1; b->h = h;
    if ( STYLE_REAL == s_style )
    {
        const uint8_t *c;
        if ( 0 == district )
        {
            c = TOWER[rnd() % 4];
        }
        else
        {
            c = TOWN[rnd() % 8];
        }
        b->wall = rgb(c[0], c[1], c[2]);
        if ( 0 == district )
        {
            b->glass = rgb(60, 80, 100);
        }
        else
        {
            b->glass = rgb(50, 62, 76);
        }
        if ( 2 == district )
        {
            b->roof = rgb(170, 80, 60);
        }
        else
        {
            b->roof = rgb(c[0] * 3 / 5, c[1] * 3 / 5, c[2] * 3 / 5);
        }
        if ( 3.5f < h )
        {
            b->pattern = 4;
        }
        else
        {
            b->pattern = 0;
        }
    }
    else if ( 0 == district )
    {               /* downtown glass */
        const uint8_t *c = GLASS[rnd() % 5];
        b->wall = rgb(c[0], c[1], c[2]);
        b->glass = rgb(c[0] / 3, c[1] / 3 + 10, c[2] / 2 + 20);
        b->roof = rgb(c[0] * 2 / 3, c[1] * 2 / 3, c[2] * 2 / 3);
        b->pattern = 1;
    }
    else
    {
        const uint8_t *c = PASTEL[rnd() % 8];
        b->wall = rgb(c[0], c[1], c[2]);
        b->glass = rgb(40, 70, 110);
        b->roof = rgb(c[0] * 3 / 4, c[1] * 3 / 4, c[2] * 3 / 4);
        if ( 7.5f < h )
        {
            b->pattern = 1;
        }
        else
        {
            b->pattern = 0;
        }
    }
    b->sign = 0;
    if ( (STYLE_NEON == s_style) && (1 >= district) && (12.0f < h) && (0 == (rnd() % 3)) )
    {
        const uint8_t *n = NEON[rnd() % 4];
        b->sign = 1;
        b->neon = rgb(n[0], n[1], n[2]);
    }
    else if ( (STYLE_REAL == s_style) && (1 >= district) && (14.0f < h) && (0 == (rnd() % 5)) )
    {
        b->sign = 2;                         /* a plain rooftop advert */
        b->neon = rgb(240, 240, 235);
    }
    return s_nb++;
}

static void add_palm(float x, float z)
{
    if ( MAX_PALMS <= s_np )
    {
        return;
    }
    palm_t *p = &s_palm[s_np++];
    p->x = x; p->z = z; p->h = frand(6.5f, 10.5f); p->lean = frand(-0.5f, 0.5f);
    if ( MAP_ALBACETE == s_map )
    {
        p->kind = 1;
    }
    else
    {
        p->kind = 0;
    }
    if ( p->kind )
    {
        p->h = frand(5.0f, 8.0f);
    }
}

static void add_prop(int kind, float x, float z, float yaw)
{
    if ( MAX_PROPS <= s_nprop )
    {
        return;
    }
    s_prop[s_nprop++] = (prop_t){kind, x, z, yaw};
}

static void set_cells(float x0, float z0, float x1, float z1, int t)
{
    for ( int cz = (int)(z0 / CELL_M); (int)ceilf(z1 / CELL_M) > cz; cz++ )
    {
        for ( int cx = (int)(x0 / CELL_M); (int)ceilf(x1 / CELL_M) > cx; cx++ )
        {
            if ( (0 <= cx) && (0 <= cz) && (MAP_N > cx) && (MAP_N > cz) && (T_WATER != world_map[cz][cx]) )
            {
                world_map[cz][cx] = (uint8_t)t;
            }
        }
    }
}

/* ---- the blocks of each city ---------------------------------------------- */

static void block_towers(float ix0, float iz0, float ix1, float iz1, float mx, float mz, float k, float hmin, float hmax)
{
    int split = 1 + (int)(rnd() % 3);
    float g = 3.0f;
    if ( 1 == split )
    {
        add_building(ix0 + 2, iz0 + 2, ix1 - 2, iz1 - 2, frand(hmin, hmax) * k, 0);
    }
    else if ( 2 == split )
    {
        float m = frand(ix0 + 16, ix1 - 16);
        add_building(ix0 + 2, iz0 + 2, m - g * 0.5f, iz1 - 2, frand(hmin, hmax) * k, 0);
        add_building(m + g * 0.5f, iz0 + 2, ix1 - 2, iz1 - 2, frand(hmin, hmax) * k, 0);
    }
    else
    {
        for ( int q = 0; 4 > q; q++ )
        {
            float qx0;
            float qx1;
            float qz0;
            float qz1;
            if ( q & 1 )
            {
                qx0 = mx + g * 0.5f;
                qx1 = ix1 - 2;
            }
            else
            {
                qx0 = ix0 + 2;
                qx1 = mx - g * 0.5f;
            }
            if ( q & 2 )
            {
                qz0 = mz + g * 0.5f;
                qz1 = iz1 - 2;
            }
            else
            {
                qz0 = iz0 + 2;
                qz1 = mz - g * 0.5f;
            }
            add_building(qx0, qz0, qx1, qz1, frand(hmin * 0.7f, hmax * 0.8f) * k, 0);
        }
    }
}

static void block_midtown(float ix0, float iz0, float ix1, float iz1, float mx, float mz, float hmin, float hmax)
{
    for ( int q = 0; 4 > q; q++ )
    {
        float g = 2.0f;
        float qx0;
        float qx1;
        float qz0;
        float qz1;
        if ( q & 1 )
        {
            qx0 = mx + g * 0.5f;
            qx1 = ix1 - 1;
        }
        else
        {
            qx0 = ix0 + 1;
            qx1 = mx - g * 0.5f;
        }
        if ( q & 2 )
        {
            qz0 = mz + g * 0.5f;
            qz1 = iz1 - 1;
        }
        else
        {
            qz0 = iz0 + 1;
            qz1 = mz - g * 0.5f;
        }
        if ( 0 == (rnd() % 7) )
        {
            set_cells(qx0, qz0, qx1, qz1, T_GRASS);
            add_palm((qx0 + qx1) * 0.5f, (qz0 + qz1) * 0.5f);
            continue;
        }
        add_building(qx0 + frand(0, 2), qz0 + frand(0, 2), qx1 - frand(0, 2), qz1 - frand(0, 2), frand(hmin, hmax), 1);
    }
}

static void block_houses(float ix0, float iz0, float ix1, float iz1, float mx, float mz)
{
    set_cells(ix0, iz0, ix1, iz1, T_GRASS);
    for ( int q = 0; 4 > q; q++ )
    {
        float qx;
        float qz;
        float px;
        float pz;
        if ( q & 1 )
        {
            qx = mx + 10;
        }
        else
        {
            qx = mx - 10;
        }
        if ( q & 2 )
        {
            qz = mz + 10;
        }
        else
        {
            qz = mz - 10;
        }
        float w = frand(5, 7), l = frand(5, 7);
        add_building(qx - w, qz - l, qx + w, qz + l, frand(4.5f, 8.5f), 2);
        if ( q & 1 )
        {
            px = 8.5f;
        }
        else
        {
            px = -8.5f;
        }
        if ( q & 2 )
        {
            pz = 8.5f;
        }
        else
        {
            pz = -8.5f;
        }
        add_palm(qx + px, qz + pz);
    }
}

static void block_park(float ix0, float iz0, float ix1, float iz1, int trees)
{
    set_cells(ix0, iz0, ix1, iz1, T_PARK);
    for ( int i = 0; i < trees; i++ )
    {
        add_palm(frand(ix0 + 3, ix1 - 3), frand(iz0 + 3, iz1 - 3));
    }
}

static void gen_neonbay(int bx, int bz, float ix0, float iz0, float ix1, float iz1, float mx, float mz)
{
    const float dcx = 660.0f, dcz = 640.0f;   /* downtown */
    float d = sqrtf((mx - dcx) * (mx - dcx) + (mz - dcz) * (mz - dcz));
    uint32_t h = hash2(bx, bz);
    if ( is_sand(mx, mz) )
    {
        set_cells(ix0, iz0, ix1, iz1, T_SAND);
        for ( int i = 0; 6 > i; i++ )
        {
            add_palm(frand(ix0 + 3, ix1 - 3), frand(iz0 + 3, iz1 - 3));
        }
        if ( 0 == (h & 3) )
        {
            int k = add_building(mx - 3, mz - 3, mx + 3, mz + 3, 3.2f, 2);
            if ( 0 <= k )
            {
                s_b[k].wall = rgb(255, 120, 80);
                s_b[k].roof = rgb(250, 240, 220);
                s_b[k].pattern = 0;
            }
        }
    }
    else if ( (0 == (h % 11)) || ((7 == bx) && (11 == bz)) )
    {
        block_park(ix0, iz0, ix1, iz1, 9);
    }
    else if ( 230.0f > d )
    {
        block_towers(ix0, iz0, ix1, iz1, mx, mz, 1.0f - d / 400.0f, 35.0f, 130.0f);
    }
    else if ( 420.0f > d )
    {
        block_midtown(ix0, iz0, ix1, iz1, mx, mz, 10.0f, 32.0f);
    }
    else
    {
        block_houses(ix0, iz0, ix1, iz1, mx, mz);
    }
}

/* Albacete, more or less. The places are where a joke needs them, not where
 * they are. */
static void gen_albacete(int bx, int bz, float ix0, float iz0, float ix1, float iz1, float mx, float mz)
{
    const float ccx = 540.0f, ccz = 520.0f;   /* the centre */
    float d = sqrtf((mx - ccx) * (mx - ccx) + (mz - ccz) * (mz - ccz));
    uint32_t h = hash2(bx, bz);
    /* the canal runs through block column 4: a green strip either side */
    if ( 4 == bx )
    {
        block_park(ix0, iz0, ix1, iz1, 6);
        return;
    }
    /* Parque Abelardo Sanchez: four blocks of trees */
    if ( ((8 == bx) || (9 == bx)) && ((6 == bz) || (7 == bz)) )
    {
        block_park(ix0, iz0, ix1, iz1, 18);
        if ( (8 == bx) && (7 == bz) )
        {
            /* the bandstand */
            int k = add_building(ix1 - 6, iz0 + 2, ix1 + 2, iz0 + 10, 4.0f, 2);
            if ( 0 <= k )
            {
                s_b[k].wall = rgb(232, 228, 214);
                s_b[k].roof = rgb(120, 140, 120);
                s_b[k].pattern = 0;
            }
        }
        return;
    }
    /* the fairground: the ring of booths around the crossing, the big wheel */
    if ( ((11 == bx) || (12 == bx)) && ((4 == bz) || (5 == bz)) )
    {
        set_cells(ix0, iz0, ix1, iz1, T_LOT);
        if ( (12 == bx) && (5 == bz) )
        {
            add_prop(PROP_WHEEL, (ix0 + ix1) * 0.5f, (iz0 + iz1) * 0.5f, 0.0f);
        }
        return;
    }
    /* the cathedral and its tower */
    if ( (8 == bx) && (8 == bz) )
    {
        set_cells(ix0, iz0, ix1, iz1, T_LOT);
        int k = add_building(ix0 + 6, iz0 + 8, ix1 - 6, iz1 - 14, 19.0f, 1);
        if ( 0 <= k )
        {
            s_b[k].wall = rgb(214, 196, 160);
            s_b[k].roof = rgb(150, 110, 90);
            s_b[k].pattern = 0;
            s_b[k].sign = 0;
        }
        k = add_building(ix0 + 6, iz1 - 14, ix0 + 15, iz1 - 5, 44.0f, 1);
        if ( 0 <= k )
        {
            s_b[k].wall = rgb(214, 196, 160);
            s_b[k].roof = rgb(150, 110, 90);
            s_b[k].pattern = 0;
            s_b[k].sign = 0;
        }
        return;
    }
    /* the Altozano: a square with a monument */
    if ( (9 == bx) && (8 == bz) )
    {
        set_cells(ix0, iz0, ix1, iz1, T_LOT);
        int k = add_building(mx - 2, mz - 2, mx + 2, mz + 2, 7.0f, 2);
        if ( 0 <= k )
        {
            s_b[k].wall = rgb(190, 186, 180);
            s_b[k].roof = rgb(110, 110, 110);
            s_b[k].pattern = 0;
        }
        for ( int i = 0; 6 > i; i++ )
        {
            add_palm(frand(ix0 + 3, ix1 - 3), frand(iz0 + 3, iz1 - 3));
        }
        return;
    }
    /* the Carlos Belmonte: a pitch and four stands */
    if ( (5 == bx) && (10 == bz) )
    {
        set_cells(ix0, iz0, ix1, iz1, T_PARK);
        uint16_t w = rgb(230, 230, 236);
        int k;
        k = add_building(ix0, iz0, ix1, iz0 + 5, 9.0f, 2);
        if ( 0 <= k )
        {
            s_b[k].wall = w;
            s_b[k].pattern = 0;
            s_b[k].roof = rgb(200, 40, 40);
        }
        k = add_building(ix0, iz1 - 5, ix1, iz1, 9.0f, 2);
        if ( 0 <= k )
        {
            s_b[k].wall = w;
            s_b[k].pattern = 0;
            s_b[k].roof = rgb(200, 40, 40);
        }
        k = add_building(ix0, iz0 + 5, ix0 + 4, iz1 - 5, 7.0f, 2);
        if ( 0 <= k )
        {
            s_b[k].wall = w;
            s_b[k].pattern = 0;
            s_b[k].roof = rgb(200, 40, 40);
        }
        k = add_building(ix1 - 4, iz0 + 5, ix1, iz1 - 5, 7.0f, 2);
        if ( 0 <= k )
        {
            s_b[k].wall = w;
            s_b[k].pattern = 0;
            s_b[k].roof = rgb(200, 40, 40);
        }
        return;
    }
    /* Los Llanos: the air base to the south, hangars on a flat apron */
    if ( (2 >= bz) && (6 <= bx) && (10 >= bx) )
    {
        set_cells(ix0, iz0, ix1, iz1, T_LOT);
        if ( 0 == (bx & 1) )
        {
            int k = add_building(ix0 + 4, iz0 + 8, ix1 - 4, iz1 - 12, 11.0f, 2);
            if ( 0 <= k )
            {
                s_b[k].wall = rgb(150, 156, 150);
                s_b[k].roof = rgb(120, 126, 120);
                s_b[k].pattern = 0;
            }
        }
        return;
    }
    /* the plain all around: fields, vineyards, windmills */
    if ( 400.0f < d )
    {
        int t;
        if ( 0 == (h % 3) )
        {
            t = T_VINE;
        }
        else if ( 1 == (h % 3) )
        {
            t = T_FIELD;
        }
        else
        {
            t = T_FIELD2;
        }
        set_cells(ix0, iz0, ix1, iz1, t);
        if ( 0 == (h % 5) )
        {
            add_prop(PROP_WINDMILL, mx + frand(-10, 10), mz + frand(-10, 10), frand(0, 6.28f));
        }
        else if ( 0 == (h % 7) )
        {
            int k = add_building(mx - 6, mz - 4, mx + 6, mz + 4, 5.0f, 2);   /* a farmhouse */
            if ( 0 <= k )
            {
                s_b[k].wall = rgb(236, 232, 220);
                s_b[k].roof = rgb(170, 80, 60);
            }
        }
        return;
    }
    if ( 0 == (h % 13) )
    {
        block_park(ix0, iz0, ix1, iz1, 8);
        return;
    }
    if ( 150.0f > d )
    {
        block_towers(ix0, iz0, ix1, iz1, mx, mz, 1.0f, 20.0f, 52.0f);
    }
    else if ( 320.0f > d )
    {
        block_midtown(ix0, iz0, ix1, iz1, mx, mz, 9.0f, 26.0f);
    }
    else
    {
        block_houses(ix0, iz0, ix1, iz1, mx, mz);
    }
}

/* the fairground's ring: booths on a circle around the crossing, off the roads */
static void fair_ring(void)
{
    float cx = 774.0f, cz = 326.0f;
    for ( int i = 0; 40 > i; i++ )
    {
        float a = i * 6.2831853f / 40.0f;
        float x = cx + cosf(a) * 42.0f, z = cz + sinf(a) * 42.0f;
        if ( (T_ROAD == world_type(x, z)) || (T_ROAD == world_type(x - 3, z - 3)) || (T_ROAD == world_type(x + 3, z + 3)) )
        {
            continue;
        }
        int bx = (int)(x / BLOCK_M), bz = (int)(z / BLOCK_M);
        /* booths go in their own block's range: append, then fix the index below */
        (void)bx; (void)bz;
        int k = add_building(x - 2.6f, z - 2.6f, x + 2.6f, z + 2.6f, 5.5f, 2);
        if ( 0 <= k )
        {
            if ( i & 1 )
            {
                s_b[k].wall = rgb(236, 230, 214);
            }
            else
            {
                s_b[k].wall = rgb(200, 150, 90);
            }
            s_b[k].roof = rgb(160, 70, 50);
            s_b[k].pattern = 0;
        }
    }
}

/* ---- shadows ------------------------------------------------------------- */

static void make_shadows(void)
{
    if ( NULL == s_shadow )
    {
        s_shadow = kc_fast_alloc(SH_N * SH_N / 8);
    }
    memset(s_shadow, 0, SH_N * SH_N / 8);
    s_has_shadow = true;
    v3 s = r3d_sun;
    if ( 0.2f > s.y )
    {
        s_has_shadow = false;
        return;
    }
    float ox = -s.x / s.y, oz = -s.z / s.y;           /* shadow length per metre of height */
    for ( int i = 0; i < s_nb; i++ )
    {
        const building_t *b = &s_b[i];
        float lx = ox * b->h, lz = oz * b->h;
        float len = sqrtf(lx * lx + lz * lz);
        int steps = (int)(len / 1.5f) + 1;
        for ( int k = 0; k <= steps; k++ )
        {
            float t = (float)k / steps;
            float x0 = b->x0 + lx * t, x1 = b->x1 + lx * t, z0 = b->z0 + lz * t, z1 = b->z1 + lz * t;
            for ( int cz = (int)(z0 * 0.5f); (int)(z1 * 0.5f) >= cz; cz++ )
            {
                if ( (0 > cz) || (SH_N <= cz) )
                {
                    continue;
                }
                for ( int cx = (int)(x0 * 0.5f); (int)(x1 * 0.5f) >= cx; cx++ )
                {
                    if ( (0 > cx) || (SH_N <= cx) )
                    {
                        continue;
                    }
                    s_shadow[cz * (SH_N / 8) + (cx >> 3)] |= (uint8_t)(1u << (cx & 7));
                }
            }
        }
    }
}

/* ---- init ----------------------------------------------------------------- */

static void style_sun(void)
{
    v3 sd;
    if ( STYLE_REAL == s_style )
    {
        sd = v3m(-0.45f, 0.72f, -0.35f);
    }
    else
    {
        sd = v3m(-0.92f, 0.10f, 0.38f);
    }
    r3d_sun = v3scale(sd, 1.0f / sqrtf(v3dot(sd, sd)));
}

void world_init(uint32_t seed, int map, int style)
{
    if ( 0U != seed )
    {
        s_rng = seed;
    }
    else
    {
        s_rng = 0x1234567u;
    }
    s_map = map;
    s_style = style;
    r3d_style = style;
    style_sun();
    if ( NULL == world_map )
    {
        world_map = kc_fast_alloc(MAP_N * MAP_N);
    }
    if ( NULL == s_b )
    {
        s_b = kc_slow_alloc(MAX_BUILDINGS * sizeof(building_t));
    }
    if ( NULL == s_palm )
    {
        s_palm = kc_slow_alloc(MAX_PALMS * sizeof(palm_t));
    }
    if ( NULL == s_overview )
    {
        s_overview = kc_slow_alloc(MAP_N * MAP_N * 2);
    }
    s_nb = 0;
    s_np = 0;
    s_nprop = 0;

    for ( int cz = 0; MAP_N > cz; cz++ )
    {
        for ( int cx = 0; MAP_N > cx; cx++ )
        {
            float x = cx * CELL_M + 2.0f, z = cz * CELL_M + 2.0f;
            int lx = (cx * CELL_M) % BLOCK_M, lz = (cz * CELL_M) % BLOCK_M;
            uint8_t t;
            if ( (ROAD_W > lx) || (ROAD_W > lz) )
            {
                if ( (MAP_NEONBAY == s_map) && is_water(x, z) )
                {
                    t = T_WATER;
                }
                else
                {
                    t = T_ROAD;
                }
            }
            else if ( is_water(x, z) )
            {
                t = T_WATER;
            }
            else if ( is_sand(x, z) )
            {
                t = T_SAND;
            }
            else if ( ((ROAD_W + WALK_W) > lx) || ((ROAD_W + WALK_W) > lz) || ((BLOCK_M - WALK_W) <= lx) || ((BLOCK_M - WALK_W) <= lz) )
            {
                t = T_WALK;
            }
            else
            {
                t = T_LOT;
            }
            world_map[cz][cx] = t;
        }
    }
    for ( int i = 0; MAP_N > i; i++ )
    {
        world_map[MAP_N - 1][i] = T_WATER;
        world_map[i][MAP_N - 1] = T_WATER;
    }

    for ( int bz = 0; BLOCKS > bz; bz++ )
    {
        for ( int bx = 0; BLOCKS > bx; bx++ )
        {
            s_block_first[bz][bx] = (uint16_t)s_nb;
            s_palm_first[bz][bx] = (uint16_t)s_np;
            float ox = bx * BLOCK_M, oz = bz * BLOCK_M;
            float ix0 = ox + ROAD_W + WALK_W, iz0 = oz + ROAD_W + WALK_W;
            float ix1 = ox + BLOCK_M - WALK_W, iz1 = oz + BLOCK_M - WALK_W;
            float mx = (ix0 + ix1) * 0.5f, mz = (iz0 + iz1) * 0.5f;
            bool water = (MAP_NEONBAY == s_map) &&
                         (is_water(ix0, iz0) || is_water(ix1, iz0) || is_water(ix0, iz1) || is_water(ix1, iz1));
            if ( !(water || ((BLOCKS - 1) == bx) || ((BLOCKS - 1) == bz)) )
            {
                if ( MAP_ALBACETE == s_map )
                {
                    gen_albacete(bx, bz, ix0, iz0, ix1, iz1, mx, mz);
                }
                else
                {
                    gen_neonbay(bx, bz, ix0, iz0, ix1, iz1, mx, mz);
                }
                if ( (MAP_ALBACETE == s_map) && (12 == bx) && (5 == bz) )
                {
                    fair_ring();
                }
            }
            s_block_count[bz][bx] = (uint16_t)(s_nb - s_block_first[bz][bx]);
            s_palm_count[bz][bx] = (uint16_t)(s_np - s_palm_first[bz][bx]);
        }
    }
    if ( MAP_ALBACETE == s_map )
    {
        /* the fair's booths were appended to block (12,5)'s range but stand in
         * four blocks: give every block that range too, for collisions */
        int f = s_block_first[5][12], n = s_block_count[5][12];
        for ( int bz = 4; 5 >= bz; bz++ )
        {
            for ( int bx = 11; 12 >= bx; bx++ )
            {
                if ( !((12 == bx) && (5 == bz)) )
                {
                    s_block_first[bz][bx] = (uint16_t)f;
                    s_block_count[bz][bx] = (uint16_t)n;
                }
            }
        }
    }

    fix_places();
    if ( STYLE_REAL == s_style )
    {
        make_shadows();
    }
    else
    {
        s_has_shadow = false;
    }

    for ( int cz = 0; MAP_N > cz; cz++ )
    {
        for ( int cx = 0; MAP_N > cx; cx++ )
        {
            uint16_t c;
            switch ( world_map[cz][cx] )
            {
                case T_WATER: c = rgb(30, 70, 120); break;
                case T_SAND:  c = rgb(220, 196, 140); break;
                case T_ROAD:  c = rgb(70, 70, 78); break;
                case T_WALK:  c = rgb(150, 150, 150); break;
                case T_GRASS: case T_PARK: c = rgb(80, 150, 80); break;
                case T_FIELD: c = rgb(196, 160, 104); break;
                case T_FIELD2: c = rgb(214, 190, 120); break;
                case T_VINE: c = rgb(150, 150, 90); break;
                default:      c = rgb(120, 115, 110); break;
            }
            s_overview[cz * MAP_N + cx] = c;
        }
    }
    for ( int i = 0; i < s_nb; i++ )
    {
        building_t *b = &s_b[i];
        uint16_t c = rgb_mix(b->wall, 0, 12);
        for ( int cz = (int)(b->z0 / CELL_M); (int)(b->z1 / CELL_M) >= cz; cz++ )
        {
            for ( int cx = (int)(b->x0 / CELL_M); (int)(b->x1 / CELL_M) >= cx; cx++ )
            {
                if ( (0 <= cx) && (0 <= cz) && (MAP_N > cx) && (MAP_N > cz) )
                {
                    s_overview[cz * MAP_N + cx] = c;
                }
            }
        }
    }
}

const uint16_t *world_overview(void)
{
    return s_overview;
}

int world_type(float x, float z)
{
    int cx = (int)(x / CELL_M), cz = (int)(z / CELL_M);
    if ( (0 > x) || (0 > z) || (MAP_N <= cx) || (MAP_N <= cz) )
    {
        return T_WATER;
    }
    return world_map[cz][cx];
}

bool world_is_road(float x, float z)
{
    return T_ROAD == world_type(x, z);
}

void world_nearest_crossing(float x, float z, float *cx, float *cz)
{
    *cx = floorf((x - ROAD_W * 0.5f) / BLOCK_M + 0.5f) * BLOCK_M + ROAD_W * 0.5f;
    *cz = floorf((z - ROAD_W * 0.5f) / BLOCK_M + 0.5f) * BLOCK_M + ROAD_W * 0.5f;
}

bool world_free(float x, float z, float r, bool water_blocks)
{
    if ( (r > x) || (r > z) || ((WORLD_M - r) < x) || ((WORLD_M - r) < z) )
    {
        return false;
    }
    if ( water_blocks && (T_WATER == world_type(x, z)) )
    {
        return false;
    }
    int bx = (int)(x / BLOCK_M), bz = (int)(z / BLOCK_M);
    for ( int dz = -1; 1 >= dz; dz++ )
    {
        for ( int dx = -1; 1 >= dx; dx++ )
        {
            int ax = bx + dx, az = bz + dz;
            if ( (0 > ax) || (0 > az) || (BLOCKS <= ax) || (BLOCKS <= az) )
            {
                continue;
            }
            int f = s_block_first[az][ax], n = s_block_count[az][ax];
            for ( int i = f; (f + n) > i; i++ )
            {
                const building_t *b = &s_b[i];
                if ( ((b->x0 - r) < x) && ((b->x1 + r) > x) && ((b->z0 - r) < z) && ((b->z1 + r) > z) )
                {
                    return false;
                }
            }
        }
    }
    return true;
}

int world_ray(float x0, float z0, float x1, float z1, float *t)
{
    float dx = x1 - x0, dz = z1 - z0;
    float len = sqrtf(dx * dx + dz * dz);
    int steps = (int)(len / 1.0f) + 1;
    for ( int i = 1; i <= steps; i++ )
    {
        float f = (float)i / steps;
        if ( !world_free(x0 + dx * f, z0 + dz * f, 0.05f, false) )
        {
            *t = f;
            return 1;
        }
    }
    *t = 1.0f;
    return -1;
}

static place_t PLACES_NB[] = {
    {"THE BEACH", 210, 520}, {"DOWNTOWN", 652, 620}, {"THE PIER", 300, 120},
    {"NEON MOTEL", 908, 300}, {"PALM HILLS", 860, 860}, {"CITY PARK", 460, 716},
    {"THE AVENUE", 520, 380}, {"THE LIGHTHOUSE", 230, 900},
};
static place_t PLACES_AB[] = {
    {"VILLACERRADA", 460, 492}, {"P. ABELARDO SANCHEZ", 590, 460}, {"LA FERIA", 750, 300},
    {"EL ALTOZANO", 590, 590}, {"THE CATHEDRAL", 520, 590}, {"CARLOS BELMONTE", 380, 700},
    {"LOS LLANOS", 580, 140}, {"THE CANAL", 300, 520}, {"THE WINDMILLS", 900, 900},
    {"CALLE ANCHA", 652, 652},
};
#define N_NB ((int)(sizeof PLACES_NB / sizeof PLACES_NB[0]))   /**< places in Neon Bay */
#define N_AB ((int)(sizeof PLACES_AB / sizeof PLACES_AB[0]))   /**< places in Albacete */

/* every place must be somewhere a car can stop: if not, the nearest crossing */
static void fix_places(void)
{
    place_t *pl;
    int n;
    if ( MAP_ALBACETE == s_map )
    {
        pl = PLACES_AB;
        n = N_AB;
    }
    else
    {
        pl = PLACES_NB;
        n = N_NB;
    }
    for ( int i = 0; i < n; i++ )
    {
        place_t *p = &pl[i];
        if ( world_free(p->x, p->z, 2.5f, true) )
        {
            continue;
        }
        float cx, cz;
        world_nearest_crossing(p->x, p->z, &cx, &cz);
        p->x = cx; p->z = cz;
    }
}

int world_places(const place_t **out)
{
    if ( MAP_ALBACETE == s_map )
    {
        *out = PLACES_AB;
        return N_AB;
    }
    else
    {
        *out = PLACES_NB;
        return N_NB;
    }
}

const char *world_name(void)
{
    if ( MAP_ALBACETE == s_map )
    {
        return "ALBACETE";
    }
    else
    {
        return "NEON BAY";
    }
}

/* ---- the sky ------------------------------------------------------------- */

static float s_sun_x = -1000.0f;

static float horizon_row(void)
{
    const r3d_cam *c = r3d_camera();
    float hx = c->fwd.x, hz = c->fwd.z, l = sqrtf(hx * hx + hz * hz);
    v3 d = v3m(hx / l, 0, hz / l);
    return c->cy - c->focal * v3dot(d, c->up) / v3dot(d, c->fwd);
}

/* a far ridge per column: islands at sea, or the sierra over La Mancha */
static void far_ridge(uint16_t col, bool sea_only, float scale)
{
    const r3d_cam *c = r3d_camera();
    int hy2 = (int)horizon_row();
    for ( int x = 0; GW > x; x++ )
    {
        float a = (x + 0.5f - c->cx) / c->focal;
        v3 d = v3add(c->fwd, v3scale(c->right, a));
        float yaw = atan2f(d.x, d.z);
        if ( sea_only && (-0.2f < d.x) )
        {
            continue;
        }
        float h = (6.0f + 5.0f * sinf(yaw * 7.0f) + 3.0f * sinf(yaw * 19.0f + 1.0f) + 2.0f * sinf(yaw * 41.0f)) * scale;
        if ( 2.0f > h )
        {
            continue;
        }
        int top = hy2 - (int)h;
        int y0;
        if ( 0 > top )
        {
            y0 = 0;
        }
        else
        {
            y0 = top;
        }
        for ( int y = y0; (hy2 >= y) && (GH > y); y++ )
        {
            r3d_fb[y * GW + x] = col;
        }
    }
}

static void sky_gradient(const uint8_t *top, const uint8_t *mid, const uint8_t *hor, int hy)
{
    for ( int y = 0; GH > y; y++ )
    {
        float t = (float)(hy - y) / 170.0f;
        if ( 0 > t )
        {
            t = 0;
        }
        if ( 1 < t )
        {
            t = 1;
        }
        int r, g, b;
        if ( 0.35f > t )
        {
            float k = t / 0.35f;
            r = (int)(hor[0] + (mid[0] - hor[0]) * k);
            g = (int)(hor[1] + (mid[1] - hor[1]) * k);
            b = (int)(hor[2] + (mid[2] - hor[2]) * k);
        }
        else
        {
            float k = (t - 0.35f) / 0.65f;
            r = (int)(mid[0] + (top[0] - mid[0]) * k);
            g = (int)(mid[1] + (top[1] - mid[1]) * k);
            b = (int)(mid[2] + (top[2] - mid[2]) * k);
        }
        uint16_t col = rgb(r, g, b);
        uint16_t *row = r3d_fb + y * GW;
        for ( int x = 0; GW > x; x++ )
        {
            row[x] = col;
        }
    }
}

void world_draw_sky(float time)
{
    (void)time;
    const r3d_cam *c = r3d_camera();
    int hy = (int)horizon_row();
    float sx, sy, dz;
    float ridge_scale;
    s_sun_x = -1000.0f;
    style_sun();
    v3 p = v3add(c->pos, v3scale(r3d_sun, 500.0f));
    if ( MAP_ALBACETE == s_map )
    {
        ridge_scale = 0.6f;
    }
    else
    {
        ridge_scale = 1.0f;
    }
    if ( STYLE_REAL == s_style )
    {
        static const uint8_t TOP[3] = {58, 112, 196}, MID[3] = {120, 168, 222}, HOR[3] = {200, 214, 228};
        sky_gradient(TOP, MID, HOR, hy);
        far_ridge(rgb(150, 160, 175), MAP_NEONBAY == s_map, ridge_scale);
        /* fair-weather clouds */
        for ( int k = 0; 6 > k; k++ )
        {
            float cyaw = -2.6f + k * 1.05f;
            v3 cp = v3add(c->pos, v3m(sinf(cyaw) * 700.0f, 110.0f + (k & 1) * 40.0f, cosf(cyaw) * 700.0f));
            float cx2, cy2, cz2;
            if ( r3d_project(cp, &cx2, &cy2, &cz2) )
            {
                int w = 46 + (k % 3) * 16;
                r2d_rect_blend((int)cx2 - w / 2, (int)cy2, w, 5, rgb(245, 248, 252), 20);
                r2d_rect_blend((int)cx2 - w / 3, (int)cy2 - 4, w * 2 / 3, 4, rgb(250, 252, 255), 18);
                r2d_rect_blend((int)cx2 - w / 2 + 4, (int)cy2 + 5, w - 8, 2, rgb(200, 210, 225), 14);
            }
        }
        if ( r3d_project(p, &sx, &sy, &dz) )
        {
            s_sun_x = sx;
            r2d_disc((int)sx, (int)sy, 14, rgb(255, 250, 225));
            r2d_disc((int)sx, (int)sy, 9, rgb(255, 255, 250));
        }
        r3d_set_fog(rgb(196, 210, 226));
        return;
    }
    static const uint8_t TOP[3] = {45, 35, 120}, MID[3] = {200, 80, 150}, HOR[3] = {255, 150, 90};
    sky_gradient(TOP, MID, HOR, hy);
    far_ridge(rgb(150, 70, 120), MAP_NEONBAY == s_map, ridge_scale);
    for ( int k = 0; 4 > k; k++ )
    {
        float cyaw = -1.9f + k * 0.55f;
        v3 cp = v3add(c->pos, v3m(sinf(cyaw) * 600.0f, 70.0f + k * 22.0f, cosf(cyaw) * 600.0f));
        float cx2, cy2, cz2;
        if ( r3d_project(cp, &cx2, &cy2, &cz2) )
        {
            int w = 60 + k * 18;
            r2d_rect_blend((int)cx2 - w / 2, (int)cy2, w, 3, rgb(255, 170, 150), 18);
            r2d_rect_blend((int)cx2 - w / 3, (int)cy2 + 3, w * 2 / 3, 2, rgb(255, 140, 140), 14);
        }
    }
    if ( r3d_project(p, &sx, &sy, &dz) )
    {
        s_sun_x = sx;
        r2d_disc((int)sx, (int)sy, 26, rgb(255, 170, 110));
        r2d_disc((int)sx, (int)sy, 19, rgb(255, 225, 160));
        for ( int k = 0; 4 > k; k++ )
        {
            r2d_rect((int)sx - 26, (int)sy + 4 + k * 5, 52, 1 + k / 2, rgb(255, 150, 110));
        }
    }
    r3d_set_fog(rgb(255, 150, 120));
}

/* ---- the ground ---------------------------------------------------------- */

enum {
    G_ASPHALT, G_ASPHALT2, G_LINE_W, G_LINE_Y, G_WALK, G_CURB, G_SAND, G_SAND2,
    G_WATER, G_WATER2, G_GRASS, G_GRASS2, G_LOT, G_LOT2, G_FOAM, G_GLINT,
    G_FIELD, G_FIELD_D, G_WHEAT, G_WHEAT_D, G_VINE, G_COUNT
};
static uint16_t s_gbase[2][G_COUNT];

static void ground_colors(void)
{
    uint16_t *n = s_gbase[STYLE_NEON], *r = s_gbase[STYLE_REAL];
    n[G_ASPHALT] = rgb(58, 56, 66);    r[G_ASPHALT] = rgb(78, 79, 82);
    n[G_ASPHALT2] = rgb(70, 66, 76);   r[G_ASPHALT2] = rgb(92, 93, 95);
    n[G_LINE_W] = rgb(230, 230, 225);  r[G_LINE_W] = rgb(214, 214, 208);
    n[G_LINE_Y] = rgb(250, 200, 60);   r[G_LINE_Y] = rgb(220, 178, 60);
    n[G_WALK] = rgb(186, 178, 174);    r[G_WALK] = rgb(184, 178, 168);
    n[G_CURB] = rgb(140, 132, 130);    r[G_CURB] = rgb(150, 146, 140);
    n[G_SAND] = rgb(240, 205, 150);    r[G_SAND] = rgb(226, 206, 166);
    n[G_SAND2] = rgb(225, 186, 130);   r[G_SAND2] = rgb(212, 192, 152);
    n[G_WATER] = rgb(40, 110, 170);    r[G_WATER] = rgb(54, 96, 122);
    n[G_WATER2] = rgb(90, 170, 210);   r[G_WATER2] = rgb(90, 130, 150);
    n[G_GRASS] = rgb(86, 160, 80);     r[G_GRASS] = rgb(92, 134, 64);
    n[G_GRASS2] = rgb(80, 152, 76);    r[G_GRASS2] = rgb(84, 124, 58);
    n[G_LOT] = rgb(176, 168, 162);     r[G_LOT] = rgb(176, 170, 160);
    n[G_LOT2] = rgb(150, 140, 132);    r[G_LOT2] = rgb(162, 156, 146);
    n[G_FOAM] = rgb(235, 245, 250);    r[G_FOAM] = rgb(225, 232, 236);
    n[G_GLINT] = rgb(255, 200, 120);   r[G_GLINT] = rgb(235, 240, 245);
    n[G_FIELD] = rgb(214, 160, 100);   r[G_FIELD] = rgb(178, 140, 96);
    n[G_FIELD_D] = rgb(190, 132, 84);  r[G_FIELD_D] = rgb(154, 118, 80);
    n[G_WHEAT] = rgb(240, 210, 110);   r[G_WHEAT] = rgb(206, 182, 112);
    n[G_WHEAT_D] = rgb(222, 190, 96);  r[G_WHEAT_D] = rgb(190, 166, 100);
    n[G_VINE] = rgb(80, 150, 70);      r[G_VINE] = rgb(78, 104, 52);
}

static inline int ground_index(int X, int Z, int tf, bool real)
{
    /* X, Z in 1/16 m */
    int ix = X >> 4, iz = Z >> 4;
    if ( (WORLD_M <= (unsigned)ix) || (WORLD_M <= (unsigned)iz) )
    {
        if ( MAP_ALBACETE == s_map )
        {
            if ( (ix >> 2) & 1 )
            {
                return G_FIELD;
            }
            return G_FIELD_D;
        }
        if ( 0 == (((ix * 3 + iz * 5 + tf) >> 3) & 15) )
        {
            return G_WATER2;
        }
        return G_WATER;
    }
    int t = world_map[iz >> 2][ix >> 2];
    switch ( t )
    {
        case T_ROAD: {
            int lx = ix & 63, lz = iz & 63;
            int base = G_ASPHALT;
            if ( real )
            {
                uint32_t hh = (uint32_t)(X * 2654435761u ^ Z * 40503u);
                if ( 0 == ((hh >> 11) & 15) )
                {
                    base = G_ASPHALT2;
                }
            }
            if ( (ROAD_W > lx) && (ROAD_W > lz) )
            {
                return base;
            }
            if ( ROAD_W > lz )
            {
                if ( (ROAD_W + 3) > lx )
                {
                    if ( (lz & 1) && (0 < lz) && ((ROAD_W - 1) > lz) )
                    {
                        return G_LINE_W;
                    }
                    return base;
                }
                int q = (Z >> 2) & 255;
                if ( (23 == q) || (24 == q) )
                {
                    return G_LINE_Y;
                }
                if ( (2 == q) || (45 == q) )
                {
                    return G_LINE_W;
                }
                if ( real )
                {
                    return base;
                }
                if ( (0 == ((ix >> 1) & 7)) && ((iz ^ ix) & 1) )
                {
                    return G_ASPHALT2;
                }
                return G_ASPHALT;
            }
            if ( (ROAD_W + 3) > lz )
            {
                if ( (lx & 1) && (0 < lx) && ((ROAD_W - 1) > lx) )
                {
                    return G_LINE_W;
                }
                return base;
            }
            int q = (X >> 2) & 255;
            if ( (23 == q) || (24 == q) )
            {
                return G_LINE_Y;
            }
            if ( (2 == q) || (45 == q) )
            {
                return G_LINE_W;
            }
            if ( real )
            {
                return base;
            }
            if ( (0 == ((iz >> 1) & 7)) && ((iz ^ ix) & 1) )
            {
                return G_ASPHALT2;
            }
            return G_ASPHALT;
        }
        case T_WALK: {
            int lx = ix & 63, lz = iz & 63;
            if ( (ROAD_W == lx) || (ROAD_W == lz) || (63 == lx) || (63 == lz) )
            {
                return G_CURB;
            }
            if ( real )
            {
                if ( (0 == ((X >> 3) & 7)) || (0 == ((Z >> 3) & 7)) )
                {
                    return G_LOT2;
                }
                return G_WALK;
            }
            if ( (ix ^ iz) & 2 )
            {
                return G_WALK;
            }
            return G_LOT;
        }
        case T_SAND: {
            uint32_t h = (uint32_t)(ix * 7919 + iz * 104729);
            if ( 0 == ((h >> 5) & 7) )
            {
                return G_SAND2;
            }
            return G_SAND;
        }
        case T_WATER: {
            int w = ((ix * 3 + iz * 5 + tf) >> 3) & 15;
            if ( 0 == w )
            {
                return G_WATER2;
            }
            return G_WATER;
        }
        case T_GRASS:
        case T_PARK:
            if ( real )
            {
                uint32_t hh = (uint32_t)((X >> 3) * 73856093u ^ (Z >> 3) * 19349663u);
                if ( 0 == ((hh >> 9) & 3) )
                {
                    return G_GRASS2;
                }
                return G_GRASS;
            }
            if ( ((ix >> 1) ^ (iz >> 1)) & 1 )
            {
                return G_GRASS;
            }
            return G_GRASS2;
        case T_FIELD:
            if ( (Z >> 5) & 1 )
            {
                return G_FIELD;          /* furrows */
            }
            return G_FIELD_D;
        case T_FIELD2:
            if ( 0 == ((X >> 4) & 3) )
            {
                return G_WHEAT_D;
            }
            return G_WHEAT;
        case T_VINE:
            if ( (4 > ((X >> 3) & 15)) && (5 > ((Z >> 3) & 7)) )
            {
                return G_VINE;
            }
            return G_FIELD;
        default:
            if ( real )
            {
                if ( (0 == ((X >> 4) & 15)) || (0 == ((Z >> 4) & 15)) )
                {
                    return G_LOT2;
                }
                return G_LOT;
            }
            if ( (0 == (ix & 3)) || (0 == (iz & 3)) )
            {
                return G_LOT2;
            }
            return G_LOT;
    }
}

static inline bool in_shadow(int X, int Z)
{
    int cx = X >> 5, cz = Z >> 5;      /* 2 m cells */
    if ( (SH_N <= (unsigned)cx) || (SH_N <= (unsigned)cz) )
    {
        return false;
    }
    return (s_shadow[cz * (SH_N / 8) + (cx >> 3)] >> (cx & 7)) & 1;
}

void world_draw_ground(float time)
{
    static bool inited;
    if ( !inited )
    {
        ground_colors();
        inited = true;
    }
    const r3d_cam *c = r3d_camera();
    bool real = (STYLE_REAL == s_style);
    bool shadows = real && s_has_shadow;
    const uint16_t *gb;
    if ( real )
    {
        gb = s_gbase[1];
    }
    else
    {
        gb = s_gbase[0];
    }
    int hy = (int)horizon_row() + 1;
    if ( 0 > hy )
    {
        hy = 0;
    }
    int tf = (int)(time * 24.0f);
    float start = c->far_dist * 0.45f;
    for ( int y = hy; GH > y; y++ )
    {
        float a = (c->cy - (y + 0.5f)) / c->focal;
        v3 dir = v3add(c->fwd, v3scale(c->up, a));
        if ( -1e-4f <= dir.y )
        {
            continue;
        }
        float t = -c->pos.y / dir.y;
        uint16_t *row = r3d_fb + y * GW;
        float f = (t - start) / (c->far_dist - start);
        if ( 1.0f <= f )
        {
            for ( int x = 0; GW > x; x++ )
            {
                row[x] = r3d_fog;
            }
            continue;
        }
        int fi;
        if ( 0 > f )
        {
            fi = 0;
        }
        else
        {
            fi = (int)(f * 32.0f);
        }
        uint16_t pal[G_COUNT * 2];
        for ( int i = 0; G_COUNT > i; i++ )
        {
            pal[i] = rgb_mix(gb[i], r3d_fog, fi);
            if ( shadows )
            {
                pal[G_COUNT + i] = rgb_mix(rgb_mix(gb[i], rgb(40, 50, 80), 13), r3d_fog, fi);
            }
        }
        v3 p0 = v3add(c->pos, v3scale(dir, t));
        float k = t / c->focal;
        p0 = v3add(p0, v3scale(c->right, k * (0.5f - c->cx)));
        v3 st = v3scale(c->right, k);
        int X = (int)(p0.x * 16.0f), Z = (int)(p0.z * 16.0f);
        int dX = (int)(st.x * 16.0f * 256.0f), dZ = (int)(st.z * 16.0f * 256.0f);
        int Xf = X << 8, Zf = Z << 8;
        int gw = (int)(6 + (y - hy) * 0.35f), gx0 = (int)s_sun_x - gw, gx1 = (int)s_sun_x + gw;
        if ( shadows )
        {
            for ( int x = 0; GW > x; x++ )
            {
                int Xi = Xf >> 8, Zi = Zf >> 8;
                int gi = ground_index(Xi, Zi, tf, true);
                if ( in_shadow(Xi, Zi) )
                {
                    gi += G_COUNT;
                }
                row[x] = pal[gi];
                Xf += dX;
                Zf += dZ;
            }
        }
        else
        {
            for ( int x = 0; GW > x; x++ )
            {
                int gi = ground_index(Xf >> 8, Zf >> 8, tf, real);
                if ( ((G_WATER == gi) || (G_WATER2 == gi)) && (x > gx0) && (x < gx1) && (0 == (((x * 7 + y * 13 + tf * 3) >> 2) & 3)) )
                {
                    gi = G_GLINT;
                }
                row[x] = pal[gi];
                Xf += dX;
                Zf += dZ;
            }
        }
    }
}

/* ---- buildings, trees, props --------------------------------------------- */

static void neon_band(const building_t *b)
{
    float y0 = b->h - 2.4f, y1 = b->h - 1.2f, e = 0.15f;
    float x0 = b->x0 - e, x1 = b->x1 + e, z0 = b->z0 - e, z1 = b->z1 + e;
    v3 c[4] = {v3m(x0, 0, z0), v3m(x1, 0, z0), v3m(x1, 0, z1), v3m(x0, 0, z1)};
    for ( int i = 0; 4 > i; i++ )
    {
        int j = (i + 1) & 3;
        v3 q[4] = {
            v3m(c[i].x, y0, c[i].z), v3m(c[i].x, y1, c[i].z), v3m(c[j].x, y1, c[j].z), v3m(c[j].x, y0, c[j].z),
        };
        r3d_poly_flat(q, 4, b->neon);
    }
}

static void draw_palm(const palm_t *p, float dist)
{
    v3 base = v3m(p->x, 0, p->z);
    bool real = (STYLE_REAL == s_style);
    if ( 1 == p->kind )
    {
        /* a plane tree: a trunk and two blocks of leaves */
        r3d_box(base, 0.5f, p->h * 0.55f, 0.5f, p->lean, rgb(120, 100, 80), rgb(120, 100, 80), 0, 0);
        uint16_t g1, g2;
        if ( real )
        {
            g1 = rgb(78, 116, 56);
            g2 = rgb(96, 136, 64);
        }
        else
        {
            g1 = rgb(70, 170, 80);
            g2 = rgb(110, 200, 100);
        }
        r3d_box(v3m(p->x, p->h * 0.45f, p->z), 4.2f, p->h * 0.45f, 4.2f, p->lean, g1, g2, 0, 0);
        if ( 140.0f > dist )
        {
            r3d_box(v3m(p->x, p->h * 0.88f, p->z), 2.8f, 1.4f, 2.8f, p->lean + 0.7f, g1, g2, 0, 0);
        }
        return;
    }
    r3d_box(base, 0.45f, p->h, 0.45f, 0.3f, rgb(150, 110, 80), rgb(150, 110, 80), 0, 0);
    if ( 170.0f < dist )
    {
        return;
    }
    v3 top = v3m(p->x, p->h, p->z);
    uint16_t green, green2;
    if ( real )
    {
        green = rgb(70, 130, 70);
        green2 = rgb(50, 100, 55);
    }
    else
    {
        green = rgb(60, 170, 90);
        green2 = rgb(40, 130, 70);
    }
    for ( int i = 0; 6 > i; i++ )
    {
        float a = i * 1.047f + p->lean;
        float ca = cosf(a), sa = sinf(a);
        v3 tip = v3add(top, v3m(ca * 3.6f, -1.4f, sa * 3.6f));
        v3 mid = v3add(top, v3m(ca * 1.8f, 0.35f, sa * 1.8f));
        v3 side = v3m(-sa * 0.7f, 0, ca * 0.7f);
        v3 q1[4] = {top, v3add(mid, side), tip, v3sub(mid, side)};
        v3 q2[4] = {top, v3sub(mid, side), tip, v3add(mid, side)};
        uint16_t col1, col2;
        if ( i & 1 )
        {
            col1 = green;
            col2 = green2;
        }
        else
        {
            col1 = green2;
            col2 = green;
        }
        r3d_poly_flat(q1, 4, col1);
        r3d_poly_flat(q2, 4, col2);
    }
}

static void draw_lamp(float x, float z, uint16_t head, int phase)
{
    /* a traffic light on the corner's pole: a dark box and the lit lamp */
    if ( 0 <= phase )
    {
        r3d_box(v3m(x, 3.0f, z), 0.45f, 1.2f, 0.45f, 0.0f, rgb(30, 30, 34), rgb(30, 30, 34), 0, 0);
        uint16_t lc;
        float ly;
        if ( 0 == phase )
        {
            lc = rgb(255, 40, 40);
            ly = 3.95f;
        }
        else if ( 1 == phase )
        {
            lc = rgb(255, 180, 30);
            ly = 3.6f;
        }
        else
        {
            lc = rgb(60, 255, 120);
            ly = 3.25f;
        }
        const r3d_cam *cm = r3d_camera();
        float tx = cm->pos.x - x, tz = cm->pos.z - z, tl = sqrtf(tx * tx + tz * tz) + 1e-3f;
        r3d_billboard(v3m(x + tx / tl * 0.32f, ly, z + tz / tl * 0.32f), 0.42f, 0.32f, lc);
    }
    uint16_t pole = rgb(60, 60, 72);
    v3 p1[4] = {v3m(x - 0.12f, 0, z), v3m(x - 0.12f, 6.2f, z), v3m(x + 0.12f, 6.2f, z), v3m(x + 0.12f, 0, z)};
    v3 p2[4] = {p1[3], p1[2], p1[1], p1[0]};
    r3d_poly_flat(p1, 4, pole);
    r3d_poly_flat(p2, 4, pole);
    float head_w;
    if ( STYLE_REAL == s_style )
    {
        head_w = 0.6f;
    }
    else
    {
        head_w = 0.9f;
    }
    r3d_billboard(v3m(x, 6.45f, z), head_w, 0.4f, head);
}

static float s_time;
void world_set_time(float t)
{
    s_time = t;
}

static void draw_billboard(const building_t *b)
{
    float w = (b->x1 - b->x0) * 0.7f, cx = (b->x0 + b->x1) * 0.5f, z = (b->z0 + b->z1) * 0.5f;
    float y0 = b->h + 1.0f, y1 = b->h + 6.0f;
    r3d_box(v3m(cx - w * 0.4f, b->h, z), 0.3f, 1.2f, 0.3f, 0, rgb(50, 50, 60), rgb(50, 50, 60), 0, 0);
    r3d_box(v3m(cx + w * 0.4f, b->h, z), 0.3f, 1.2f, 0.3f, 0, rgb(50, 50, 60), rgb(50, 50, 60), 0, 0);
    uint16_t c1, c2;
    if ( STYLE_REAL == s_style )
    {
        c1 = rgb(236, 236, 230);
        if ( (int)cx & 1 )
        {
            c2 = rgb(200, 40, 40);
        }
        else
        {
            c2 = rgb(30, 90, 170);
        }
    }
    else
    {
        int ph = ((int)(s_time * 0.7f) + (int)(cx)) % 3;
        if ( 0 == ph )
        {
            c1 = rgb(255, 60, 170);
            c2 = rgb(60, 20, 90);
        }
        else if ( 1 == ph )
        {
            c1 = rgb(40, 230, 255);
            c2 = rgb(20, 40, 90);
        }
        else
        {
            c1 = rgb(255, 210, 50);
            c2 = rgb(90, 40, 20);
        }
    }
    r3d_vtx f1[4] = {{v3m(cx - w * 0.5f, y0, z), 0, 0}, {v3m(cx - w * 0.5f, y1, z), 0, 5}, {v3m(cx + w * 0.5f, y1, z), w, 5}, {v3m(cx + w * 0.5f, y0, z), w, 0}};
    r3d_vtx f2[4] = {f1[3], f1[2], f1[1], f1[0]};
    r3d_poly(f1, 4, c1, 2, c2, false);
    r3d_poly(f2, 4, c1, 2, c2, false);
}

static void draw_blade(v3 hub, float a, float len, float w, v3 ax, v3 ay, uint16_t col)
{
    /* a flat blade from the hub, in the plane spanned by ax and ay */
    v3 d = v3add(v3scale(ax, cosf(a)), v3scale(ay, sinf(a)));
    v3 n = v3add(v3scale(ax, -sinf(a)), v3scale(ay, cosf(a)));
    v3 q[4] = {v3add(hub, v3scale(n, -w)), v3add(hub, v3scale(n, w)),
               v3add(v3add(hub, v3scale(d, len)), v3scale(n, w)), v3add(v3add(hub, v3scale(d, len)), v3scale(n, -w))};
    v3 q2[4] = {q[3], q[2], q[1], q[0]};
    r3d_poly_flat(q, 4, col);
    r3d_poly_flat(q2, 4, col);
}

static void draw_props(void)
{
    const r3d_cam *c = r3d_camera();
    for ( int i = 0; i < s_nprop; i++ )
    {
        const prop_t *p = &s_prop[i];
        float dx = p->x - c->pos.x, dz = p->z - c->pos.z;
        if ( dx * dx + dz * dz > c->far_dist * c->far_dist )
        {
            continue;
        }
        if ( PROP_WINDMILL == p->kind )
        {
            if ( !r3d_visible(v3m(p->x, 6, p->z), 9.0f) )
            {
                continue;
            }
            /* a white mill with a dark cap and four sails */
            r3d_box(v3m(p->x, 0, p->z), 4.4f, 8.0f, 4.4f, 0.4f, rgb(240, 238, 230), rgb(240, 238, 230), 0, 0);
            r3d_box(v3m(p->x, 8.0f, p->z), 3.6f, 1.6f, 3.6f, 0.4f, rgb(70, 60, 60), rgb(90, 70, 60), 0, 0);
            v3 fw = v3m(sinf(p->yaw), 0, cosf(p->yaw)), rt = v3m(cosf(p->yaw), 0, -sinf(p->yaw));
            v3 hub = v3add(v3m(p->x, 8.4f, p->z), v3scale(fw, 2.6f));
            for ( int k = 0; 4 > k; k++ )
            {
                draw_blade(hub, s_time * 0.9f + k * 1.5708f, 7.5f, 0.55f, rt, v3m(0, 1, 0), rgb(120, 96, 70));
            }
        }
        else if ( PROP_WHEEL == p->kind )
        {
            if ( !r3d_visible(v3m(p->x, 15, p->z), 18.0f) )
            {
                continue;
            }
            /* the big wheel: two legs, a turning rim of spokes and gondolas */
            v3 hub = v3m(p->x, 16.0f, p->z);
            r3d_box(v3m(p->x - 3.0f, 0, p->z), 0.6f, 16.0f, 0.6f, 0, rgb(200, 200, 210), rgb(200, 200, 210), 0, 0);
            r3d_box(v3m(p->x + 3.0f, 0, p->z), 0.6f, 16.0f, 0.6f, 0, rgb(200, 200, 210), rgb(200, 200, 210), 0, 0);
            float rot = s_time * 0.25f;
            uint16_t spoke;
            if ( STYLE_REAL == s_style )
            {
                spoke = rgb(220, 220, 228);
            }
            else
            {
                spoke = rgb(255, 80, 200);
            }
            for ( int k = 0; 12 > k; k++ )
            {
                float a = rot + k * 0.5236f;
                draw_blade(hub, a, 13.0f, 0.18f, v3m(1, 0, 0), v3m(0, 1, 0), spoke);
                v3 g = v3add(hub, v3m(cosf(a) * 13.0f, sinf(a) * 13.0f - 1.2f, 0));
                uint16_t gc;
                if ( 0 == (k % 3) )
                {
                    gc = rgb(230, 60, 60);
                }
                else if ( 1 == (k % 3) )
                {
                    gc = rgb(60, 140, 230);
                }
                else
                {
                    gc = rgb(240, 200, 60);
                }
                r3d_box(g, 1.6f, 1.4f, 1.6f, 0, gc, gc, 0, 0);
            }
        }
    }
}

void world_draw_objects(void)
{
    const r3d_cam *c = r3d_camera();
    bool real = (STYLE_REAL == s_style);
    float far = c->far_dist;
    int bx0 = (int)((c->pos.x - far) / BLOCK_M), bx1 = (int)((c->pos.x + far) / BLOCK_M);
    int bz0 = (int)((c->pos.z - far) / BLOCK_M), bz1 = (int)((c->pos.z + far) / BLOCK_M);
    if ( 0 > bx0 )
    {
        bx0 = 0;
    }
    if ( 0 > bz0 )
    {
        bz0 = 0;
    }
    if ( (BLOCKS - 1) < bx1 )
    {
        bx1 = BLOCKS - 1;
    }
    if ( (BLOCKS - 1) < bz1 )
    {
        bz1 = BLOCKS - 1;
    }
    float pat_dist;
    if ( real )
    {
        pat_dist = 210.0f;
    }
    else
    {
        pat_dist = 150.0f;
    }
    for ( int bz = bz0; bz <= bz1; bz++ )
    {
        for ( int bx = bx0; bx <= bx1; bx++ )
        {
            v3 bc = v3m(bx * BLOCK_M + 38.0f, 20.0f, bz * BLOCK_M + 38.0f);
            if ( !r3d_visible(bc, 90.0f) )
            {
                continue;
            }
            int f = s_block_first[bz][bx], n = s_block_count[bz][bx];
            /* the fair's booths are listed under four blocks: draw them once */
            bool shared = (MAP_ALBACETE == s_map) && ((11 == bx) || (12 == bx)) && ((4 == bz) || (5 == bz)) && !((12 == bx) && (5 == bz));
            if ( shared )
            {
                n = 0;
            }
            for ( int i = f; (f + n) > i; i++ )
            {
                const building_t *b = &s_b[i];
                float hx = (b->x1 - b->x0) * 0.5f, hz = (b->z1 - b->z0) * 0.5f;
                v3 cen = v3m(b->x0 + hx, b->h * 0.5f, b->z0 + hz);
                float rad = sqrtf(hx * hx + hz * hz + b->h * b->h * 0.25f);
                if ( !r3d_visible(cen, rad) )
                {
                    continue;
                }
                float dx = cen.x - c->pos.x, dz = cen.z - c->pos.z;
                float dist = sqrtf(dx * dx + dz * dz) - rad;
                int pat;
                if ( b->pattern && (pat_dist > dist) )
                {
                    pat = b->pattern;
                }
                else
                {
                    pat = 0;
                }
                r3d_box(v3m(cen.x, 0, cen.z), b->x1 - b->x0, b->h, b->z1 - b->z0, 0.0f,
                        b->wall, b->roof, pat, b->glass);
                if ( (1 == b->sign) && (220.0f > dist) )
                {
                    neon_band(b);
                }
                if ( (130.0f > dist) && (9.0f < b->h) )
                {
                    uint32_t h = (uint32_t)i * 2654435761u;
                    float rx = b->x0 + (b->x1 - b->x0) * (0.25f + 0.5f * ((h >> 8) & 255) / 255.0f);
                    float rz = b->z0 + (b->z1 - b->z0) * (0.25f + 0.5f * ((h >> 16) & 255) / 255.0f);
                    if ( 0 == (h & 3) )
                    {
                        r3d_box(v3m(rx, b->h, rz), 0.25f, 9.0f, 0.25f, 0, rgb(70, 70, 80), rgb(255, 40, 60), 0, 0);
                    }
                    else if ( 1 == (h & 3) )
                    {
                        r3d_box(v3m(rx, b->h, rz), 3.0f, 3.2f, 3.0f, 0.4f, rgb(150, 120, 100), rgb(120, 95, 80), 0, 0);
                    }
                    else
                    {
                        r3d_box(v3m(rx, b->h, rz), 4.0f, 1.6f, 2.6f, 0, rgb(170, 175, 180), rgb(140, 145, 150), 0, 0);
                    }
                }
                if ( b->sign && (14.0f < b->h) && (45.0f > b->h) && (0 == ((i * 7) % 3)) && (200.0f > dist) )
                {
                    draw_billboard(b);
                }
            }
            {
                float ox = bx * BLOCK_M, oz = bz * BLOCK_M;
                float dx = ox + 32 - c->pos.x, dz = oz + 32 - c->pos.z;
                if ( ((130.0f * 130.0f) > (dx * dx + dz * dz)) && (T_WALK == world_type(ox + ROAD_W + 1, oz + ROAD_W + 1)) )
                {
                    uint16_t head;
                    if ( real )
                    {
                        head = rgb(236, 232, 210);
                    }
                    else if ( (bx + bz) & 1 )
                    {
                        head = rgb(255, 80, 200);
                    }
                    else
                    {
                        head = rgb(80, 240, 255);
                    }
                    float a0 = ROAD_W + 1.0f, a1 = BLOCK_M - 1.0f;
                    float lx[4] = {a0, a1, a0, a1}, lz[4] = {a0, a0, a1, a1};
                    /* the lights cycle: 7 s green, 2 s amber, 9 s red, the two roads out of step */
                    float cyc = fmodf(s_time + (float)((bx * 7 + bz * 3) % 5) * 3.0f, 18.0f);
                    int ph_main;
                    if ( 7.0f > cyc )
                    {
                        ph_main = 2;
                    }
                    else if ( 9.0f > cyc )
                    {
                        ph_main = 1;
                    }
                    else
                    {
                        ph_main = 0;
                    }
                    bool near = ((95.0f * 95.0f) > (dx * dx + dz * dz));
                    for ( int k = 0; 4 > k; k++ )
                    {
                        if ( r3d_visible(v3m(ox + lx[k], 3.4f, oz + lz[k]), 3.6f) )
                        {
                            int phase;
                            if ( near && (0 == k) )
                            {
                                phase = ph_main;
                            }
                            else if ( near && (3 == k) )
                            {
                                if ( 2 == ph_main )
                                {
                                    phase = 0;
                                }
                                else if ( 0 == ph_main )
                                {
                                    phase = 2;
                                }
                                else
                                {
                                    phase = 1;
                                }
                            }
                            else
                            {
                                phase = -1;
                            }
                            draw_lamp(ox + lx[k], oz + lz[k], head, phase);
                        }
                    }
                }
            }
            int pf = s_palm_first[bz][bx], pn = s_palm_count[bz][bx];
            for ( int i = pf; (pf + pn) > i; i++ )
            {
                const palm_t *p = &s_palm[i];
                v3 pc = v3m(p->x, p->h * 0.5f, p->z);
                if ( !r3d_visible(pc, p->h) )
                {
                    continue;
                }
                float dx = p->x - c->pos.x, dz = p->z - c->pos.z;
                draw_palm(p, sqrtf(dx * dx + dz * dz));
            }
        }
    }
    draw_props();
}
