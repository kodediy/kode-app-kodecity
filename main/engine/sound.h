/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file sound.h
 * @brief KodeCity's synthesiser: every sound is made here, no samples.
 */
#ifndef SOUND_H_
#define SOUND_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Types ---- */

/** The one-shot sound effects. */
enum
{
    SFX_GUN,    /**< A gun shot. */
    SFX_BOOM,   /**< An explosion. */
    SFX_CRASH,  /**< A crash. */
    SFX_COIN,   /**< A coin picked up. */
    SFX_HIT,    /**< A hit. */
    SFX_DOOR,   /**< A door. */
    SFX_FAIL,   /**< A failure. */
    SFX_COUNT   /**< The number of sound effects. */
};

/* ---- Calls ---- */

/**
 * @brief Start the synthesiser.
 * @param rate The sample rate, Hz.
 */
void snd_init(int rate);

/**
 * @brief Fill `frames` stereo frames (interleaved int16).
 * @param out    The output buffer, 2 * frames samples.
 * @param frames The number of frames.
 */
void snd_render(int16_t *out, int frames);

/**
 * @brief The same, mono.
 * @param out    The output buffer, frames samples.
 * @param frames The number of frames.
 */
void snd_render_mono(int16_t *out, int frames);

/* ---- What the game says, every frame ---- */

/**
 * @brief Set the engine sound.
 * @param on    0..1
 * @param speed Speed, m/s.
 */
void snd_engine(float on, float speed);

/**
 * @brief Set the siren volume.
 * @param vol 0..1
 */
void snd_siren(float vol);

/**
 * @brief Set the music volume.
 * @param vol 0..1: the radio
 */
void snd_music(float vol);

/**
 * @brief Set the helicopter volume.
 * @param vol 0..1: rotor thump
 */
void snd_heli(float vol);

/**
 * @brief Play a one-shot sound effect.
 * @param sfx One of SFX_*.
 */
void snd_play(int sfx);

#ifdef __cplusplus
}
#endif

#endif /* SOUND_H_ */
