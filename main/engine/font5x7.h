/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file font5x7.h
 * @brief A 5x7 bitmap font, MSB-left in the low 5 bits of each row.
 */
#ifndef FONT5X7_H_
#define FONT5X7_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Constants ---- */

#define FONT5X7_GLYPHS 76   /**< The number of glyphs in the font. */

/* ---- Data ---- */

/** The glyphs, seven rows each. */
extern const uint8_t font5x7[FONT5X7_GLYPHS][7];

/* ---- Calls ---- */

/**
 * @brief The glyph index of a character.
 * @param ch The character.
 * @return The index into font5x7; 0 (space) for a character not in the font.
 */
int font5x7_index(char ch);

#ifdef __cplusplus
}
#endif

#endif /* FONT5X7_H_ */
