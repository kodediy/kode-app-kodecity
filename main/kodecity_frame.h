/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kodecity_frame.h
 * @brief The hand-off of frames between the game task and the renderer: two pictures, in turn.
 */
#ifndef KODECITY_FRAME_H_
#define KODECITY_FRAME_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Calls ---- */

/**
 * @brief Make the hand-off. Call once, before the renderer starts.
 */
void kodecity_frame_init(void);

/**
 * @brief The renderer has a picture free again. Never blocks.
 */
void kodecity_frame_release(void);

/**
 * @brief Wait until the game may draw its next frame, and say where. For the game task only.
 * @param ms The longest to wait.
 * @return The picture's pixels (GW x GH RGB565), or NULL when none was free in time.
 */
uint16_t *kodecity_frame_begin(uint32_t ms);

/**
 * @brief Hand the frame just drawn to the renderer, which scales it onto the glass.
 */
void kodecity_frame_end(void);

#ifdef __cplusplus
}
#endif

#endif /* KODECITY_FRAME_H_ */
