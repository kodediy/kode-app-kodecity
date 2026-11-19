/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kodecity_main.c
 * @brief KodeCity: an open-world game for the Kode Dot. Starts the renderer and the game's tasks.
 *
 * Three tasks of the app's own, all on kode.h or plain FreeRTOS:
 *   game   - updates and draws the city on core 1, at whatever rate it runs;
 *   audio  - the game's synthesiser into the speaker's stream, 48 kHz mono;
 *   haptic - turns the game's jolts into vibrations.
 * The renderer (kodecity_ao.c) owns the glass and the buttons.
 */
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "kode.h"
#include "kode_sys.h"
#include "kode_variables.h"

#include "game.h"
#include "kodecity_ao.h"
#include "kodecity_frame.h"
#include "r3d.h"
#include "sound.h"

#define TAG "kodecity"

KODE_APP_MANIFEST("KodeCity", "games");

#define KC_AUDIO_RATE     48000U  /**< The speaker's stream rate.               */
#define KC_AUDIO_FRAMES   480U    /**< 10 ms a write.                           */
#define KC_AUDIO_WAIT_MS  100U
#define KC_GAME_STACK     24576U
#define KC_AUDIO_STACK    6144U
#define KC_HAPTIC_STACK   4096U
#define KC_GAME_PRIO      2U      /**< Below every Active Object: it only computes. */
#define KC_AUDIO_PRIO     6U
#define KC_HAPTIC_PRIO    3U
#define KC_FRAME_WAIT_MS  500U
#define KC_HAPTIC_TICK_MS 25U
#define KC_HAPTIC_GAP_MS  110U    /**< The least time between two vibrations.   */

static volatile float s_fps;
static volatile float s_frame_ms;

/* ---- Memory for the engine --------------------------------------------- */

void *kc_fast_alloc(unsigned bytes)
{
    void *p = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    if ( NULL == p )
    {
        KLOG_W(TAG, "fast alloc of %u fell back to PSRAM", bytes);
        p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    }
    return p;
}

void *kc_slow_alloc(unsigned bytes)
{
    return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
}

/* ---- The tasks ---------------------------------------------------------- */

static void game_task(void *arg)
{
    int64_t last = esp_timer_get_time();
    int64_t fps_t = last;
    uint32_t frames = 0U;
    float work = 0.0f;

    (void)arg;
    game_init((uint32_t)esp_timer_get_time());
    KLOG_I(TAG, "game up; internal heap %u KB free",
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024U));
    for ( ;; )
    {
        uint16_t *fb = kodecity_frame_begin(KC_FRAME_WAIT_MS);

        if ( NULL == fb )
        {
            continue;
        }
        int64_t now = esp_timer_get_time();
        float dt = (float)(now - last) / 1e6f;

        last = now;
        game_frame(fb, dt, kodecity_buttons());
        work += (float)(esp_timer_get_time() - now) / 1000.0f;
        kodecity_frame_end();
        /* One tick for core 1's idle task: its watchdog wants it to run. */
        vTaskDelay(1);
        frames++;
        if ( (now - fps_t) > 1000000 )
        {
            s_fps = (float)frames * 1e6f / (float)(now - fps_t);
            s_frame_ms = work / (float)frames;
            frames = 0U;
            work = 0.0f;
            fps_t = now;
        }
    }
}

static void audio_task(void *arg)
{
    static int16_t buf[KC_AUDIO_FRAMES];
    bool open = false;

    (void)arg;
    snd_init((int)KC_AUDIO_RATE);
    for ( ;; )
    {
        if ( !open )
        {
            open = kode_audio_stream_open(1U);
            if ( !open )
            {
                KLOG_W(TAG, "no speaker yet: %s", kode_error());
                kode_delay(1000U);
                continue;
            }
        }
        snd_render_mono(buf, (int)KC_AUDIO_FRAMES);
        if ( !kode_audio_stream_write(buf, KC_AUDIO_FRAMES, KC_AUDIO_WAIT_MS, NULL) )
        {
            /* Closed for silence, or someone else has the speaker: open again. */
            open = false;
        }
    }
}

static void haptic_task(void *arg)
{
    /* Vibrations of the game's own, from a tap to a blast. */
    static const uint16_t SHOT[] = {28U};
    static const uint16_t BUMP[] = {60U};
    static const uint16_t CRASH[] = {90U, 40U, 70U};
    static const uint16_t BOOM[] = {260U, 60U, 160U};
    uint8_t prev = 0U;
    uint32_t last = 0U;

    (void)arg;
    (void)kode_haptic_add_profile("kc_shot", SHOT, 1U, 45U);
    (void)kode_haptic_add_profile("kc_bump", BUMP, 1U, 65U);
    (void)kode_haptic_add_profile("kc_crash", CRASH, 3U, 85U);
    (void)kode_haptic_add_profile("kc_boom", BOOM, 3U, 100U);
    for ( ;; )
    {
        uint8_t level = game_rumble();
        uint32_t now = kode_uptime_ms();

        /* A rise of the game's jolt is one vibration, as strong as the jolt. */
        if ( ((uint8_t)(prev + 20U) < level) && ((now - last) >= KC_HAPTIC_GAP_MS) )
        {
            const char *name = "kc_shot";

            if ( 230U < level )
            {
                name = "kc_boom";
            }
            else if ( 180U < level )
            {
                name = "kc_crash";
            }
            else if ( 110U < level )
            {
                name = "kc_bump";
            }
            else
            {
                name = "kc_shot";
            }
            (void)kode_haptic_play(name);
            last = now;
        }
        prev = level;
        kode_delay(KC_HAPTIC_TICK_MS);
    }
}

/* ---- The console, for the bench ----------------------------------------- */

static kode_status_t get_fps(char *buf, size_t cap)
{
    (void)snprintf(buf, cap, "%.1f", (double)s_fps);
    return KODE_OK;
}

static kode_status_t get_ms(char *buf, size_t cap)
{
    (void)snprintf(buf, cap, "%.1f", (double)s_frame_ms);
    return KODE_OK;
}

static kode_status_t run_press(const kode_value_t *arg)
{
    kode_status_t status = KODE_DENIED;

    if ( NULL != arg )
    {
        kodecity_inject((uint32_t)arg->i & 0x7FU, 200U);
        status = KODE_OK;
    }
    return status;
}

static kode_status_t run_hold(const kode_value_t *arg)
{
    kode_status_t status = KODE_DENIED;

    if ( NULL != arg )
    {
        kodecity_inject((uint32_t)arg->i & 0x7FU, 2000U);
        status = KODE_OK;
    }
    return status;
}

static kode_status_t run_debug(const kode_value_t *arg)
{
    kode_status_t status = KODE_DENIED;

    if ( NULL != arg )
    {
        game_debug((int)arg->i);
        status = KODE_OK;
    }
    return status;
}

static kode_status_t run_start(const kode_value_t *arg)
{
    (void)arg;
    game_start_now();
    return KODE_OK;
}

static const kode_variable_t VARIABLES[] = {
    { .name = "fps", .get = get_fps, .desc = "Frames a second" },
    { .name = "frame_ms", .get = get_ms, .desc = "Game work per frame, ms" },
};

static const kode_function_t FUNCTIONS[] = {
    { .name = "press", .arg = KODE_TYPE_INT, .min = 0, .max = 127, .run = run_press,
      .desc = "Tap: 1 up 2 dn 4 lt 8 rt 16 top 32 bot" },
    { .name = "hold", .arg = KODE_TYPE_INT, .min = 0, .max = 127, .run = run_hold,
      .desc = "Hold the same bits for 2 s" },
    { .name = "debug", .arg = KODE_TYPE_INT, .min = 1, .max = 5, .run = run_debug,
      .desc = "1 star 2 cash 3 no stars 4 map 5 style" },
    { .name = "start", .run = run_start, .desc = "Skip the title" },
};

/* ---- Start ---------------------------------------------------------------- */

void app_main(void)
{
    kode_init();
    KodeCity_ctor();
    KodeCity_start();
    if ( KODE_OK != KODE_VARIABLES_ADD(VARIABLES) )
    {
        KLOG_E(TAG, "the variable table was refused");
    }
    if ( KODE_OK != KODE_FUNCTIONS_ADD(FUNCTIONS) )
    {
        KLOG_E(TAG, "the function table was refused");
    }
    (void)xTaskCreatePinnedToCore(game_task, "kc_game", KC_GAME_STACK, NULL, KC_GAME_PRIO, NULL, 1);
    (void)xTaskCreatePinnedToCore(audio_task, "kc_audio", KC_AUDIO_STACK, NULL, KC_AUDIO_PRIO, NULL, 0);
    (void)xTaskCreatePinnedToCore(haptic_task, "kc_haptic", KC_HAPTIC_STACK, NULL, KC_HAPTIC_PRIO, NULL, 0);
}
