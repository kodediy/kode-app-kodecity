/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file game.c
 * @brief KodeCity: drive, walk, shoot, run from the police, do jobs.
 *
 * Units are metres and seconds. The ground is the x-z plane, y is up, a yaw
 * of 0 faces +z and the right hand of that facing is +x.
 */
#include "game.h"
#include "r3d.h"
#include "world.h"
#include "sound.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define PI 3.14159265f

/* ---- small helpers -------------------------------------------------------- */

static uint32_t s_rng = 0x2545F491u;
static uint32_t rnd(void)
{
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5;
    return s_rng;
}
static float frand(float a, float b) { return a + (b - a) * (float)(rnd() & 0xFFFF) / 65535.0f; }
static float clampf(float v, float a, float b)
{
    /* below a gives a, else above b gives b, else v */
    float r = v;
    if ( v > b )
    {
        r = b;
    }
    if ( v < a )
    {
        r = a;
    }
    return r;
}
static float wrap_pi(float a)
{
    while ( PI < a )
    {
        a -= 2 * PI;
    }
    while ( -PI > a )
    {
        a += 2 * PI;
    }
    return a;
}
static float approach(float v, float target, float step)
{
    if ( v < target )
    {
        if ( v + step > target )
        {
            return target;
        }
        return v + step;
    }
    if ( v - step < target )
    {
        return target;
    }
    return v - step;
}
static float dist2(float ax, float az, float bx, float bz)
{
    return (ax - bx) * (ax - bx) + (az - bz) * (az - bz);
}

/* ---- the things in the street -------------------------------------------- */

enum { CAR_FREE, CAR_TRAFFIC, CAR_PARKED, CAR_PLAYER, CAR_POLICE, CAR_WRECK };
enum { KIND_SEDAN, KIND_TAXI, KIND_SPORT, KIND_POLICE, KIND_VAN };

typedef struct {
    bool used;
    int state, kind;
    float x, z, yaw;
    float speed;          /* along the heading */
    float slip;           /* sideways, for the drift */
    float steer;
    float hp;
    uint16_t color;
    /* traffic: the point it drives to, and where it goes after */
    float tx, tz;
    int dir;              /* 0 +x, 1 +z, 2 -x, 3 -z */
    bool at_cross;
    float stuck, reverse;
    float burn;           /* a wreck smokes, then goes */
    bool braking;
    float y, vy;          /* a wreck jumps */
    float smoke;          /* tyre smoke from a drift, 0..1 */
} car_t;

enum { PED_WALK, PED_FLEE, PED_DEAD };

typedef struct {
    bool used;
    int state;
    float x, z, yaw, speed;
    float t;               /* walk phase, or time dead */
    float fear;
    uint16_t shirt, pants, skin;
    int bx, bz;            /* the block whose sidewalk it walks */
    int side;              /* 0..3 */
    int sense;             /* +1 / -1 around the block */
    float vx, vy, vz, y;   /* thrown by a car */
} ped_t;

typedef struct { bool used; v3 a, b; float ttl; uint16_t c; } tracer_t;
typedef struct { bool used; float x, z, t; } blast_t;
typedef struct { bool used; v3 p; float t; } spark_t;

#define MAX_CARS    30
#define MAX_PEDS    30
#define MAX_TRACERS 12
#define MAX_BLASTS  6
#define N_TRAFFIC   16
#define N_PEDS      22

static car_t s_car[MAX_CARS];
static ped_t s_ped[MAX_PEDS];
static tracer_t s_tr[MAX_TRACERS];
static blast_t s_bl[MAX_BLASTS];
#define MAX_SPARKS 8
static spark_t s_sp[MAX_SPARKS];
/* stunt ramps: start point, direction (0 +x, 1 +z, 2 -x, 3 -z) */
typedef struct { float x, z; int dir; } ramp_t;
static const ramp_t RAMPS[] = {
    {521.0f, 805.0f, 1}, {201.0f, 590.0f, 1}, {598.0f, 643.0f, 0}, {771.0f, 520.0f, 3}, {457.0f, 387.0f, 2},
};
#define N_RAMPS ((int)(sizeof RAMPS / sizeof RAMPS[0]))
#define RAMP_LEN 7.0f
#define RAMP_H   1.9f
#define RAMP_W   4.2f

/* how far along a ramp a point is (0..1), or -1 when it is not on one */
static float ramp_at(float x, float z, int *which)
{
    static const float DX[4] = {1, 0, -1, 0}, DZ[4] = {0, 1, 0, -1};
    for ( int i = 0; N_RAMPS > i; i++ )
    {
        const ramp_t *r = &RAMPS[i];
        float ax = x - r->x, az = z - r->z;
        float along = ax * DX[r->dir] + az * DZ[r->dir];
        float side = ax * DZ[r->dir] - az * DX[r->dir];
        if ( (0 <= along) && (RAMP_LEN >= along) && (RAMP_W * 0.5f >= fabsf(side)) )
        {
            *which = i;
            return along / RAMP_LEN;
        }
    }
    return -1.0f;
}

static void add_spark(v3 p)
{
    for ( int i = 0; MAX_SPARKS > i; i++ )
    {
        if ( !s_sp[i].used )
        {
            s_sp[i] = (spark_t){true, p, 0};
            return;
        }
    }
}

/* ---- the player ------------------------------------------------------------ */

enum { ST_TITLE, ST_PLAY, ST_MAP, ST_BUSTED, ST_KO };

static struct {
    int state;
    float time, state_t;
    float x, z, yaw, walk_t;
    int car;               /* -1 on foot */
    float hp;
    int money, shown_money;
    float wanted;          /* 0..5 */
    float calm;            /* seconds with no police near */
    float fire_cd;
    float flash;           /* muzzle flash */
    uint32_t prev_btn;
    float chord_t;
    bool chord_used;
    /* the job */
    int job_place;
    float job_t;
    int jobs_done;
    char msg[32];
    float msg_t;
    uint16_t msg_c;
    /* the camera */
    v3 cam_pos, cam_look;
    bool cam_init;
    float shake;
    float rumble;
    float siren_t;
    float air_t;
    /* the police helicopter */
    bool heli;
    float hx, hz, hy, hyaw, rotor;
} P;

static char s_status[192];
static int s_opt_map = MAP_NEONBAY, s_opt_style = STYLE_NEON, s_menu;

/* ---- spawning -------------------------------------------------------------- */

static const uint16_t *car_colors(void)
{
    static uint16_t c[10], r[10];
    static bool done;
    if ( STYLE_REAL == s_opt_style )
    {
        static bool rdone;
        if ( !rdone )
        {
            r[0] = rgb(200, 200, 205);
            r[1] = rgb(240, 240, 240);
            r[2] = rgb(30, 30, 34);
            r[3] = rgb(40, 60, 110);
            r[4] = rgb(150, 30, 34);
            r[5] = rgb(120, 124, 130);
            r[6] = rgb(60, 90, 70);
            r[7] = rgb(210, 205, 190);
            r[8] = rgb(90, 30, 40);
            r[9] = rgb(170, 175, 180);
            rdone = true;
        }
        return r;
    }
    if ( !done )
    {
        c[0] = rgb(230, 60, 80);
        c[1] = rgb(40, 200, 220);
        c[2] = rgb(250, 250, 245);
        c[3] = rgb(30, 30, 40);
        c[4] = rgb(255, 140, 40);
        c[5] = rgb(120, 220, 120);
        c[6] = rgb(170, 90, 230);
        c[7] = rgb(250, 210, 60);
        c[8] = rgb(90, 120, 230);
        c[9] = rgb(200, 200, 210);
        done = true;
    }
    return c;
}

static int car_alloc(void)
{
    for ( int i = 0; MAX_CARS > i; i++ )
    {
        if ( !s_car[i].used )
        {
            memset(&s_car[i], 0, sizeof s_car[i]);
            s_car[i].used = true;
            return i;
        }
    }
    return -1;
}

static const float DIR_X[4] = {1, 0, -1, 0}, DIR_Z[4] = {0, 1, 0, -1};

/* the lane point on a crossing's edge for leaving it in direction d */
static void lane_exit(float cx, float cz, int d, float *x, float *z)
{
    /* right-hand traffic: heading +z keeps to +x, heading +x keeps to -z */
    float off = 3.0f;
    switch ( d )
    {
        case 0: *x = cx + 8.0f; *z = cz - off; break;
        case 1: *x = cx + off; *z = cz + 8.0f; break;
        case 2: *x = cx - 8.0f; *z = cz + off; break;
        default: *x = cx - off; *z = cz - 8.0f; break;
    }
}

/* the lane point before the next crossing in direction d */
static void lane_approach(float cx, float cz, int d, float *x, float *z)
{
    float ncx = cx + DIR_X[d] * BLOCK_M, ncz = cz + DIR_Z[d] * BLOCK_M;
    float off = 3.0f;
    switch ( d )
    {
        case 0: *x = ncx - 8.0f; *z = ncz - off; break;
        case 1: *x = ncx + off; *z = ncz - 8.0f; break;
        case 2: *x = ncx + 8.0f; *z = ncz + off; break;
        default: *x = ncx - off; *z = ncz + 8.0f; break;
    }
}

static bool road_ok(float cx, float cz, int d)
{
    /* the road from this crossing to the next in d exists */
    for ( int k = 1; 4 >= k; k++ )
    {
        float x = cx + DIR_X[d] * 16.0f * k, z = cz + DIR_Z[d] * 16.0f * k;
        if ( !world_is_road(x, z) )
        {
            return false;
        }
    }
    return true;
}

static void traffic_next(car_t *c)
{
    float cx, cz;
    world_nearest_crossing(c->x, c->z, &cx, &cz);
    if ( !c->at_cross )
    {
        /* arrived before a crossing: choose where to go */
        int opts[3] = {c->dir, (c->dir + 1) & 3, (c->dir + 3) & 3};
        int pick = -1;
        int first;
        if ( 6 > (rnd() % 10) )
        {
            first = 0;
        }
        else
        {
            first = 1 + (int)(rnd() & 1);
        }
        for ( int k = 0; (3 > k) && (0 > pick); k++ )
        {
            int d = opts[(first + k) % 3];
            if ( road_ok(cx, cz, d) )
            {
                pick = d;
            }
        }
        if ( 0 > pick )
        {
            pick = (c->dir + 2) & 3;
        }
        c->dir = pick;
        lane_exit(cx, cz, pick, &c->tx, &c->tz);
        c->at_cross = true;
    }
    else
    {
        lane_approach(cx, cz, c->dir, &c->tx, &c->tz);
        c->at_cross = false;
    }
}

static void spawn_traffic(int i, float px, float pz, float rmin, float rmax)
{
    for ( int tries = 0; 20 > tries; tries++ )
    {
        float a = frand(0, 2 * PI), r = frand(rmin, rmax);
        float cx, cz;
        world_nearest_crossing(px + cosf(a) * r, pz + sinf(a) * r, &cx, &cz);
        int d = (int)(rnd() & 3);
        if ( !road_ok(cx, cz, d) || !world_is_road(cx, cz) )
        {
            continue;
        }
        car_t *c = &s_car[i];
        memset(c, 0, sizeof *c);
        c->used = true;
        c->state = CAR_TRAFFIC;
        c->dir = d;
        lane_exit(cx, cz, d, &c->x, &c->z);
        c->x += DIR_X[d] * frand(0, 30);
        c->z += DIR_Z[d] * frand(0, 30);
        c->yaw = atan2f(DIR_X[d], DIR_Z[d]);
        c->at_cross = false;
        lane_approach(cx, cz, d, &c->tx, &c->tz);
        int k = (int)(rnd() % 10);
        if ( 2 > k )
        {
            c->kind = KIND_TAXI;
        }
        else if ( 3 > k )
        {
            c->kind = KIND_SPORT;
        }
        else if ( 4 > k )
        {
            c->kind = KIND_VAN;
        }
        else
        {
            c->kind = KIND_SEDAN;
        }
        if ( KIND_TAXI == c->kind )
        {
            c->color = rgb(250, 200, 40);
        }
        else
        {
            c->color = car_colors()[rnd() % 10];
        }
        c->hp = 100;
        c->speed = 9;
        return;
    }
    s_car[i].used = false;
}

static void spawn_police(float px, float pz)
{
    int i = car_alloc();
    if ( 0 > i )
    {
        return;
    }
    for ( int tries = 0; 20 > tries; tries++ )
    {
        float a = frand(0, 2 * PI), r = frand(70, 110);
        float cx, cz;
        world_nearest_crossing(px + cosf(a) * r, pz + sinf(a) * r, &cx, &cz);
        if ( !world_is_road(cx, cz) || !world_free(cx, cz, 2.0f, true) )
        {
            continue;
        }
        car_t *c = &s_car[i];
        c->state = CAR_POLICE;
        c->kind = KIND_POLICE;
        c->x = cx;
        c->z = cz;
        c->yaw = atan2f(px - cx, pz - cz);
        c->color = rgb(245, 245, 250);
        c->hp = 140;
        return;
    }
    s_car[i].used = false;
}

static void ped_place(ped_t *p)
{
    float ox = p->bx * BLOCK_M, oz = p->bz * BLOCK_M;
    float a0 = ROAD_W + 2.0f, a1 = BLOCK_M - 2.0f;
    float t = frand(a0, a1);
    switch ( p->side )
    {
        case 0: p->x = ox + t; p->z = oz + a0; break;
        case 1: p->x = ox + a1; p->z = oz + t; break;
        case 2: p->x = ox + t; p->z = oz + a1; break;
        default: p->x = ox + a0; p->z = oz + t; break;
    }
}

static void spawn_ped(int i, float px, float pz)
{
    ped_t *p = &s_ped[i];
    for ( int tries = 0; 20 > tries; tries++ )
    {
        int bx = (int)((px + frand(-140, 140)) / BLOCK_M), bz = (int)((pz + frand(-140, 140)) / BLOCK_M);
        if ( (0 > bx) || (0 > bz) || (BLOCKS - 1 <= bx) || (BLOCKS - 1 <= bz) )
        {
            continue;
        }
        memset(p, 0, sizeof *p);
        p->bx = bx;
        p->bz = bz;
        p->side = (int)(rnd() & 3);
        if ( rnd() & 1 )
        {
            p->sense = 1;
        }
        else
        {
            p->sense = -1;
        }
        ped_place(p);
        if ( (T_WATER == world_type(p->x, p->z)) || !world_free(p->x, p->z, 0.4f, true) )
        {
            continue;
        }
        if ( 30.0f * 30.0f > dist2(p->x, p->z, px, pz) )
        {
            continue;
        }
        p->used = true;
        p->state = PED_WALK;
        p->speed = frand(1.1f, 1.6f);
        static const uint8_t SH[][3] = {{255, 90, 120}, {80, 200, 255}, {255, 230, 90}, {250, 250, 250}, {140, 230, 140}, {255, 150, 60}, {180, 120, 250}};
        const uint8_t *s = SH[rnd() % 7];
        p->shirt = rgb(s[0], s[1], s[2]);
        if ( rnd() & 1 )
        {
            p->pants = rgb(50, 60, 110);
        }
        else
        {
            p->pants = rgb(230, 220, 200);
        }
        static const uint8_t SK[][3] = {{240, 200, 170}, {200, 150, 110}, {140, 95, 70}, {90, 60, 45}};
        const uint8_t *k = SK[rnd() % 4];
        p->skin = rgb(k[0], k[1], k[2]);
        return;
    }
}

/* ---- messages ---------------------------------------------------------- */

static void say(const char *m, uint16_t c, float t)
{
    snprintf(P.msg, sizeof P.msg, "%s", m);
    P.msg_c = c;
    P.msg_t = t;
}

static void new_job(void)
{
    const place_t *pl;
    int n = world_places(&pl);
    int k;
    do
    {
        k = (int)(rnd() % n);
    } while ( (k == P.job_place) && (1 < n) );
    P.job_place = k;
    float d = sqrtf(dist2(pl[k].x, pl[k].z, P.x, P.z));
    P.job_t = 20.0f + d / 9.0f;
}

/* ---- game start --------------------------------------------------------- */

/* on the pavement of the avenue that runs north past the park of block (7, 11) */
#define SPAWN_X 526.5f
#define SPAWN_Z 726.0f

static void respawn(float x, float z)
{
    for ( int i = 0; MAX_CARS > i; i++ )
    {
        s_car[i].used = false;
    }
    for ( int i = 0; MAX_PEDS > i; i++ )
    {
        s_ped[i].used = false;
    }
    P.x = x; P.z = z; P.yaw = 0.0f;
    P.car = -1;
    P.hp = 100;
    P.wanted = 0;
    P.heli = false;
    P.cam_init = false;
    /* a car waiting at the kerb, nose up the avenue */
    int i = car_alloc();
    if ( 0 <= i )
    {
        car_t *c = &s_car[i];
        c->state = CAR_PARKED;
        c->kind = KIND_SPORT;
        if ( STYLE_REAL == s_opt_style )
        {
            c->color = rgb(175, 24, 34);
        }
        else
        {
            c->color = rgb(255, 60, 150);
        }
        c->x = x - 5.0f;
        c->z = z + 3.0f;
        c->yaw = 0.0f;
        c->hp = 100;
    }
}

static void rebuild_world(void)
{
    world_init(1234, s_opt_map, s_opt_style);
}

void game_set_options(int map, int style)
{
    s_opt_map = map;
    s_opt_style = style;
    rebuild_world();
    respawn(SPAWN_X, SPAWN_Z);
    P.job_place = -1;
    new_job();
}

void game_init(uint32_t seed)
{
    if ( 0U != seed )
    {
        s_rng = seed;
    }
    else
    {
        s_rng = 0xC0FFEEu;
    }
    rebuild_world();
    memset(&P, 0, sizeof P);
    memset(s_car, 0, sizeof s_car);
    memset(s_ped, 0, sizeof s_ped);
    P.state = ST_TITLE;
    P.money = 0;
    P.job_place = -1;
    respawn(SPAWN_X, SPAWN_Z);
    new_job();
}

void game_start_now(void)
{
    P.state = ST_PLAY;
    P.state_t = 0;
    P.cam_init = false;
}

uint8_t game_rumble(void) { return (uint8_t)clampf(P.rumble * 255.0f, 0, 255); }
const char *game_status(void) { return s_status; }

void game_debug(int what)
{
    if ( 1 == what )
    {
        P.wanted = clampf(P.wanted + 1.0f, 0, 5);
        P.calm = 0;
    }
    if ( 2 == what )
    {
        P.money += 1000;
    }
    if ( 3 == what )
    {
        P.wanted = 0;
    }
    if ( 4 == what )
    {
        int map;
        if ( MAP_NEONBAY == s_opt_map )
        {
            map = MAP_ALBACETE;
        }
        else
        {
            map = MAP_NEONBAY;
        }
        game_set_options(map, s_opt_style);
    }
    if ( 5 == what )
    {
        int style;
        if ( STYLE_NEON == s_opt_style )
        {
            style = STYLE_REAL;
        }
        else
        {
            style = STYLE_NEON;
        }
        game_set_options(s_opt_map, style);
    }
}

/* ---- shooting --------------------------------------------------------- */

static void add_tracer(v3 a, v3 b, uint16_t c)
{
    for ( int i = 0; MAX_TRACERS > i; i++ )
    {
        if ( !s_tr[i].used )
        {
            s_tr[i] = (tracer_t){true, a, b, 0.07f, c};
            return;
        }
    }
}

static void add_wanted(float w)
{
    P.wanted = clampf(P.wanted + w, 0, 5);
    P.calm = 0;
}

static void blast(float x, float z)
{
    for ( int i = 0; MAX_BLASTS > i; i++ )
    {
        if ( !s_bl[i].used )
        {
            s_bl[i] = (blast_t){true, x, z, 0};
            break;
        }
    }
    P.shake = 0.6f;
    P.rumble = 1.0f;
    snd_play(SFX_BOOM);
    for ( int i = 0; MAX_PEDS > i; i++ )
    {
        ped_t *p = &s_ped[i];
        if ( !p->used || (PED_DEAD == p->state) )
        {
            continue;
        }
        if ( 8.0f * 8.0f > dist2(p->x, p->z, x, z) )
        {
            p->state = PED_DEAD;
            p->t = 0;
            float dx = p->x - x, dz = p->z - z, l = sqrtf(dx * dx + dz * dz) + 0.1f;
            p->vx = dx / l * 9.0f;
            p->vz = dz / l * 9.0f;
            p->vy = 8.0f;
            p->y = 0.01f;
        }
        else if ( 40.0f * 40.0f > dist2(p->x, p->z, x, z) )
        {
            p->state = PED_FLEE;
            p->fear = 6;
        }
    }
    float pd = dist2(P.x, P.z, x, z);
    if ( (0 > P.car) && (7.0f * 7.0f > pd) )
    {
        P.hp -= 60;
    }
}

static void wreck(car_t *c)
{
    if ( CAR_WRECK == c->state )
    {
        return;
    }
    bool was_police = CAR_POLICE == c->state;
    bool was_player = ((0 <= P.car) && (&s_car[P.car] == c));
    c->state = CAR_WRECK;
    c->speed = 0;
    c->burn = 0;
    c->color = rgb(45, 40, 40);
    c->vy = 7.0f; c->y = 0.01f;
    blast(c->x, c->z);
    if ( was_police )
    {
        add_wanted(1.2f);
    }
    else
    {
        add_wanted(0.6f);
    }
    if ( was_police )
    {
        P.money += 250;
        say("+$250", rgb(120, 255, 140), 1.2f);
        snd_play(SFX_COIN);
    }
    if ( was_player )
    {
        P.hp = 0;
    }
}

static void fire(void)
{
    float fx = sinf(P.yaw), fz = cosf(P.yaw);
    float ox = P.x, oz = P.z, oy = 1.3f;
    if ( 0 <= P.car )
    {
        ox += fx * 2.6f;
        oz += fz * 2.6f;
        oy = 1.2f;
    }
    float range = 48.0f;
    /* aim help: the nearest thing inside a narrow cone */
    float best = range, cone = 0.22f;
    int best_ped = -1, best_car = -1;
    for ( int i = 0; MAX_PEDS > i; i++ )
    {
        ped_t *p = &s_ped[i];
        if ( !p->used || (PED_DEAD == p->state) )
        {
            continue;
        }
        float dx = p->x - ox, dz = p->z - oz, d = sqrtf(dx * dx + dz * dz);
        if ( (d > best) || (0.3f > d) )
        {
            continue;
        }
        float a = fabsf(wrap_pi(atan2f(dx, dz) - P.yaw));
        if ( cone + 0.6f / d > a )
        {
            best = d;
            best_ped = i;
            best_car = -1;
        }
    }
    for ( int i = 0; MAX_CARS > i; i++ )
    {
        car_t *c = &s_car[i];
        if ( !c->used || (i == P.car) || (CAR_WRECK == c->state) )
        {
            continue;
        }
        float dx = c->x - ox, dz = c->z - oz, d = sqrtf(dx * dx + dz * dz);
        if ( (d > best) || (0.5f > d) )
        {
            continue;
        }
        float a = fabsf(wrap_pi(atan2f(dx, dz) - P.yaw));
        if ( cone + 1.8f / d > a )
        {
            best = d;
            best_car = i;
            best_ped = -1;
        }
    }
    float tx = ox + fx * best, tz = oz + fz * best, ty = oy;
    float t;
    if ( 0 <= best_ped )
    {
        tx = s_ped[best_ped].x;
        tz = s_ped[best_ped].z;
        ty = 1.1f;
    }
    if ( 0 <= best_car )
    {
        tx = s_car[best_car].x;
        tz = s_car[best_car].z;
        ty = 0.9f;
    }
    if ( 0 <= world_ray(ox, oz, tx, tz, &t) )
    {
        tx = ox + (tx - ox) * t;
        tz = oz + (tz - oz) * t;
        best_ped = best_car = -1;
    }
    add_tracer(v3m(ox, oy, oz), v3m(tx, ty, tz), rgb(255, 240, 150));
    if ( 0 > best_ped )
    {
        add_spark(v3m(tx, ty, tz));
    }
    P.flash = 0.05f;
    snd_play(SFX_GUN);
    P.rumble = fmaxf(P.rumble, 0.35f);
    if ( 0 <= best_ped )
    {
        s_ped[best_ped].state = PED_DEAD;
        s_ped[best_ped].t = 0;
        snd_play(SFX_HIT);
        add_wanted(0.7f);
    }
    if ( 0 <= best_car )
    {
        car_t *c = &s_car[best_car];
        c->hp -= 18;
        if ( CAR_POLICE == c->state )
        {
            add_wanted(0.2f);
        }
        if ( 0 >= c->hp )
        {
            wreck(c);
        }
    }
    /* anyone near the noise runs */
    for ( int i = 0; MAX_PEDS > i; i++ )
    {
        ped_t *p = &s_ped[i];
        if ( p->used && (PED_DEAD != p->state) && (35.0f * 35.0f > dist2(p->x, p->z, P.x, P.z)) )
        {
            p->state = PED_FLEE;
            p->fear = 5;
        }
    }
}

/* ---- driving -------------------------------------------------------------- */

static bool car_fits(float x, float z, float yaw)
{
    float fx = sinf(yaw), fz = cosf(yaw);
    return world_free(x + fx * 1.4f, z + fz * 1.4f, 1.05f, true) &&
           world_free(x - fx * 1.4f, z - fz * 1.4f, 1.05f, true);
}

static void car_physics(car_t *c, float throttle, float brake, float steer, float dt, float vmax)
{
    float fwdv = c->speed;
    c->braking = (0 < brake) && (0.5f < fwdv);
    if ( 0 < throttle )
    {
        float acc;
        if ( 0 > fwdv )
        {
            acc = 22.0f;
        }
        else
        {
            acc = 14.0f * (1.0f - fwdv / vmax);
        }
        c->speed += acc * throttle * dt;
    }
    if ( 0 < brake )
    {
        if ( 0.5f < fwdv )
        {
            c->speed -= 26.0f * brake * dt;
        }
        else
        {
            c->speed = approach(c->speed, -9.0f, 12.0f * brake * dt);
        }
    }
    if ( (0 == throttle) && (0 == brake) )
    {
        c->speed = approach(c->speed, 0, 4.0f * dt);
    }
    c->speed -= c->speed * 0.08f * dt;
    c->steer = approach(c->steer, steer, 5.0f * dt);
    /* turning: a little at a crawl, full at town speed, less flat out */
    float sp = fabsf(c->speed);
    float turn = c->steer * clampf(sp / 6.0f, 0, 1) * (1.9f - 0.9f * clampf(sp / 35.0f, 0, 1));
    if ( 0 > c->speed )
    {
        turn = -turn;
    }
    c->yaw = wrap_pi(c->yaw + turn * dt);
    /* the drift: the tail steps out in a fast turn and comes back */
    c->slip += (-c->steer * sp * sp * 0.006f - c->slip * 3.5f) * dt;
    float smoke_to = 0.0f;
    if ( (2.2f < fabsf(c->slip)) || (c->braking && (14.0f < fwdv)) )
    {
        smoke_to = 1.0f;
    }
    c->smoke = approach(c->smoke, smoke_to, dt * 4.0f);
    float fx = sinf(c->yaw), fz = cosf(c->yaw), rx = cosf(c->yaw), rz = -sinf(c->yaw);
    float nx = c->x + (fx * c->speed + rx * c->slip) * dt;
    float nz = c->z + (fz * c->speed + rz * c->slip) * dt;
    if ( car_fits(nx, nz, c->yaw) )
    {
        c->x = nx;
        c->z = nz;
    }
    else if ( car_fits(nx, c->z, c->yaw) )
    {
        c->x = nx;
        c->speed *= 0.8f;
    }
    else if ( car_fits(c->x, nz, c->yaw) )
    {
        c->z = nz;
        c->speed *= 0.8f;
    }
    else
    {
        if ( 12.0f < fabsf(c->speed) )
        {
            c->hp -= fabsf(c->speed) * 0.6f;
            if ( (0 <= P.car) && (c == &s_car[P.car]) )
            {
                P.shake = 0.35f;
                P.rumble = 0.8f;
                snd_play(SFX_CRASH);
            }
        }
        c->speed *= -0.3f;
        c->slip = 0;
    }
}

static void traffic_ai(car_t *c, float dt)
{
    float dx = c->tx - c->x, dz = c->tz - c->z;
    float d = sqrtf(dx * dx + dz * dz);
    if ( 3.0f > d )
    {
        traffic_next(c);
        dx = c->tx - c->x;
        dz = c->tz - c->z;
    }
    float want = atan2f(dx, dz);
    float err = wrap_pi(want - c->yaw);
    float target_v;
    if ( c->at_cross )
    {
        target_v = 6.0f;
    }
    else
    {
        target_v = 11.0f;
    }
    if ( KIND_SPORT == c->kind )
    {
        target_v *= 1.3f;
    }
    /* brake for anything ahead: cars, people, the player */
    float fx = sinf(c->yaw), fz = cosf(c->yaw);
    for ( int i = 0; MAX_CARS > i; i++ )
    {
        car_t *o = &s_car[i];
        if ( !o->used || (o == c) )
        {
            continue;
        }
        float ox = o->x - c->x, oz = o->z - c->z;
        float along = ox * fx + oz * fz, side = fabsf(ox * fz - oz * fx);
        if ( (0 < along) && (11.0f > along) && (2.2f > side) )
        {
            target_v = fminf(target_v, (along - 5.0f) * 1.2f);
        }
    }
    for ( int i = 0; MAX_PEDS > i; i++ )
    {
        ped_t *p = &s_ped[i];
        if ( !p->used || (PED_DEAD == p->state) )
        {
            continue;
        }
        float ox = p->x - c->x, oz = p->z - c->z;
        float along = ox * fx + oz * fz, side = fabsf(ox * fz - oz * fx);
        if ( (0 < along) && (9.0f > along) && (1.8f > side) )
        {
            target_v = fminf(target_v, (along - 3.5f) * 1.2f);
        }
    }
    if ( 0 > P.car )
    {
        float ox = P.x - c->x, oz = P.z - c->z;
        float along = ox * fx + oz * fz, side = fabsf(ox * fz - oz * fx);
        if ( (0 < along) && (9.0f > along) && (1.8f > side) )
        {
            target_v = fminf(target_v, (along - 3.5f) * 1.2f);
        }
    }
    if ( 0 > target_v )
    {
        target_v = 0;
    }
    c->yaw = wrap_pi(c->yaw + clampf(err, -1.6f * dt, 1.6f * dt) * clampf(c->speed / 4.0f, 0.2f, 1.0f) * 1.0f / fmaxf(dt, 1e-3f) * dt);
    float rate;
    if ( target_v < c->speed )
    {
        rate = 14.0f;
    }
    else
    {
        rate = 5.0f;
    }
    c->speed = approach(c->speed, target_v, rate * dt);
    float nx = c->x + sinf(c->yaw) * c->speed * dt, nz = c->z + cosf(c->yaw) * c->speed * dt;
    if ( car_fits(nx, nz, c->yaw) )
    {
        c->x = nx;
        c->z = nz;
    }
    else
    {
        c->speed = 0;
        c->stuck += dt;
    }
    if ( 4.0f < c->stuck )
    {
        c->used = false;  /* give up: it will respawn elsewhere */
    }
}

static void police_ai(car_t *c, float dt)
{
    float px = P.x, pz = P.z;
    float dx = px - c->x, dz = pz - c->z;
    float d = sqrtf(dx * dx + dz * dz);
    float want = atan2f(dx, dz);
    /* look ahead: if a building is in the way, try the sides */
    float look = fminf(d, 14.0f);
    if ( !world_free(c->x + sinf(want) * look, c->z + cosf(want) * look, 1.4f, true) )
    {
        float best = 99, pick = want;
        for ( int k = 1; 4 >= k; k++ )
        {
            for ( int s = -1; 1 >= s; s += 2 )
            {
                float a = want + s * k * 0.4f;
                if ( world_free(c->x + sinf(a) * look, c->z + cosf(a) * look, 1.4f, true) )
                {
                    float e = fabsf(wrap_pi(a - c->yaw));
                    if ( e < best )
                    {
                        best = e;
                        pick = a;
                    }
                }
            }
        }
        want = pick;
    }
    float err = wrap_pi(want - c->yaw);
    float steer = clampf(err * 2.0f, -1, 1);
    float throttle = 1.0f, brake = 0;
    if ( 9.0f > d )
    {
        throttle = 0.25f;
        if ( 6 < c->speed )
        {
            brake = 1;
        }
    }
    if ( 0 < c->reverse )
    {
        c->reverse -= dt;
        throttle = 0;
        brake = 1;
        steer = -steer;
    }
    else if ( (1.0f > fabsf(c->speed)) && (10.0f < d) )
    {
        c->stuck += dt;
        if ( 1.2f < c->stuck )
        {
            c->reverse = 1.0f;
            c->stuck = 0;
        }
    }
    else
    {
        c->stuck = 0;
    }
    car_physics(c, throttle, brake, steer, dt, 31.0f);
}

/* ---- the update ----------------------------------------------------------- */

static void collide_cars(float dt)
{
    (void)dt;
    for ( int i = 0; MAX_CARS > i; i++ )
    {
        car_t *a = &s_car[i];
        if ( !a->used )
        {
            continue;
        }
        for ( int j = i + 1; MAX_CARS > j; j++ )
        {
            car_t *b = &s_car[j];
            if ( !b->used )
            {
                continue;
            }
            float dx = b->x - a->x, dz = b->z - a->z, d2 = dx * dx + dz * dz;
            if ( (3.2f * 3.2f < d2) || (1e-4f > d2) )
            {
                continue;
            }
            float d = sqrtf(d2), push = (3.2f - d) * 0.5f;
            float nx = dx / d, nz = dz / d;
            float rel = fabsf(a->speed - b->speed);
            if ( car_fits(a->x - nx * push, a->z - nz * push, a->yaw) )
            {
                a->x -= nx * push;
                a->z -= nz * push;
            }
            if ( car_fits(b->x + nx * push, b->z + nz * push, b->yaw) )
            {
                b->x += nx * push;
                b->z += nz * push;
            }
            if ( 6.0f < rel )
            {
                a->hp -= rel * 0.5f;
                b->hp -= rel * 0.5f;
                if ( (i == P.car) || (j == P.car) )
                {
                    P.shake = 0.3f;
                    P.rumble = 0.7f;
                    snd_play(SFX_CRASH);
                    car_t *o;
                    if ( i == P.car )
                    {
                        o = b;
                    }
                    else
                    {
                        o = a;
                    }
                    if ( CAR_TRAFFIC == o->state )
                    {
                        add_wanted(0.15f);
                    }
                }
                if ( 0 >= a->hp )
                {
                    wreck(a);
                }
                if ( 0 >= b->hp )
                {
                    wreck(b);
                }
            }
            float avg = (a->speed + b->speed) * 0.5f;
            a->speed = avg * 0.6f;
            b->speed = avg * 0.6f;
        }
    }
}

static void update_peds(float dt)
{
    for ( int i = 0; MAX_PEDS > i; i++ )
    {
        ped_t *p = &s_ped[i];
        if ( !p->used )
        {
            continue;
        }
        if ( PED_DEAD == p->state )
        {
            if ( 0 < p->y )
            {
                p->vy -= 20.0f * dt;
                float nx = p->x + p->vx * dt, nz = p->z + p->vz * dt;
                if ( world_free(nx, nz, 0.3f, false) )
                {
                    p->x = nx;
                    p->z = nz;
                }
                else
                {
                    p->vx = -p->vx * 0.3f;
                    p->vz = -p->vz * 0.3f;
                }
                p->y += p->vy * dt;
                p->yaw += dt * 9.0f;
                if ( 0 >= p->y )
                {
                    p->y = 0;
                    p->vx = p->vz = 0;
                }
            }
            p->t += dt;
            if ( 25.0f < p->t )
            {
                p->used = false;
            }
            continue;
        }
        /* cars hit people */
        for ( int k = 0; MAX_CARS > k; k++ )
        {
            car_t *c = &s_car[k];
            if ( !c->used || (3.5f > fabsf(c->speed)) || (1.2f < c->y) )
            {
                continue;
            }
            if ( 1.9f * 1.9f > dist2(c->x, c->z, p->x, p->z) )
            {
                p->state = PED_DEAD;
                p->t = 0;
                p->vx = sinf(c->yaw) * c->speed * 0.7f;
                p->vz = cosf(c->yaw) * c->speed * 0.7f;
                p->vy = 3.0f + fabsf(c->speed) * 0.15f;
                p->y = 0.01f;
                c->speed *= 0.85f;
                if ( k == P.car )
                {
                    add_wanted(0.4f);
                    P.rumble = 0.5f;
                    snd_play(SFX_HIT);
                }
                break;
            }
        }
        if ( PED_DEAD == p->state )
        {
            continue;
        }
        float sp = p->speed;
        if ( PED_FLEE == p->state )
        {
            p->fear -= dt;
            float away = atan2f(p->x - P.x, p->z - P.z);
            p->yaw = away + sinf(p->t * 3.0f + i) * 0.4f;
            sp = 4.5f;
            if ( 0 >= p->fear )
            {
                p->state = PED_WALK;
            }
        }
        else
        {
            /* round the block, on the pavement */
            float ox = p->bx * BLOCK_M, oz = p->bz * BLOCK_M;
            float a0 = ROAD_W + 2.0f, a1 = BLOCK_M - 2.0f;
            float cx[4] = {ox + a1, ox + a1, ox + a0, ox + a0}, cz[4] = {oz + a0, oz + a1, oz + a1, oz + a0};
            /* corner k ends side k going +sense */
            int k;
            if ( 0 < p->sense )
            {
                k = p->side;
            }
            else
            {
                k = (p->side + 3) & 3;
            }
            float tx = cx[k], tz = cz[k];
            if ( 0.5f > dist2(p->x, p->z, tx, tz) )
            {
                int step;
                if ( 0 < p->sense )
                {
                    step = 1;
                }
                else
                {
                    step = 3;
                }
                p->side = (p->side + step) & 3;
                if ( 0 == (rnd() % 9) )
                {
                    p->sense = -p->sense;
                }
            }
            p->yaw = atan2f(tx - p->x, tz - p->z);
        }
        float nx = p->x + sinf(p->yaw) * sp * dt, nz = p->z + cosf(p->yaw) * sp * dt;
        if ( world_free(nx, nz, 0.35f, true) )
        {
            p->x = nx;
            p->z = nz;
        }
        else
        {
            p->yaw += 1.5f;
        }
        p->t += dt * sp * 2.2f;
    }
}

static int nearest_car(float x, float z, float r)
{
    int best = -1;
    float bd = r * r;
    for ( int i = 0; MAX_CARS > i; i++ )
    {
        car_t *c = &s_car[i];
        if ( !c->used || (CAR_WRECK == c->state) )
        {
            continue;
        }
        float d = dist2(c->x, c->z, x, z);
        if ( d < bd )
        {
            bd = d;
            best = i;
        }
    }
    return best;
}

static void enter_or_leave(void)
{
    if ( 0 <= P.car )
    {
        car_t *c = &s_car[P.car];
        if ( 6.0f < fabsf(c->speed) )
        {
            return;
        }
        float rx = cosf(c->yaw), rz = -sinf(c->yaw);
        float ex = c->x - rx * 2.2f, ez = c->z - rz * 2.2f;
        if ( !world_free(ex, ez, 0.4f, true) )
        {
            ex = c->x + rx * 2.2f;
            ez = c->z + rz * 2.2f;
        }
        if ( !world_free(ex, ez, 0.4f, true) )
        {
            return;
        }
        c->state = CAR_PARKED;
        c->speed = 0;
        snd_play(SFX_DOOR);
        P.x = ex;
        P.z = ez;
        P.yaw = c->yaw;
        P.car = -1;
    }
    else
    {
        int i = nearest_car(P.x, P.z, 6.0f);
        if ( 0 > i )
        {
            return;
        }
        car_t *c = &s_car[i];
        if ( (CAR_TRAFFIC == c->state) || (CAR_POLICE == c->state) )
        {
            /* the driver gets out and runs */
            for ( int k = 0; MAX_PEDS > k; k++ )
            {
                if ( s_ped[k].used )
                {
                    continue;
                }
                spawn_ped(k, c->x, c->z);
                if ( !s_ped[k].used )
                {
                    break;
                }
                ped_t *d = &s_ped[k];
                float rx = cosf(c->yaw), rz = -sinf(c->yaw);
                d->x = c->x - rx * 1.6f;
                d->z = c->z - rz * 1.6f;
                d->state = PED_FLEE;
                d->fear = 6;
                if ( CAR_POLICE == c->state )
                {
                    d->shirt = rgb(30, 50, 120);
                    d->pants = rgb(30, 30, 50);
                }
                break;
            }
        }
        if ( CAR_TRAFFIC == c->state )
        {
            add_wanted(0.3f);   /* you took someone's car */
        }
        if ( CAR_POLICE == c->state )
        {
            add_wanted(1.0f);
        }
        c->state = CAR_PLAYER;
        P.car = i;
        snd_play(SFX_DOOR);
        P.yaw = c->yaw;
        if ( KIND_POLICE == c->kind )
        {
            say("POLICE CAR!", rgb(255, 255, 255), 1.6f);
        }
        else
        {
            say("RADIO NEON 88.1", rgb(255, 120, 220), 1.6f);
        }
    }
}

static void manage_population(void)
{
    int traffic = 0, police = 0, peds = 0;
    for ( int i = 0; MAX_CARS > i; i++ )
    {
        car_t *c = &s_car[i];
        if ( !c->used || (i == P.car) )
        {
            continue;
        }
        float d2 = dist2(c->x, c->z, P.x, P.z);
        if ( (CAR_WRECK == c->state) && (30.0f < c->burn) )
        {
            c->used = false;
            continue;
        }
        if ( 280.0f * 280.0f < d2 )
        {
            c->used = false;
            continue;
        }
        if ( CAR_TRAFFIC == c->state )
        {
            traffic++;
        }
        if ( CAR_POLICE == c->state )
        {
            police++;
            if ( (1.0f > P.wanted) && (120.0f * 120.0f < d2) )
            {
                c->used = false;
            }
        }
    }
    for ( int i = 0; MAX_PEDS > i; i++ )
    {
        ped_t *p = &s_ped[i];
        if ( !p->used )
        {
            continue;
        }
        if ( 200.0f * 200.0f < dist2(p->x, p->z, P.x, P.z) )
        {
            p->used = false;
            continue;
        }
        peds++;
    }
    if ( N_TRAFFIC > traffic )
    {
        int i = car_alloc();
        if ( 0 <= i )
        {
            float rmin;
            if ( 6 > traffic )
            {
                rmin = 40.0f;
            }
            else
            {
                rmin = 110.0f;
            }
            spawn_traffic(i, P.x, P.z, rmin, 200.0f);
        }
    }
    int want_police = (int)P.wanted;
    if ( 4 < want_police )
    {
        want_police = 4;
    }
    if ( police < want_police )
    {
        spawn_police(P.x, P.z);
    }
    if ( N_PEDS > peds )
    {
        for ( int i = 0; MAX_PEDS > i; i++ )
        {
            if ( !s_ped[i].used )
            {
                spawn_ped(i, P.x, P.z);
                break;
            }
        }
    }
}

static void update_play(float dt, uint32_t btn, uint32_t pressed)
{
    /* the chord A+B (or a tap on the glass) is the map */
    bool both = (BTN_A | BTN_B) == (btn & (BTN_A | BTN_B));
    if ( (pressed & BTN_MAP) || (both && !P.chord_used) )
    {
        P.chord_used = true;
        P.state = ST_MAP;
        P.state_t = 0;
        return;
    }
    if ( !(btn & (BTN_A | BTN_B)) )
    {
        P.chord_used = false;
    }
    bool fire_held = (btn & BTN_A) && !both;
    bool use = (pressed & BTN_B) && !(btn & BTN_A);

    if ( use )
    {
        enter_or_leave();
    }

    if ( (0 <= P.car) && (0.0f != s_car[P.car].vy) && (CAR_WRECK != s_car[P.car].state) )
    {
        /* in the air: no grip, no steering, gravity */
        car_t *c = &s_car[P.car];
        c->vy -= 15.0f * dt;
        c->y += c->vy * dt;
        P.air_t += dt;
        float nx = c->x + sinf(c->yaw) * c->speed * dt, nz = c->z + cosf(c->yaw) * c->speed * dt;
        if ( (4.0f < c->y) || car_fits(nx, nz, c->yaw) )
        {
            c->x = nx;
            c->z = nz;
        }
        else
        {
            c->speed *= -0.2f;
        }
        if ( 0.0f >= c->y )
        {
            c->y = 0.0f;
            float hard = -c->vy;
            c->vy = 0.0f;
            P.shake = fminf(0.6f, hard * 0.04f);
            P.rumble = 0.8f;
            snd_play(SFX_CRASH);
            if ( 0.6f < P.air_t )
            {
                int bonus = (int)(P.air_t * 100.0f) * 2;
                char m[32];
                snprintf(m, sizeof m, "JUMP %.1fS +$%d", (double)P.air_t, bonus);
                P.money += bonus;
                say(m, rgb(120, 255, 240), 2.0f);
                snd_play(SFX_COIN);
            }
        }
        P.x = c->x;
        P.z = c->z;
        P.yaw = c->yaw;
    }
    else if ( 0 <= P.car )
    {
        car_t *c = &s_car[P.car];
        float right = 0, left = 0, gas = 0, brake = 0, vmax;
        if ( btn & BTN_RIGHT )
        {
            right = 1.0f;
        }
        if ( btn & BTN_LEFT )
        {
            left = 1.0f;
        }
        float steer = right - left;
        if ( btn & BTN_UP )
        {
            gas = 1.0f;
        }
        if ( btn & BTN_DOWN )
        {
            brake = 1.0f;
        }
        if ( KIND_SPORT == c->kind )
        {
            vmax = 46.0f;
        }
        else if ( KIND_POLICE == c->kind )
        {
            vmax = 40.0f;
        }
        else
        {
            vmax = 36.0f;
        }
        car_physics(c, gas, brake, steer, dt, vmax);
        /* up a ramp, and off its end */
        int ri;
        float prog = ramp_at(c->x, c->z, &ri);
        static const float RDX[4] = {1, 0, -1, 0}, RDZ[4] = {0, 1, 0, -1};
        if ( (0 <= prog) && (0.5f < sinf(c->yaw) * RDX[RAMPS[ri].dir] + cosf(c->yaw) * RDZ[RAMPS[ri].dir]) && (4.0f < c->speed) )
        {
            c->y = prog * RAMP_H;
            if ( (0.85f < prog) && (10.0f < c->speed) )
            {
                c->vy = c->speed * 0.42f;
                P.air_t = 0.0f;
            }
        }
        else if ( 0.0f == c->vy )
        {
            c->y = 0.0f;
        }
        P.x = c->x;
        P.z = c->z;
        P.yaw = c->yaw;
        if ( (CAR_WRECK == c->state) || (0 >= c->hp) )
        {
            wreck(c);
        }
    }
    else
    {
        float right = 0, left = 0, fwd = 0, back = 0;
        if ( btn & BTN_RIGHT )
        {
            right = 1.0f;
        }
        if ( btn & BTN_LEFT )
        {
            left = 1.0f;
        }
        float turn = right - left;
        P.yaw = wrap_pi(P.yaw + turn * 2.8f * dt);
        if ( btn & BTN_UP )
        {
            fwd = 1.0f;
        }
        if ( btn & BTN_DOWN )
        {
            back = 0.6f;
        }
        float mv = fwd - back;
        float sp = 5.2f * mv;
        float nx = P.x + sinf(P.yaw) * sp * dt, nz = P.z + cosf(P.yaw) * sp * dt;
        if ( world_free(nx, nz, 0.4f, true) )
        {
            P.x = nx;
            P.z = nz;
        }
        else if ( world_free(nx, P.z, 0.4f, true) )
        {
            P.x = nx;
        }
        else if ( world_free(P.x, nz, 0.4f, true) )
        {
            P.z = nz;
        }
        if ( 0 != mv )
        {
            P.walk_t += dt * 9.0f;
        }
        /* traffic that hits you hurts */
        for ( int i = 0; MAX_CARS > i; i++ )
        {
            car_t *c = &s_car[i];
            if ( !c->used || (5.0f > fabsf(c->speed)) )
            {
                continue;
            }
            if ( 2.0f * 2.0f > dist2(c->x, c->z, P.x, P.z) )
            {
                P.hp -= fabsf(c->speed) * 2.5f;
                c->speed *= 0.4f;
                P.shake = 0.4f;
                P.rumble = 0.9f;
            }
        }
    }

    P.fire_cd -= dt;
    if ( fire_held && (0 >= P.fire_cd) )
    {
        fire();
        P.fire_cd = 0.14f;
    }

    for ( int i = 0; MAX_CARS > i; i++ )
    {
        car_t *c = &s_car[i];
        if ( !c->used )
        {
            continue;
        }
        if ( CAR_TRAFFIC == c->state )
        {
            traffic_ai(c, dt);
        }
        else if ( CAR_POLICE == c->state )
        {
            police_ai(c, dt);
        }
        else if ( CAR_WRECK == c->state )
        {
            c->burn += dt;
            if ( 0 < c->y )
            {
                c->vy -= 20.0f * dt;
                c->y += c->vy * dt;
                if ( 0 >= c->y )
                {
                    c->y = 0;
                    c->vy = 0;
                    P.shake = fmaxf(P.shake, 0.2f);
                }
            }
        }
        else if ( CAR_PARKED == c->state )
        {
            c->speed = 0;
        }
    }
    collide_cars(dt);
    update_peds(dt);
    manage_population();

    /* the police */
    float nearest_cop = 1e9f;
    for ( int i = 0; MAX_CARS > i; i++ )
    {
        car_t *c = &s_car[i];
        if ( !c->used || (CAR_POLICE != c->state) )
        {
            continue;
        }
        float d = sqrtf(dist2(c->x, c->z, P.x, P.z));
        if ( d < nearest_cop )
        {
            nearest_cop = d;
        }
    }
    /* three stars bring the helicopter */
    if ( 3.0f <= P.wanted )
    {
        if ( !P.heli )
        {
            P.heli = true;
            P.hx = P.x - 120.0f;
            P.hz = P.z - 80.0f;
            P.hy = 45.0f;
        }
        float lead;   /* ahead of you, so it is in the shot */
        if ( 0 <= P.car )
        {
            lead = 40.0f;
        }
        else
        {
            lead = 28.0f;
        }
        float tx = P.x + sinf(P.yaw) * lead, tz = P.z + cosf(P.yaw) * lead;
        float dx = tx - P.hx, dz = tz - P.hz, d = sqrtf(dx * dx + dz * dz) + 1e-3f;
        float sp = fminf(d * 0.9f, 38.0f);
        P.hx += dx / d * sp * dt;
        P.hz += dz / d * sp * dt;
        P.hy = approach(P.hy, 11.5f, dt * 4.0f);
        if ( 6.0f < d )
        {
            P.hyaw = atan2f(dx, dz);
        }
        else
        {
            P.hyaw = atan2f(P.x - P.hx, P.z - P.hz);
        }
    }
    else if ( P.heli )
    {
        P.hy += dt * 8.0f;
        P.hx += dt * 20.0f;
        if ( 90.0f < P.hy )
        {
            P.heli = false;
        }
    }
    P.rotor += dt * 25.0f;
    if ( 1.0f <= P.wanted )
    {
        if ( 75.0f < nearest_cop )
        {
            P.calm += dt;
        }
        else
        {
            P.calm = 0;
        }
        if ( 10.0f < P.calm )
        {
            P.wanted -= dt * 0.25f;
        }
        /* caught: still, with a patrol car on top of you */
        float mysp, catch_r;
        if ( 0 <= P.car )
        {
            mysp = fabsf(s_car[P.car].speed);
            catch_r = 5.5f;
        }
        else
        {
            if ( P.prev_btn & (BTN_UP | BTN_DOWN) )
            {
                mysp = 5.0f;
            }
            else
            {
                mysp = 0.0f;
            }
            catch_r = 4.5f;
        }
        static float caught;
        if ( (catch_r > nearest_cop) && (2.0f > mysp) )
        {
            caught += dt;
        }
        else
        {
            caught = 0;
        }
        if ( 1.6f < caught )
        {
            caught = 0;
            P.state = ST_BUSTED;
            P.state_t = 0;
            snd_play(SFX_FAIL);
        }
    }
    else
    {
        P.wanted = approach(P.wanted, 0, dt * 0.02f);
    }

    /* the job */
    if ( 0 <= P.job_place )
    {
        const place_t *pl;
        world_places(&pl);
        P.job_t -= dt;
        if ( 9.0f * 9.0f > dist2(P.x, P.z, pl[P.job_place].x, pl[P.job_place].z) )
        {
            int reward = 300 + (int)(P.job_t * 25.0f);
            if ( 150 > reward )
            {
                reward = 150;
            }
            P.money += reward;
            P.jobs_done++;
            char m[24];
            snprintf(m, sizeof m, "+$%d", reward);
            say(m, rgb(120, 255, 140), 1.6f);
            P.rumble = 0.6f;
            snd_play(SFX_COIN);
            new_job();
        }
        else if ( 0 >= P.job_t )
        {
            say("TIME'S UP!", rgb(255, 120, 120), 1.5f);
            snd_play(SFX_FAIL);
            new_job();
        }
    }

    if ( 0 >= P.hp )
    {
        P.state = ST_KO;
        P.state_t = 0;
        snd_play(SFX_FAIL);
    }
}

/* ---- the camera ------------------------------------------------------------ */

static r3d_cam make_cam(float dt)
{
    float fx = sinf(P.yaw), fz = cosf(P.yaw);
    float sp, back, up, cy_off, ahead, look_h, follow;
    if ( 0 <= P.car )
    {
        sp = fabsf(s_car[P.car].speed);
        back = 7.5f + sp * 0.08f;
        up = 3.6f + sp * 0.02f;
        cy_off = s_car[P.car].y;
        ahead = 6.0f;
        look_h = 1.0f;
        follow = 9.0f;
    }
    else
    {
        sp = 0;
        back = 5.2f;
        up = 2.6f;
        cy_off = 0.0f;
        ahead = 3.0f;
        look_h = 1.3f;
        follow = 6.0f;
    }
    v3 want = v3m(P.x - fx * back, up + cy_off * 0.3f, P.z - fz * back);
    v3 look = v3m(P.x + fx * ahead, look_h + cy_off * 0.6f, P.z + fz * ahead);
    if ( !P.cam_init )
    {
        P.cam_pos = want;
        P.cam_look = look;
        P.cam_init = true;
    }
    float k = 1.0f - expf(-dt * follow);
    P.cam_pos = v3add(P.cam_pos, v3scale(v3sub(want, P.cam_pos), k));
    P.cam_look = v3add(P.cam_look, v3scale(v3sub(look, P.cam_look), 1.0f - expf(-dt * 10.0f)));
    r3d_cam c;
    c.pos = P.cam_pos;
    if ( 0 < P.shake )
    {
        c.pos.x += frand(-1, 1) * P.shake * 0.5f;
        c.pos.y += frand(-1, 1) * P.shake * 0.4f;
    }
    v3 f = v3sub(P.cam_look, c.pos);
    f = v3scale(f, 1.0f / sqrtf(v3dot(f, f)));
    v3 r = v3m(f.z, 0, -f.x);
    r = v3scale(r, 1.0f / sqrtf(v3dot(r, r)));
    c.fwd = f;
    c.right = r;
    c.up = v3cross(f, r);
    if ( 0 > c.up.y )
    {
        c.up = v3scale(c.up, -1);
    }
    float zoom = 0;
    if ( 0 <= P.car )
    {
        zoom = clampf(sp * 0.45f, 0, 14.0f);
    }
    c.focal = 178.0f - zoom;  /* wider when fast */
    c.cx = GW * 0.5f;
    c.cy = GH * 0.42f;
    c.far_dist = 300.0f;
    return c;
}

/* ---- drawing --------------------------------------------------------------- */

static void draw_car(const car_t *c, int idx)
{
    v3 base = v3m(c->x, 0.32f + c->y, c->z);
    if ( !r3d_visible(v3m(c->x, 1.0f + c->y, c->z), 3.0f) )
    {
        return;
    }
    const r3d_cam *cam = r3d_camera();
    float d = sqrtf(dist2(c->x, c->z, cam->pos.x, cam->pos.z));
    float fx = sinf(c->yaw), fz = cosf(c->yaw), rx = cosf(c->yaw), rz = -sinf(c->yaw);
    float L, W = 2.0f;
    if ( KIND_VAN == c->kind )
    {
        L = 4.8f;
    }
    else
    {
        L = 4.4f;
    }
    float bodyh;
    if ( KIND_SPORT == c->kind )
    {
        bodyh = 0.62f;
    }
    else
    {
        bodyh = 0.82f;
    }
    uint16_t body = c->color;
    uint16_t glass = rgb(40, 60, 90);
    if ( CAR_WRECK == c->state )
    {
        glass = rgb(20, 20, 20);
    }
    /* its shadow, a little bigger than the car and offset away from the sun */
    if ( 0.05f > c->y )
    {
        float sx = -r3d_sun.x * 0.5f, sz = -r3d_sun.z * 0.5f;
        float hw = W * 0.62f, hl = L * 0.56f;
        v3 q[4];
        float lxs[4] = {-hw, hw, hw, -hw}, lzs[4] = {-hl, -hl, hl, hl};
        for ( int k = 0; 4 > k; k++ )
        {
            q[k] = v3m(c->x + sx + lxs[k] * rx + lzs[k] * fx, 0.03f, c->z + sz + lxs[k] * rz + lzs[k] * fz);
        }
        v3 qq[4] = {q[0], q[3], q[2], q[1]};
        r3d_poly_flat(qq, 4, rgb(30, 26, 38));
        r3d_poly_flat(q, 4, rgb(30, 26, 38));
    }
    /* the body, and its floor when it is in the air */
    r3d_box(base, W, bodyh, L, c->yaw, body, body, 0, 0);
    if ( 0.3f < c->y )
    {
        float hw = W * 0.5f, hl = L * 0.5f;
        v3 q[4];
        float lxs[4] = {-hw, hw, hw, -hw}, lzs[4] = {-hl, -hl, hl, hl};
        for ( int k = 0; 4 > k; k++ )
        {
            q[k] = v3m(c->x + lxs[k] * rx + lzs[k] * fx, base.y, c->z + lxs[k] * rz + lzs[k] * fz);
        }
        v3 qq[4] = {q[0], q[3], q[2], q[1]};
        r3d_poly_flat(q, 4, rgb(35, 32, 40));
        r3d_poly_flat(qq, 4, rgb(35, 32, 40));
    }
    /* the cabin */
    float cab_l, cab_off, cab_h;
    if ( KIND_VAN == c->kind )
    {
        cab_l = 3.4f;
        cab_off = -0.5f;
        cab_h = 1.0f;
    }
    else if ( KIND_SPORT == c->kind )
    {
        cab_l = 1.8f;
        cab_off = -0.35f;
        cab_h = 0.5f;
    }
    else
    {
        cab_l = 2.3f;
        cab_off = -0.35f;
        cab_h = 0.68f;
    }
    v3 cab = v3m(c->x + fx * cab_off, 0.32f + bodyh + c->y, c->z + fz * cab_off);
    if ( KIND_VAN == c->kind )
    {
        r3d_box(cab, W * 0.9f, cab_h, cab_l, c->yaw, glass, body, 0, 0);
    }
    else
    {
        float top;
        if ( KIND_SPORT == c->kind )
        {
            top = 0.5f;
        }
        else
        {
            top = 0.62f;
        }
        uint16_t roof;
        if ( KIND_POLICE == c->kind )
        {
            roof = rgb(250, 250, 250);
        }
        else
        {
            roof = body;
        }
        r3d_cabin(cab, W * 0.86f, cab_h, cab_l, cab_l * top, -0.1f, c->yaw,
                   glass, roof);
    }
    if ( 120.0f > d )
    {
        /* wheels */
        for ( int k = 0; 4 > k; k++ )
        {
            float sx = -0.92f, sz = -1.35f;
            if ( k & 1 )
            {
                sx = 0.92f;
            }
            if ( k & 2 )
            {
                sz = 1.35f;
            }
            v3 w = v3m(c->x + rx * sx + fx * sz, c->y, c->z + rz * sx + fz * sz);
            r3d_box(w, 0.36f, 0.62f, 0.68f, c->yaw, rgb(25, 25, 30), rgb(25, 25, 30), 0, 0);
        }
        /* lights */
        for ( int k = 0; 2 > k; k++ )
        {
            float sx;
            if ( 0 != k )
            {
                sx = 0.62f;
            }
            else
            {
                sx = -0.62f;
            }
            v3 hl = v3m(c->x + rx * sx + fx * (L * 0.5f), 0.62f + c->y, c->z + rz * sx + fz * (L * 0.5f));
            r3d_box(hl, 0.42f, 0.2f, 0.08f, c->yaw, rgb(255, 250, 200), rgb(255, 250, 200), 0, 0);
            v3 tl = v3m(c->x + rx * sx - fx * (L * 0.5f + 0.04f), 0.66f + c->y, c->z + rz * sx - fz * (L * 0.5f + 0.04f));
            uint16_t tc;
            float tw, th;
            if ( c->braking )
            {
                tc = rgb(255, 90, 90);
                tw = 0.5f;
                th = 0.26f;
            }
            else
            {
                tc = rgb(200, 20, 40);
                tw = 0.42f;
                th = 0.18f;
            }
            r3d_box(tl, tw, th, 0.08f, c->yaw, tc, tc, 0, 0);
        }
    }
    if ( (KIND_POLICE == c->kind) && (CAR_WRECK != c->state) )
    {
        /* the doors are dark, the roof carries the lights */
        v3 bar = v3m(cab.x, cab.y + cab_h, cab.z);
        bool ph = ((int)(P.siren_t * 6.0f + idx)) & 1;
        uint16_t red_a, red_b, blue_a, blue_b;
        if ( ph )
        {
            red_a = rgb(255, 40, 40);
            red_b = rgb(255, 60, 60);
            blue_a = rgb(0, 0, 90);
            blue_b = rgb(0, 0, 90);
        }
        else
        {
            red_a = rgb(90, 0, 0);
            red_b = rgb(90, 0, 0);
            blue_a = rgb(40, 80, 255);
            blue_b = rgb(60, 100, 255);
        }
        r3d_box(v3add(bar, v3m(rx * 0.35f, 0, rz * 0.35f)), 0.6f, 0.22f, 0.4f, c->yaw, red_a, red_b, 0, 0);
        r3d_box(v3add(bar, v3m(-rx * 0.35f, 0, -rz * 0.35f)), 0.6f, 0.22f, 0.4f, c->yaw, blue_a, blue_b, 0, 0);
        v3 stripe = v3m(c->x, 0.5f + c->y, c->z);
        r3d_box(stripe, W + 0.04f, 0.3f, L * 0.55f, c->yaw, rgb(20, 30, 70), rgb(20, 30, 70), 0, 0);
    }
    if ( (KIND_TAXI == c->kind) && (CAR_WRECK != c->state) )
    {
        v3 sign = v3m(cab.x, cab.y + cab_h, cab.z);
        r3d_box(sign, 0.8f, 0.28f, 0.35f, c->yaw, rgb(30, 30, 30), rgb(255, 240, 120), 3, rgb(255, 240, 120));
    }
    if ( (0.05f < c->smoke) && (90.0f > d) )
    {
        /* tyre smoke from the back wheels */
        for ( int k = 0; 3 > k; k++ )
        {
            float tt = fmodf(P.time * 2.5f + k * 0.33f + idx * 0.1f, 1.0f);
            float sx;
            if ( k & 1 )
            {
                sx = 1.0f;
            }
            else
            {
                sx = -1.0f;
            }
            v3 s = v3m(c->x - fx * (1.6f + tt * 2.5f) + rx * sx, 0.4f + tt * 1.2f, c->z - fz * (1.6f + tt * 2.5f) + rz * sx);
            int g = 235 - (int)(tt * 40);
            int gb = g + 8;
            if ( 255 < gb )
            {
                gb = 255;
            }
            r3d_billboard(s, (0.8f + tt * 1.8f) * c->smoke, (0.8f + tt * 1.8f) * c->smoke, rgb(g, g, gb));
        }
    }
    if ( (CAR_WRECK == c->state) && (30.0f > c->burn) )
    {
        /* smoke and fire */
        for ( int k = 0; 4 > k; k++ )
        {
            float tt = fmodf(c->burn * 0.8f + k * 0.25f, 1.0f);
            v3 s = v3m(c->x + sinf(k * 2.1f) * 0.6f, 1.6f + tt * 6.0f, c->z + cosf(k * 1.7f) * 0.6f);
            uint16_t col;
            if ( 0.25f > tt )
            {
                col = rgb(255, 160, 40);
            }
            else
            {
                col = rgb(70 - (int)(tt * 30), 65 - (int)(tt * 30), 70 - (int)(tt * 30));
            }
            r3d_billboard(s, 1.2f + tt * 2.5f, 1.2f + tt * 2.5f, col);
        }
    }
}

static void draw_person(float x, float z, float yaw, float phase, bool moving, uint16_t shirt, uint16_t pants,
                        uint16_t skin, bool dead, float dead_t, bool gun, float y)
{
    if ( !r3d_visible(v3m(x, 0.9f, z), 1.2f) )
    {
        return;
    }
    if ( dead )
    {
        float fx = sinf(yaw), fz = cosf(yaw);
        r3d_box(v3m(x - fx * 0.2f, y, z - fz * 0.2f), 0.55f, 0.32f, 1.1f, yaw, shirt, shirt, 0, 0);
        r3d_box(v3m(x + fx * 0.65f, y, z + fz * 0.65f), 0.3f, 0.28f, 0.3f, yaw, skin, skin, 0, 0);
        r3d_box(v3m(x - fx * 1.05f, y, z - fz * 1.05f), 0.5f, 0.26f, 0.7f, yaw, pants, pants, 0, 0);
        (void)dead_t;
        return;
    }
    float fx = sinf(yaw), fz = cosf(yaw), rx = cosf(yaw), rz = -sinf(yaw);
    float sw = 0;
    if ( moving )
    {
        sw = sinf(phase) * 0.28f;
    }
    {
        float sx = -r3d_sun.x * 0.4f, sz = -r3d_sun.z * 0.4f;
        v3 q[4] = {v3m(x + sx - 0.4f, 0.03f, z + sz - 0.4f), v3m(x + sx - 0.4f, 0.03f, z + sz + 0.4f),
                   v3m(x + sx + 0.4f, 0.03f, z + sz + 0.4f), v3m(x + sx + 0.4f, 0.03f, z + sz - 0.4f)};
        r3d_poly_flat(q, 4, rgb(30, 26, 38));
    }
    for ( int k = 0; 2 > k; k++ )
    {
        float s, o;
        if ( 0 != k )
        {
            s = 0.14f;
            o = sw;
        }
        else
        {
            s = -0.14f;
            o = -sw;
        }
        r3d_box(v3m(x + rx * s + fx * o, 0, z + rz * s + fz * o), 0.22f, 0.86f, 0.26f, yaw, pants, pants, 0, 0);
    }
    r3d_box(v3m(x, 0.86f, z), 0.58f, 0.66f, 0.34f, yaw, shirt, shirt, 0, 0);
    r3d_box(v3m(x, 1.52f, z), 0.3f, 0.3f, 0.3f, yaw, skin, rgb(60, 40, 30), 0, 0);
    if ( gun )
    {
        r3d_box(v3m(x + rx * 0.3f + fx * 0.45f, 1.18f, z + rz * 0.3f + fz * 0.45f), 0.12f, 0.12f, 0.55f, yaw,
                rgb(30, 30, 35), rgb(30, 30, 35), 0, 0);
    }
}

static void draw_marker(void)
{
    if ( 0 > P.job_place )
    {
        return;
    }
    const place_t *pl;
    world_places(&pl);
    float x = pl[P.job_place].x, z = pl[P.job_place].z;
    float bob = 0.5f + 0.5f * sinf(P.time * 4.0f);
    /* a beam of light you can see over the roofs */
    r3d_box(v3m(x, 0, z), 1.2f, 60.0f, 1.2f, P.time, rgb(255, 230, 80), rgb(255, 230, 80), 0, 0);
    r3d_box(v3m(x, 0.02f, z), 7.0f, 0.15f + bob * 0.2f, 7.0f, 0.0f, rgb(255, 200, 60), rgb(255, 230, 120), 0, 0);
}

/* ---- the HUD --------------------------------------------------------------- */

static void star(int cx, int cy, int r, uint16_t c)
{
    /* a small five-point star made of two triangles' worth of lines */
    for ( int i = 0; 5 > i; i++ )
    {
        float a0 = -PI / 2 + i * 2 * PI / 5, a1 = a0 + 4 * PI / 5;
        for ( int k = 0; 2 > k; k++ )
        {
            r2d_line(cx + (int)(cosf(a0) * r) + k, cy + (int)(sinf(a0) * r), cx + (int)(cosf(a1) * r) + k, cy + (int)(sinf(a1) * r), c);
        }
    }
    r2d_disc(cx, cy, r / 2, c);
}

static void draw_minimap(void)
{
    const int CX = 60, CY = 268, R = 44;
    const float scale = 2.4f;      /* metres per pixel */
    const uint16_t *ov = world_overview();
    float cs = cosf(P.yaw), sn = sinf(P.yaw);
    for ( int dy = -R; R >= dy; dy++ )
    {
        int w = (int)sqrtf((float)(R * R - dy * dy));
        uint16_t *row = r3d_fb + (CY + dy) * GW;
        for ( int dx = -w; dx <= w; dx++ )
        {
            /* screen right is the player's right, screen up is ahead */
            float lx = dx * scale, lf = -dy * scale;
            float wx = P.x + lx * cs + lf * sn;
            float wz = P.z - lx * sn + lf * cs;
            int cx = (int)(wx / CELL_M), cz = (int)(wz / CELL_M);
            uint16_t c;
            if ( (MAP_N > (unsigned)cx) && (MAP_N > (unsigned)cz) )
            {
                c = ov[cz * MAP_N + cx];
            }
            else
            {
                c = rgb(30, 70, 120);
            }
            row[CX + dx] = rgb_mix(c, 0, 6);
        }
    }
    /* the ring */
    for ( int a = 0; 180 > a; a++ )
    {
        float t = a * 2 * PI / 180;
        int x = CX + (int)(cosf(t) * (R + 1)), y = CY + (int)(sinf(t) * (R + 1));
        r2d_rect(x, y, 2, 2, rgb(20, 20, 30));
    }
    /* blips */
    for ( int i = 0; MAX_CARS > i; i++ )
    {
        car_t *c = &s_car[i];
        if ( !c->used || (CAR_POLICE != c->state) )
        {
            continue;
        }
        float dx = c->x - P.x, dz = c->z - P.z;
        float lx = (dx * cs - dz * sn) / scale, lf = (dx * sn + dz * cs) / scale;
        if ( (R - 3) * (R - 3) < lx * lx + lf * lf )
        {
            continue;
        }
        bool ph = ((int)(P.siren_t * 6.0f)) & 1;
        uint16_t blip;
        if ( ph )
        {
            blip = rgb(255, 50, 50);
        }
        else
        {
            blip = rgb(60, 110, 255);
        }
        r2d_rect(CX + (int)lx - 2, CY - (int)lf - 2, 5, 5, blip);
    }
    if ( 0 <= P.job_place )
    {
        const place_t *pl;
        world_places(&pl);
        float dx = pl[P.job_place].x - P.x, dz = pl[P.job_place].z - P.z;
        float lx = (dx * cs - dz * sn) / scale, lf = (dx * sn + dz * cs) / scale;
        float l = sqrtf(lx * lx + lf * lf);
        if ( R - 5 < l )
        {
            lx *= (R - 5) / l;
            lf *= (R - 5) / l;
        }
        r2d_disc(CX + (int)lx, CY - (int)lf, 4, rgb(255, 220, 60));
        r2d_disc(CX + (int)lx, CY - (int)lf, 2, rgb(120, 70, 0));
    }
    /* you */
    r2d_line(CX, CY - 6, CX - 4, CY + 4, rgb(255, 255, 255));
    r2d_line(CX, CY - 6, CX + 4, CY + 4, rgb(255, 255, 255));
    r2d_line(CX - 4, CY + 4, CX + 4, CY + 4, rgb(255, 255, 255));
    r2d_rect(CX - 1, CY - 2, 3, 5, rgb(255, 255, 255));
}

static void draw_hud(void)
{
    char buf[40];
    /* money rolls up */
    if ( P.shown_money < P.money )
    {
        P.shown_money += 1 + (P.money - P.shown_money) / 8;
    }
    if ( P.shown_money > P.money )
    {
        P.shown_money = P.money;
    }
    snprintf(buf, sizeof buf, "$%07d", P.shown_money);
    int w = r2d_text_w(2, buf);
    r2d_text_shadow((GW - w) / 2, 10, 2, rgb(120, 255, 150), buf);
    /* wanted */
    int stars = (int)P.wanted;
    for ( int i = 0; 5 > i; i++ )
    {
        int x = GW / 2 - 40 + i * 20;
        uint16_t c;
        if ( i < stars )
        {
            if ( (((int)(P.time * 5)) & 1) && (4 < P.calm) )
            {
                c = rgb(255, 255, 255);
            }
            else
            {
                c = rgb(255, 210, 60);
            }
        }
        else
        {
            c = rgb(60, 60, 70);
        }
        star(x, 36, 6, c);
    }
    /* health */
    int hw = (int)(clampf(P.hp, 0, 100) * 0.8f);
    r2d_rect(GW / 2 - 41, 47, 82, 6, rgb(20, 20, 20));
    uint16_t hc;
    if ( 30 < P.hp )
    {
        hc = rgb(255, 80, 120);
    }
    else
    {
        hc = rgb(255, 40, 40);
    }
    r2d_rect(GW / 2 - 40, 48, hw, 4, hc);
    /* the job */
    if ( 0 <= P.job_place )
    {
        const place_t *pl;
        world_places(&pl);
        int d = (int)sqrtf(dist2(P.x, P.z, pl[P.job_place].x, pl[P.job_place].z));
        snprintf(buf, sizeof buf, "GO TO %s", pl[P.job_place].name);
        r2d_text_shadow((GW - r2d_text_w(1, buf)) / 2, 60, 1, rgb(255, 230, 100), buf);
        snprintf(buf, sizeof buf, "%dM  %d:%02d", d, (int)P.job_t / 60, (int)P.job_t % 60);
        uint16_t tc;
        if ( 10 > P.job_t )
        {
            tc = rgb(255, 100, 100);
        }
        else
        {
            tc = rgb(255, 255, 255);
        }
        r2d_text_shadow((GW - r2d_text_w(1, buf)) / 2, 70, 1, tc, buf);
    }
    draw_minimap();
    /* speed */
    if ( 0 <= P.car )
    {
        int kmh = (int)(fabsf(s_car[P.car].speed) * 3.6f);
        snprintf(buf, sizeof buf, "%d", kmh);
        r2d_text_shadow(176, 268, 3, rgb(255, 255, 255), buf);
        r2d_text_shadow(178, 292, 1, rgb(200, 200, 220), "KM/H");
    }
    /* a message */
    if ( 0 < P.msg_t )
    {
        int sc = 2;
        int mw = r2d_text_w(sc, P.msg);
        r2d_text_shadow((GW - mw) / 2, 112, sc, P.msg_c, P.msg);
    }
    /* a patrol car close behind: the edges of the glass flash red and blue */
    {
        float cop = 1e9f;
        for ( int i = 0; MAX_CARS > i; i++ )
        {
            if ( s_car[i].used && (CAR_POLICE == s_car[i].state) )
            {
                float d = sqrtf(dist2(s_car[i].x, s_car[i].z, P.x, P.z));
                if ( d < cop )
                {
                    cop = d;
                }
            }
        }
        if ( 45.0f > cop )
        {
            bool ph = ((int)(P.siren_t * 5.0f)) & 1;
            int t = (int)(10.0f * (1.0f - cop / 45.0f)) + 2;
            uint16_t c1, c2;
            if ( ph )
            {
                c1 = rgb(255, 30, 40);
                c2 = rgb(30, 80, 255);
            }
            else
            {
                c1 = rgb(30, 80, 255);
                c2 = rgb(255, 30, 40);
            }
            for ( int k = 0; 10 > k; k++ )
            {
                r2d_rect_blend(0, 90 + k * 3, 4 + (9 - k), 3, c1, t);
                r2d_rect_blend(GW - 4 - (9 - k), 90 + k * 3, 4 + (9 - k), 3, c2, t);
            }
            r2d_rect_blend(0, 120, 6, 110, c1, t);
            r2d_rect_blend(GW - 6, 120, 6, 110, c2, t);
        }
    }
    /* the muzzle flash */
    if ( 0 < P.flash )
    {
        r2d_rect_blend(0, 0, GW, GH, rgb(255, 240, 200), 3);
    }
}

static void draw_map(void)
{
    const uint16_t *ov = world_overview();
    /* the map fills the glass's middle */
    const int ox = 8, oy = 40, S = 256;
    r2d_rect(0, 0, GW, GH, rgb(14, 12, 24));
    for ( int y = 0; S > y; y++ )
    {
        uint16_t *row = r3d_fb + (oy + y) * GW + ox;
        int cz = MAP_N - 1 - y;          /* north up */
        for ( int x = 0; S > x; x++ )
        {
            row[x] = ov[cz * MAP_N + x];
        }
    }
    const place_t *pl;
    int n = world_places(&pl);
    int lrx[16], lry[16], lrw[16], nl = 0;
    for ( int i = 0; (i < n) && (16 > i); i++ )
    {
        int x = ox + (int)(pl[i].x / CELL_M), y = oy + MAP_N - 1 - (int)(pl[i].z / CELL_M);
        bool job = i == P.job_place;
        if ( job )
        {
            r2d_disc(x, y, 4, rgb(255, 220, 60));
        }
        else
        {
            r2d_disc(x, y, 2, rgb(255, 255, 255));
        }
        int lw = r2d_text_w(1, pl[i].name);
        int lx;
        if ( GW - 4 < x + 5 + lw )
        {
            lx = x - 5 - lw;
        }
        else
        {
            lx = x + 5;
        }
        int ly = y - 3;
        /* names that would overlap step down a line */
        for ( int tries = 0; 6 > tries; tries++ )
        {
            bool hit = false;
            for ( int k = 0; k < nl; k++ )
            {
                if ( (lrx[k] + lrw[k] + 2 > lx) && (lx + lw + 2 > lrx[k]) && (lry[k] + 9 > ly) && (ly + 9 > lry[k]) )
                {
                    hit = true;
                    break;
                }
            }
            if ( !hit )
            {
                break;
            }
            ly += 9;
        }
        if ( y - 3 != ly )
        {
            int to;
            if ( lx > x )
            {
                to = 0;
            }
            else
            {
                to = lw;
            }
            r2d_line(x, y, lx + to, ly + 3, rgb(200, 200, 210));
        }
        uint16_t nc;
        if ( job )
        {
            nc = rgb(255, 220, 60);
        }
        else
        {
            nc = rgb(230, 230, 240);
        }
        r2d_text_shadow(lx, ly, 1, nc, pl[i].name);
        lrx[nl] = lx;
        lry[nl] = ly;
        lrw[nl] = lw;
        nl++;
    }
    for ( int i = 0; MAX_CARS > i; i++ )
    {
        car_t *c = &s_car[i];
        if ( !c->used || (CAR_POLICE != c->state) )
        {
            continue;
        }
        r2d_rect(ox + (int)(c->x / CELL_M) - 1, oy + MAP_N - 1 - (int)(c->z / CELL_M) - 1, 3, 3, rgb(255, 50, 50));
    }
    for ( int i = 0; N_RAMPS > i; i++ )
    {
        r2d_rect(ox + (int)(RAMPS[i].x / CELL_M) - 1, oy + MAP_N - 1 - (int)(RAMPS[i].z / CELL_M) - 1, 3, 3, rgb(120, 255, 240));
    }
    int px = ox + (int)(P.x / CELL_M), py = oy + MAP_N - 1 - (int)(P.z / CELL_M);
    bool blink = ((int)(P.time * 4)) & 1;
    uint16_t you;
    if ( blink )
    {
        you = rgb(255, 255, 255);
    }
    else
    {
        you = rgb(255, 60, 150);
    }
    r2d_disc(px, py, 4, you);
    r2d_line(px, py, px + (int)(sinf(P.yaw) * 10), py - (int)(cosf(P.yaw) * 10), rgb(255, 255, 255));
    r2d_text_shadow((GW - r2d_text_w(2, world_name())) / 2, 14, 2, rgb(255, 90, 190), world_name());
    r2d_text_shadow((GW - r2d_text_w(1, "RED: BACK")) / 2, 304, 1, rgb(200, 200, 220), "RED: BACK");
}

static void draw_world(void)
{
    world_set_time(P.time);
    world_draw_sky(P.time);
    world_draw_ground(P.time);
    world_draw_objects();
    draw_marker();
    for ( int i = 0; N_RAMPS > i; i++ )
    {
        static const float DX[4] = {1, 0, -1, 0}, DZ[4] = {0, 1, 0, -1};
        const ramp_t *r = &RAMPS[i];
        float dx = DX[r->dir], dz = DZ[r->dir], sx = dz * RAMP_W * 0.5f, sz = -dx * RAMP_W * 0.5f;
        v3 a0 = v3m(r->x - sx, 0.02f, r->z - sz), a1 = v3m(r->x + sx, 0.02f, r->z + sz);
        v3 b0 = v3m(r->x - sx + dx * RAMP_LEN, RAMP_H, r->z - sz + dz * RAMP_LEN);
        v3 b1 = v3m(r->x + sx + dx * RAMP_LEN, RAMP_H, r->z + sz + dz * RAMP_LEN);
        v3 g0 = v3m(b0.x, 0, b0.z), g1 = v3m(b1.x, 0, b1.z);
        if ( !r3d_visible(v3m(r->x + dx * 3.5f, 1, r->z + dz * 3.5f), 6.0f) )
        {
            continue;
        }
        r3d_vtx slope[4] = {{a0, 0, 0}, {b0, 0, RAMP_LEN}, {b1, RAMP_W, RAMP_LEN}, {a1, RAMP_W, 0}};
        r3d_vtx slope2[4] = {slope[3], slope[2], slope[1], slope[0]};
        r3d_poly(slope, 4, rgb(255, 210, 40), 2, rgb(40, 30, 40), true);
        r3d_poly(slope2, 4, rgb(255, 210, 40), 2, rgb(40, 30, 40), true);
        v3 back[4] = {g0, b0, b1, g1};
        v3 back2[4] = {g1, b1, b0, g0};
        r3d_poly_flat(back, 4, rgb(90, 80, 90));
        r3d_poly_flat(back2, 4, rgb(90, 80, 90));
        v3 s0[3] = {a0, b0, g0}, s0b[3] = {g0, b0, a0};
        v3 s1[3] = {a1, g1, b1}, s1b[3] = {b1, g1, a1};
        r3d_poly_flat(s0, 3, rgb(120, 100, 110));
        r3d_poly_flat(s0b, 3, rgb(120, 100, 110));
        r3d_poly_flat(s1, 3, rgb(120, 100, 110));
        r3d_poly_flat(s1b, 3, rgb(120, 100, 110));
    }
    for ( int i = 0; MAX_CARS > i; i++ )
    {
        if ( s_car[i].used )
        {
            draw_car(&s_car[i], i);
        }
    }
    if ( P.heli )
    {
        v3 h = v3m(P.hx, P.hy, P.hz);
        float fx = sinf(P.hyaw), fz = cosf(P.hyaw);
        /* the searchlight on the ground */
        if ( 60.0f > P.hy )
        {
            v3 g[8], g2[8];
            float lx = P.x + (P.hx - P.x) * 0.3f, lz = P.z + (P.hz - P.z) * 0.3f;   /* it lights you up */
            for ( int k = 0; 8 > k; k++ )
            {
                float a = k * PI / 4;
                g[k] = v3m(lx + cosf(a) * 5.5f, 0.05f, lz + sinf(a) * 5.5f);
            }
            for ( int k = 0; 8 > k; k++ )
            {
                g2[k] = g[7 - k];
            }
            r3d_poly_flat(g, 6, rgb(255, 250, 190));
            r3d_poly_flat(g2, 6, rgb(255, 250, 190));
            v3 g3[4] = {g[0], g[5], g[6], g[7]}, g4[4] = {g[7], g[6], g[5], g[0]};
            r3d_poly_flat(g3, 4, rgb(255, 250, 190));
            r3d_poly_flat(g4, 4, rgb(255, 250, 190));
        }
        if ( r3d_visible(h, 8.0f) )
        {
            r3d_box(v3m(h.x, h.y - 1.0f, h.z), 2.2f, 1.9f, 4.0f, P.hyaw, rgb(30, 40, 90), rgb(240, 240, 245), 0, 0);
            r3d_box(v3m(h.x - fx * 4.0f, h.y - 0.3f, h.z - fz * 4.0f), 0.4f, 0.5f, 4.5f, P.hyaw, rgb(30, 40, 90), rgb(30, 40, 90), 0, 0);
            r3d_box(v3m(h.x + fx * 1.4f, h.y - 0.7f, h.z + fz * 1.4f), 1.8f, 1.0f, 1.4f, P.hyaw, rgb(120, 200, 255), rgb(30, 40, 90), 0, 0);
            for ( int k = 0; 2 > k; k++ )
            {
                float a = P.rotor + k * PI * 0.5f;
                float bx = cosf(a) * 6.5f, bz = sinf(a) * 6.5f, wx = -sinf(a) * 0.3f, wz = cosf(a) * 0.3f;
                v3 b1[4] = {v3m(h.x - bx - wx, h.y + 1.05f, h.z - bz - wz), v3m(h.x + bx - wx, h.y + 1.05f, h.z + bz - wz),
                            v3m(h.x + bx + wx, h.y + 1.05f, h.z + bz + wz), v3m(h.x - bx + wx, h.y + 1.05f, h.z - bz + wz)};
                v3 b2[4] = {b1[3], b1[2], b1[1], b1[0]};
                r3d_poly_flat(b1, 4, rgb(20, 20, 25));
                r3d_poly_flat(b2, 4, rgb(20, 20, 25));
            }
            bool ph = ((int)(P.siren_t * 4.0f)) & 1;
            uint16_t beacon;
            if ( ph )
            {
                beacon = rgb(255, 60, 60);
            }
            else
            {
                beacon = rgb(60, 120, 255);
            }
            r3d_billboard(v3m(h.x, h.y - 2.1f, h.z), 0.5f, 0.5f, beacon);
        }
    }
    for ( int i = 0; MAX_PEDS > i; i++ )
    {
        ped_t *p = &s_ped[i];
        if ( !p->used )
        {
            continue;
        }
        draw_person(p->x, p->z, p->yaw, p->t, PED_DEAD != p->state, p->shirt, p->pants, p->skin,
                    PED_DEAD == p->state, p->t, false, p->y);
    }
    if ( 0 > P.car )
    {
        draw_person(P.x, P.z, P.yaw, P.walk_t, true, rgb(250, 250, 250), rgb(40, 60, 120), rgb(220, 170, 130),
                    false, 0, true, 0);
    }
    for ( int i = 0; MAX_BLASTS > i; i++ )
    {
        blast_t *b = &s_bl[i];
        if ( !b->used )
        {
            continue;
        }
        float r = 2.0f + b->t * 14.0f;
        uint16_t c;
        if ( 0.15f > b->t )
        {
            c = rgb(255, 255, 200);
        }
        else if ( 0.35f > b->t )
        {
            c = rgb(255, 170, 40);
        }
        else
        {
            c = rgb(200, 70, 30);
        }
        r3d_billboard(v3m(b->x, 1.5f + b->t * 3.0f, b->z), r, r, c);
    }
    for ( int i = 0; MAX_SPARKS > i; i++ )
    {
        if ( !s_sp[i].used )
        {
            continue;
        }
        float k = 1.0f - s_sp[i].t / 0.18f;
        r3d_billboard(s_sp[i].p, 0.9f * k + 0.2f, 0.9f * k + 0.2f, rgb(255, 230, 120));
    }
    if ( 0 < P.flash )
    {
        float fx = sinf(P.yaw), fz = cosf(P.yaw), rx = cosf(P.yaw), rz = -sinf(P.yaw);
        v3 m;
        if ( 0 <= P.car )
        {
            m = v3m(P.x + fx * 2.7f, 1.2f, P.z + fz * 2.7f);
        }
        else
        {
            m = v3m(P.x + rx * 0.3f + fx * 0.85f, 1.2f, P.z + rz * 0.3f + fz * 0.85f);
        }
        r3d_billboard(m, 0.7f, 0.7f, rgb(255, 250, 200));
    }
    for ( int i = 0; MAX_TRACERS > i; i++ )
    {
        if ( s_tr[i].used )
        {
            r3d_line3(s_tr[i].a, s_tr[i].b, s_tr[i].c);
        }
    }
}

static void draw_title(void)
{
    /* the city turns slowly behind the game's name */
    const char *t1 = "KODECITY";
    bool ab = MAP_ALBACETE == s_opt_map;
    int sc;
    uint16_t c_back, c_front;     /* the name's shadow and face; c_front also marks the chosen item */
    const char *sub, *map_name;
    if ( ab )
    {
        sc = 5;
        c_back = rgb(255, 200, 40);
        c_front = rgb(200, 30, 60);
        sub = "LA MANCHA IN YOUR POCKET";
        map_name = "ALBACETE";
    }
    else
    {
        sc = 5;
        c_back = rgb(40, 220, 255);
        c_front = rgb(255, 60, 170);
        sub = "A CITY IN YOUR POCKET";
        map_name = "NEON BAY";
    }
    int w = r2d_text_w(sc, t1);
    r2d_text((GW - w) / 2 + 3, 27, sc, c_back, t1);
    r2d_text((GW - w) / 2, 24, sc, c_front, t1);
    r2d_text_shadow((GW - r2d_text_w(1, sub)) / 2, 66, 1, rgb(255, 240, 220), sub);
    r2d_rect_blend(26, 196, GW - 52, 118, rgb(10, 6, 30), 22);
    const char *items[3];
    char m1[32], m2[32];
    snprintf(m1, sizeof m1, "MAP: %s", map_name);
    const char *gfx;
    if ( STYLE_REAL == s_opt_style )
    {
        gfx = "REAL";
    }
    else
    {
        gfx = "NEON";
    }
    snprintf(m2, sizeof m2, "GRAPHICS: %s", gfx);
    items[0] = "PLAY"; items[1] = m1; items[2] = m2;
    for ( int i = 0; 3 > i; i++ )
    {
        int y = 206 + i * 22;
        bool sel = i == s_menu;
        int tw = r2d_text_w(2, items[i]);
        if ( sel )
        {
            r2d_rect_blend((GW - tw) / 2 - 10, y - 4, tw + 20, 22, c_front, 12);
        }
        uint16_t ic;
        if ( sel )
        {
            ic = rgb(255, 255, 255);
        }
        else
        {
            ic = rgb(180, 175, 210);
        }
        r2d_text_shadow((GW - tw) / 2, y, 2, ic, items[i]);
    }
    const char *help[] = {"UP/DOWN: CHOOSE  BLUE: CHANGE", "BLUE: SHOOT   RED: GET IN/OUT", "BLUE+RED OR A TAP: THE MAP"};
    for ( int i = 0; 3 > i; i++ )
    {
        r2d_text_shadow((GW - r2d_text_w(1, help[i])) / 2, 276 + i * 11, 1, rgb(230, 220, 255), help[i]);
    }
}

/* ---- the frame ------------------------------------------------------------ */

void game_frame(uint16_t *fb, float dt, uint32_t btn)
{
    if ( 0.06f < dt )
    {
        dt = 0.06f;
    }
    if ( (ST_PLAY == P.state) && (0 <= P.car) && (0.0f != s_car[P.car].vy) && (0.8f < s_car[P.car].y) )
    {
        dt *= 0.5f;   /* the stunt in slow motion */
    }
    uint32_t pressed = btn & ~P.prev_btn;
    P.prev_btn = btn;
    P.time += dt;
    P.state_t += dt;
    P.siren_t += dt;
    P.rumble = approach(P.rumble, 0, dt * 3.0f);
    P.shake = approach(P.shake, 0, dt * 1.5f);
    if ( 0 < P.flash )
    {
        P.flash -= dt;
    }
    if ( 0 < P.msg_t )
    {
        P.msg_t -= dt;
    }
    for ( int i = 0; MAX_TRACERS > i; i++ )
    {
        if ( s_tr[i].used && (0 >= (s_tr[i].ttl -= dt)) )
        {
            s_tr[i].used = false;
        }
    }
    for ( int i = 0; MAX_BLASTS > i; i++ )
    {
        if ( s_bl[i].used && (0.8f < (s_bl[i].t += dt)) )
        {
            s_bl[i].used = false;
        }
    }
    for ( int i = 0; MAX_SPARKS > i; i++ )
    {
        if ( s_sp[i].used && (0.18f < (s_sp[i].t += dt)) )
        {
            s_sp[i].used = false;
        }
    }

    r3d_cam cam;
    switch ( P.state )
    {
        case ST_TITLE: {
            float a = P.time * 0.06f;
            v3 f;
            if ( MAP_ALBACETE == s_opt_map )
            {
                /* round the centre, the fair's wheel and the park below */
                float ang = 2.4f + a * 0.8f;
                P.cam_pos = v3m(600.0f + sinf(ang) * 300.0f, 105.0f, 470.0f + cosf(ang) * 300.0f);
                v3 look = v3m(640.0f, 0.0f, 430.0f);
                f = v3sub(look, P.cam_pos);
                f = v3scale(f, 1.0f / sqrtf(v3dot(f, f)));
            }
            else
            {
                /* a slow flight over the city, into the sunset */
                P.cam_pos = v3m(820.0f + sinf(a) * 70.0f, 100.0f + 15.0f * sinf(a * 1.7f), 380.0f + cosf(a) * 90.0f - 40.0f);
                float yaw = -1.22f + 0.22f * sinf(a * 0.9f), pitch = 0.2f;
                f = v3m(sinf(yaw) * cosf(pitch), -sinf(pitch), cosf(yaw) * cosf(pitch));
            }
            cam.pos = P.cam_pos;
            cam.fwd = f;
            cam.right = v3scale(v3m(f.z, 0, -f.x), 1.0f / sqrtf(f.z * f.z + f.x * f.x));
            cam.up = v3cross(f, cam.right);
            if ( 0 > cam.up.y )
            {
                cam.up = v3scale(cam.up, -1);
            }
            cam.focal = 190; cam.cx = GW * 0.5f; cam.cy = GH * 0.40f; cam.far_dist = 420;
            r3d_begin(fb, &cam);
            world_draw_sky(P.time);
            world_draw_ground(P.time);
            world_draw_objects();
            draw_title();
            if ( pressed & BTN_UP )
            {
                s_menu = (s_menu + 2) % 3;
            }
            if ( pressed & BTN_DOWN )
            {
                s_menu = (s_menu + 1) % 3;
            }
            if ( (pressed & (BTN_A | BTN_LEFT | BTN_RIGHT)) && (1 == s_menu) )
            {
                int map;
                if ( MAP_NEONBAY == s_opt_map )
                {
                    map = MAP_ALBACETE;
                }
                else
                {
                    map = MAP_NEONBAY;
                }
                game_set_options(map, s_opt_style);
            }
            else if ( (pressed & (BTN_A | BTN_LEFT | BTN_RIGHT)) && (2 == s_menu) )
            {
                int style;
                if ( STYLE_NEON == s_opt_style )
                {
                    style = STYLE_REAL;
                }
                else
                {
                    style = STYLE_NEON;
                }
                game_set_options(s_opt_map, style);
            }
            else if ( (pressed & BTN_A) && (0 == s_menu) )
            {
                P.state = ST_PLAY;
                P.state_t = 0;
                P.cam_init = false;
                P.prev_btn |= BTN_A;
                P.chord_used = true;
                say(world_name(), rgb(255, 90, 190), 2.0f);
            }
            break;
        }
        case ST_PLAY:
            update_play(dt, btn, pressed);
            cam = make_cam(dt);
            r3d_begin(fb, &cam);
            draw_world();
            draw_hud();
            break;
        case ST_MAP:
            r3d_begin(fb, &(r3d_cam){.pos = {0, 0, 0}, .right = {1, 0, 0}, .up = {0, 1, 0}, .fwd = {0, 0, 1}, .focal = 100, .cx = 0, .cy = 0, .far_dist = 100});
            draw_map();
            if ( (0.3f < P.state_t) && ((pressed & (BTN_B | BTN_MAP)) || (((BTN_A | BTN_B) == (btn & (BTN_A | BTN_B))) && !P.chord_used)) )
            {
                P.state = ST_PLAY;
                P.chord_used = true;
                P.prev_btn |= BTN_B;
            }
            if ( !(btn & (BTN_A | BTN_B)) )
            {
                P.chord_used = false;
            }
            break;
        case ST_BUSTED:
        case ST_KO: {
            cam = make_cam(dt);
            r3d_begin(fb, &cam);
            draw_world();
            uint16_t veil, mc;
            const char *m;
            if ( ST_KO == P.state )
            {
                veil = rgb(120, 0, 20);
                m = "K.O.";
                mc = rgb(255, 80, 80);
            }
            else
            {
                veil = rgb(10, 20, 80);
                m = "ARRESTED";
                mc = rgb(120, 170, 255);
            }
            r2d_rect_blend(0, 0, GW, GH, veil, (int)clampf(P.state_t * 12.0f, 0, 18));
            r2d_text_shadow((GW - r2d_text_w(4, m)) / 2, 140, 4, mc, m);
            if ( 3.0f < P.state_t )
            {
                int lose = P.money / 10;
                P.money -= lose;
                P.shown_money = P.money;
                if ( ST_KO == P.state )
                {
                    respawn(SPAWN_X, SPAWN_Z);
                }
                else
                {
                    respawn(SPAWN_X, SPAWN_Z);
                }
                P.state = ST_PLAY;
                P.state_t = 0;
                if ( 0 != lose )
                {
                    say("-10% OF YOUR CASH", rgb(255, 200, 200), 2.0f);
                }
                else
                {
                    say("TRY AGAIN", rgb(255, 200, 200), 2.0f);
                }
                new_job();
            }
            break;
        }
    }

    {
        float cop = 1e9f;
        for ( int i = 0; MAX_CARS > i; i++ )
        {
            if ( s_car[i].used && (CAR_POLICE == s_car[i].state) )
            {
                float d = sqrtf(dist2(s_car[i].x, s_car[i].z, P.x, P.z));
                if ( d < cop )
                {
                    cop = d;
                }
            }
        }
        bool playing = (ST_PLAY == P.state) || (ST_MAP == P.state);
        float siren = 0;
        if ( playing )
        {
            siren = clampf(1.0f - cop / 150.0f, 0, 1);
        }
        snd_siren(siren);
        float engine_on = 0.0f, engine_v = 0;
        if ( playing && (0 <= P.car) )
        {
            engine_on = 1.0f;
        }
        if ( 0 <= P.car )
        {
            engine_v = fabsf(s_car[P.car].speed);
        }
        snd_engine(engine_on, engine_v);
        float music;
        if ( ST_TITLE == P.state )
        {
            music = 1.0f;
        }
        else if ( playing && (0 <= P.car) )
        {
            music = 0.55f;
        }
        else
        {
            music = 0.0f;
        }
        snd_music(music);
        float heli = 0.0f;
        if ( playing && P.heli )
        {
            heli = clampf(1.0f - sqrtf(dist2(P.hx, P.hz, P.x, P.z)) / 120.0f, 0, 1);
        }
        snd_heli(heli);
    }
    int npol = 0, ntr = 0, nped = 0;
    for ( int i = 0; MAX_CARS > i; i++ )
    {
        if ( s_car[i].used )
        {
            if ( CAR_POLICE == s_car[i].state )
            {
                npol++;
            }
            else
            {
                ntr++;
            }
        }
    }
    for ( int i = 0; MAX_PEDS > i; i++ )
    {
        if ( s_ped[i].used )
        {
            nped++;
        }
    }
    float car_y = 0.0f, car_vy = 0.0f;
    if ( 0 <= P.car )
    {
        car_y = s_car[P.car].y;
        car_vy = s_car[P.car].vy;
    }
    snprintf(s_status, sizeof s_status, "st=%d y=%.1f vy=%.1f pos=%.0f,%.0f car=%d hp=%.0f $%d wanted=%.1f cars=%d cops=%d peds=%d tris=%u",
             P.state, car_y, car_vy, P.x, P.z, P.car, P.hp, P.money, P.wanted, ntr, npol, nped, (unsigned)r3d_tris);
}
