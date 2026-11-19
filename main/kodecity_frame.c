/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kodecity_frame.c
 * @brief The hand-off of frames between the game task and the renderer.
 *
 * The game draws one picture while the accelerator scales the other. A
 * counting semaphore holds how many are free: the game task waits on it, the
 * renderer only ever gives it back. Kept out of the renderer's file, which
 * must never block.
 */
#include "kodecity_frame.h"

#include <stdbool.h>
#include <stddef.h>

#include "esp_cache.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "kodecity_ao.h"
#include "r3d.h"

#define KC_PICTURES 2U   /**< The pictures the game draws into, in turn. */

static SemaphoreHandle_t s_free;
static uint32_t s_turn;   /**< The picture drawn next; the game task's alone. */

void kodecity_frame_init(void)
{
    s_free = xSemaphoreCreateCounting(KC_PICTURES, 0U);
}

void kodecity_frame_release(void)
{
    if ( NULL != s_free )
    {
        (void)xSemaphoreGive(s_free);
    }
}

uint16_t *kodecity_frame_begin(uint32_t ms)
{
    uint16_t *px = NULL;

    BaseType_t got = pdFALSE;

    if ( NULL != s_free )
    {
        got = xSemaphoreTake(s_free, pdMS_TO_TICKS(ms));
    }
    if ( pdTRUE == got )
    {
        kscreen_pic_t *pic = kodecity_ao_pic(s_turn);

        if ( NULL != pic )
        {
            px = (uint16_t *)pic->buf.data;
        }
    }
    return px;
}

void kodecity_frame_end(void)
{
    kscreen_pic_t *pic = kodecity_ao_pic(s_turn);
    uint32_t done = s_turn;

    /* The accelerator reads memory, not the cache. */
    (void)esp_cache_msync(pic->buf.data, (size_t)GW * GH * 2U,
                          ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    s_turn ^= 1U;
    if ( !kodecity_ao_post_frame(done) )
    {
        /* The queue was full: drop the frame, keep the picture. */
        kodecity_frame_release();
    }
}
