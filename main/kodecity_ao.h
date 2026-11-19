/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kodecity_ao.h
 * @brief The renderer: LVGL on the glass with one full-screen frame that shows the game.
 */
#ifndef KODECITY_AO_H_
#define KODECITY_AO_H_

#include <stdbool.h>
#include <stdint.h>

#include "kode_sys.h"
#include "kscreen.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- The Active Object ---- */

/** @brief The singleton, as an opaque QActive. */
extern QActive * const AO_KodeCity;

/**
 * @brief Construct the renderer. Call after kode_init(), before KodeCity_start().
 */
void KodeCity_ctor(void);

/**
 * @brief Give the renderer its thread, queue and stack, and let it run.
 */
void KodeCity_start(void);

/* ---- What the frame hand-off sees ---- */

/**
 * @brief One of the two pictures the game draws into.
 * @param i 0 or 1.
 * @return The picture, NULL until the glass is the renderer's.
 */
kscreen_pic_t *kodecity_ao_pic(uint32_t i);

/**
 * @brief Tell the renderer picture @p i holds a finished frame.
 * @param i 0 or 1.
 * @return true when the event is in the renderer's queue.
 */
bool kodecity_ao_post_frame(uint32_t i);

/* ---- What the game task sees ---- */

/**
 * @brief The buttons held now, as the game's BTN_* bits, the glass's tap included.
 * @return The bits.
 */
uint32_t kodecity_buttons(void);

/**
 * @brief Hold buttons from the console, for the bench.
 * @param bits The game's BTN_* bits.
 * @param ms   How long.
 */
void kodecity_inject(uint32_t bits, uint32_t ms);

#ifdef __cplusplus
}
#endif

#endif /* KODECITY_AO_H_ */
