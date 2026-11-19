/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file game.h
 * @brief KodeCity, the game.
 *
 * Portable: the board and the host both call it.
 */
#ifndef GAME_H_
#define GAME_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Buttons ---- */

/* the six buttons of the Kode Dot */
#define BTN_UP     0x01   /**< The up button. */
#define BTN_DOWN   0x02   /**< The down button. */
#define BTN_LEFT   0x04   /**< The left button. */
#define BTN_RIGHT  0x08   /**< The right button. */
#define BTN_A      0x10   /**< The blue button, top. */
#define BTN_B      0x20   /**< The red button, bottom. */
#define BTN_MAP    0x40   /**< A tap on the glass. */

/* ---- Calls ---- */

/**
 * @brief Start a new game, on the title screen.
 * @param seed The seed of the random numbers; 0 uses a fixed one.
 */
void game_init(uint32_t seed);

/**
 * @brief Advance by dt seconds with these buttons held, and draw into fb (GW x GH).
 * @param fb      The frame to draw into, GW x GH pixels.
 * @param dt      The seconds since the last frame.
 * @param buttons The buttons held, a mask of BTN_*.
 */
void game_frame(uint16_t *fb, float dt, uint32_t buttons);

/**
 * @brief What the board may want to know: how hard to vibrate, set each frame.
 * @return 0 for still, more than 0 to vibrate (0..255).
 */
uint8_t game_rumble(void);

/**
 * @brief A short line for the console.
 * @return The line, rewritten each frame.
 */
const char *game_status(void);

/**
 * @brief Choose the map and the style. Rebuilds the city.
 * @param map   0 Neon Bay, 1 Albacete.
 * @param style 0 neon, 1 realistic.
 */
void game_set_options(int map, int style);

/**
 * @brief Bench: 1 one more star, 2 +$1000, 3 no stars, 4 the other map, 5 the other style.
 * @param what Which of these to do.
 */
void game_debug(int what);

/**
 * @brief Skip the title (bench).
 */
void game_start_now(void);

#ifdef __cplusplus
}
#endif

#endif /* GAME_H_ */
