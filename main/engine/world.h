/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file world.h
 * @brief The city of Neon Bay: the ground map, the buildings, the palms.
 */
#ifndef WORLD_H_
#define WORLD_H_

#include <stdint.h>
#include <stdbool.h>
#include "r3d.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Sizes ---- */

#define WORLD_M      1024                 /**< metres on a side */
#define CELL_M       4                    /**< metres per ground map cell */
#define MAP_N        (WORLD_M / CELL_M)   /**< ground map cells on a side */
#define BLOCK_M      64                   /**< metres per city block */
#define BLOCKS       (WORLD_M / BLOCK_M)  /**< city blocks on a side */
#define ROAD_W       12                   /**< a road band at the start of every block */
#define WALK_W       4                    /**< the sidewalk around a block's lots */

/* ---- Types ---- */

/** The kind of ground in a map cell. */
enum {
    T_WATER,   /**< sea, or the canal */
    T_SAND,    /**< beach */
    T_GRASS,   /**< grass */
    T_ROAD,    /**< road */
    T_WALK,    /**< sidewalk */
    T_LOT,     /**< a paved lot */
    T_PARK,    /**< park */
    T_PIER,    /**< pier */
    T_FIELD,   /**< ploughed field */
    T_FIELD2,  /**< wheat field */
    T_VINE     /**< vineyard */
};

/** The city to build. */
enum {
    MAP_NEONBAY,   /**< Neon Bay, a coast at sunset */
    MAP_ALBACETE   /**< a joke Albacete in La Mancha */
};

/** The look of the world. */
enum {
    STYLE_NEON,   /**< sunset, pastel, neon */
    STYLE_REAL    /**< daylight, sober colours */
};

/** A building: a box on the ground. */
typedef struct {
    float x0;           /**< west edge, metres */
    float z0;           /**< south edge, metres */
    float x1;           /**< east edge, metres */
    float z1;           /**< north edge, metres */
    float h;            /**< height, metres */
    uint16_t wall;      /**< wall colour, RGB565 */
    uint16_t roof;      /**< roof colour, RGB565 */
    uint16_t glass;     /**< window colour, RGB565 */
    uint8_t  pattern;   /**< 0 plain, 1 windows */
    uint8_t  sign;      /**< a neon band near the top: 0 none */
    uint16_t neon;      /**< the sign's colour, RGB565 */
} building_t;

/** A palm or a tree. */
typedef struct {
    float x;      /**< position x, metres */
    float z;      /**< position z, metres */
    float h;      /**< height, metres */
    float lean;   /**< lean of the trunk */
    int kind;     /**< kind 0 palm, 1 tree */
} palm_t;

/** A named place for missions. */
typedef struct {
    const char *name;   /**< the name shown */
    float x;            /**< position x, metres */
    float z;            /**< position z, metres */
} place_t;

/* ---- Data ---- */

extern uint8_t (*world_map)[MAP_N];      /**< [z][x], MAP_N rows */

/* ---- Calls ---- */

/**
 * @brief Build a city from a seed.
 * @param seed  the random seed (0 picks a fixed one)
 * @param map   MAP_NEONBAY or MAP_ALBACETE
 * @param style STYLE_NEON or STYLE_REAL
 */
void world_init(uint32_t seed, int map, int style);

/**
 * @brief The city that was built.
 * @return MAP_NEONBAY or MAP_ALBACETE
 */
int world_map_id(void);

/**
 * @brief The look of the world.
 * @return STYLE_NEON or STYLE_REAL
 */
int world_style(void);

/**
 * @brief The city's name.
 * @return the name, upper case
 */
const char *world_name(void);

/**
 * @brief Type of the ground under a point.
 * @param x position x, metres
 * @param z position z, metres
 * @return a T_ value; T_WATER outside the map
 */
int world_type(float x, float z);

/**
 * @brief Is a disc of radius r free of buildings and water?
 * @param x            centre x, metres
 * @param z            centre z, metres
 * @param r            radius, metres
 * @param water_blocks true when water counts as blocked
 * @return true when the disc is free
 */
bool world_free(float x, float z, float r, bool water_blocks);

/**
 * @brief The first building a segment hits, or -1; *t is the fraction.
 * @param x0 start x, metres
 * @param z0 start z, metres
 * @param x1 end x, metres
 * @param z1 end z, metres
 * @param t  out: the fraction of the segment at the hit (1 when none)
 * @return 1 when the segment hits a building, -1 when not
 */
int world_ray(float x0, float z0, float x1, float z1, float *t);

/**
 * @brief Drawing: the sky.
 * @param time the game time, seconds
 */
void world_draw_sky(float time);

/**
 * @brief Drawing: the ground.
 * @param time the game time, seconds
 */
void world_draw_ground(float time);

/**
 * @brief Drawing: the buildings, trees and props.
 */
void world_draw_objects(void);

/**
 * @brief Set the time the animated objects use.
 * @param t the game time, seconds
 */
void world_set_time(float t);

/**
 * @brief The overview map, MAP_N x MAP_N RGB565, made once.
 * @return the overview pixels
 */
const uint16_t *world_overview(void);

/**
 * @brief Road helpers: the centre of the intersection nearest a point.
 * @param x  position x, metres
 * @param z  position z, metres
 * @param cx out: the crossing's x, metres
 * @param cz out: the crossing's z, metres
 */
void world_nearest_crossing(float x, float z, float *cx, float *cz);

/**
 * @brief Is the point on a road?
 * @param x position x, metres
 * @param z position z, metres
 * @return true on a road
 */
bool world_is_road(float x, float z);

/**
 * @brief A few named places for missions.
 * @param out out: the places of the current city
 * @return the number of places
 */
int world_places(const place_t **out);

#ifdef __cplusplus
}
#endif

#endif /* WORLD_H_ */
