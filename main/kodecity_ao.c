/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kodecity_ao.c
 * @brief The renderer: one frame object on the glass, fed by the game task through the pixel accelerator.
 *
 * The game draws GW x GH pixels in software, on a task of its own, straight
 * into a kscreen picture. When it has finished a frame it posts KC_FRAME_SIG;
 * this AO has the accelerator scale the picture by 1.5 into the full-screen
 * picture the frame object shows, invalidates the object, and hands the small
 * picture back to the game. LVGL draws on kscreen's render pass.
 *
 * The buttons and the glass come in as events; their state is kept here as
 * one word the game task reads.
 */
#include "kodecity_ao.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "kscreen.h"

#include "game.h"
#include "kodecity_frame.h"
#include "r3d.h"

#define TAG "kodecity.ao"

#define KC_GLASS_W      410U   /**< The glass, upright.                        */
#define KC_GLASS_H      502U
#define KC_SCALE        24U    /**< 1.5 in sixteenths: 272 x 332 to 408 x 498. */
#define KC_OUT_X        1      /**< Where the scaled frame lands, centred.     */
#define KC_OUT_Y        2
#define KC_POST_MARGIN  2U     /**< Queue room a frame post leaves free.       */

_Static_assert(((GW * KC_SCALE) / 16U) + KC_OUT_X <= KC_GLASS_W, "the scaled frame must fit the glass");
_Static_assert(((GH * KC_SCALE) / 16U) + KC_OUT_Y <= KC_GLASS_H, "the scaled frame must fit the glass");

/** @brief This AO's own signals, posted to itself, never published. */
enum KodeCitySignals {
    KC_TICK_SIG = MAX_SIG,  /**< kscreen's render pass.                   */
    KC_FRAME0_SIG,          /**< The game task finished a frame in pic 0. */
    KC_FRAME1_SIG           /**< The game task finished a frame in pic 1. */
};

/** @brief The renderer's state. */
typedef struct {
    QActive        super;
    kscreen_t      screen;   /**< LVGL on the glass.                         */
    kscreen_pic_t *small[2]; /**< What the game draws into, GW x GH, in turn. */
    kscreen_pic_t *big;      /**< What the frame object shows, the glass.    */
    lv_obj_t      *frame;    /**< The full-screen frame object.              */
} KodeCity;

static QEvtPtr     l_queue[KQP_AO_QUEUE_LEN];
static StackType_t l_stack[KQP_AO_STACK_BYTES / sizeof(StackType_t)];
static KodeCity     l_ao;
QActive * const AO_KodeCity = &l_ao.super;

static const QEvt FRAME_EVT[2] = { QEVT_INITIALIZER(KC_FRAME0_SIG), QEVT_INITIALIZER(KC_FRAME1_SIG) };

static volatile uint32_t s_buttons;
static volatile uint32_t s_touch;
static volatile uint32_t s_inject_bits;
static volatile uint32_t s_inject_until;

static QState KodeCity_initial(KodeCity * const me, void const * const par);
static QState KodeCity_waiting(KodeCity * const me, QEvt const * const e);
static QState KodeCity_drawing(KodeCity * const me, QEvt const * const e);

/* ---- The game task's side ---------------------------------------------- */

kscreen_pic_t *kodecity_ao_pic(uint32_t i)
{
    return l_ao.small[i & 1U];
}

bool kodecity_ao_post_frame(uint32_t i)
{
    return QACTIVE_POST_X(AO_KodeCity, &FRAME_EVT[i & 1U], KC_POST_MARGIN, (void *)0);
}

uint32_t kodecity_buttons(void)
{
    uint32_t b = s_buttons | s_touch;
    uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

    if ( (int32_t)(s_inject_until - now) > 0 )
    {
        b |= s_inject_bits;
    }
    return b;
}

void kodecity_inject(uint32_t bits, uint32_t ms)
{
    s_inject_bits = bits;
    s_inject_until = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS) + ms;
}

/* ---- Buttons ------------------------------------------------------------- */

/**
 * @brief The game's bit for one of the board's buttons.
 * @param id KODE_SYS_BUTTON_x.
 * @return The BTN_* bit, 0 for none.
 */
static uint32_t button_bit(uint32_t id)
{
    uint32_t bit = 0U;

    switch ( id )
    {
        case KODE_SYS_BUTTON_UP:    bit = BTN_UP;    break;
        case KODE_SYS_BUTTON_DOWN:  bit = BTN_DOWN;  break;
        case KODE_SYS_BUTTON_LEFT:  bit = BTN_LEFT;  break;
        case KODE_SYS_BUTTON_RIGHT: bit = BTN_RIGHT; break;
        case KODE_SYS_BUTTON_TOP:   bit = BTN_A;     break;
        case KODE_SYS_BUTTON_BOT:   bit = BTN_B;     break;
        default:                    bit = 0U;        break;
    }
    return bit;
}

/* ---- The AO -------------------------------------------------------------- */

void KodeCity_ctor(void)
{
    KodeCity * const me = &l_ao;

    QActive_ctor(&me->super, Q_STATE_CAST(&KodeCity_initial));
    kscreen_ctor(&me->screen, &me->super, (enum_t)KC_TICK_SIG);
    me->small[0] = NULL;
    me->small[1] = NULL;
    me->big = NULL;
    me->frame = NULL;
    kodecity_frame_init();
}

void KodeCity_start(void)
{
    QActive_setAttr(AO_KodeCity, TASK_NAME_ATTR, "kodecity");
    QACTIVE_START(AO_KodeCity, KODE_PRIO_USER_GUI,
                  l_queue, Q_DIM(l_queue),
                  l_stack, sizeof(l_stack),
                  (void *)0);
}

static QState KodeCity_initial(KodeCity * const me, void const * const par)
{
    (void)par;
    QActive_subscribe(&me->super, BUTTON_EVT_SIG);
    kscreen_start(&me->screen);
    return Q_TRAN(&KodeCity_waiting);
}

/**
 * @brief The events both states handle: the buttons.
 * @param e The event.
 * @return true when it was a button.
 */
static bool common_event(QEvt const * const e)
{
    bool handled = false;

    if ( BUTTON_EVT_SIG == e->sig )
    {
        ButtonEvt const * const be = (ButtonEvt const *)e;
        uint32_t bit = button_bit((uint32_t)be->id);

        if ( be->pressed )
        {
            s_buttons |= bit;
        }
        else
        {
            s_buttons &= ~bit;
        }
        handled = true;
    }
    return handled;
}

static QState KodeCity_waiting(KodeCity * const me, QEvt const * const e)
{
    QState status;

    if ( common_event(e) )
    {
        status = Q_HANDLED();
    }
    else if ( DISPLAY_STATE_SIG == e->sig )
    {
        if ( KODE_OK == kscreen_open(&me->screen, (DisplayStateEvt const *)e) )
        {
            status = Q_TRAN(&KodeCity_drawing);
        }
        else
        {
            status = Q_HANDLED();
        }
    }
    else
    {
        status = Q_SUPER(&QHsm_top);
    }
    return status;
}

static QState KodeCity_drawing(KodeCity * const me, QEvt const * const e)
{
    QState status = Q_HANDLED();

    if ( common_event(e) )
    {
        return status;
    }
    switch ( e->sig )
    {
        case Q_ENTRY_SIG: {
            lv_obj_t * const scr = lv_screen_active();

            lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
            me->small[0] = kscreen_pic_create((uint16_t)GW, (uint16_t)GH);
            me->small[1] = kscreen_pic_create((uint16_t)GW, (uint16_t)GH);
            me->big = kscreen_pic_create((uint16_t)KC_GLASS_W, (uint16_t)KC_GLASS_H);
            if ( (NULL != me->small[0]) && (NULL != me->small[1]) && (NULL != me->big) )
            {
                me->frame = kscreen_frame_create(scr, me->big);
                if ( NULL != me->frame )
                {
                    lv_obj_set_pos(me->frame, 0, 0);
                }
                kodecity_frame_release();
                kodecity_frame_release();
                KLOG_I(TAG, "the glass is ours: %ux%u scaled by %u/16", (unsigned)GW, (unsigned)GH, (unsigned)KC_SCALE);
            }
            else
            {
                KLOG_E(TAG, "no PSRAM for the pictures");
            }
            break;
        }
        case KC_TICK_SIG: {
            kscreen_tick(&me->screen);
            break;
        }
        case KC_FRAME0_SIG:
        case KC_FRAME1_SIG: {
            kscreen_pic_t *src = me->small[0];

            if ( KC_FRAME1_SIG == e->sig )
            {
                src = me->small[1];
            }
            if ( KODE_OK == kscreen_pic_scale(me->big, KC_OUT_X, KC_OUT_Y, src, NULL, (uint16_t)KC_SCALE) )
            {
                if ( NULL != me->frame )
                {
                    lv_obj_invalidate(me->frame);
                }
            }
            kodecity_frame_release();
            break;
        }
        case TOUCH_EVT_SIG: {
            TouchEvt const * const te = (TouchEvt const *)e;

            kscreen_touch(&me->screen, te);
            if ( te->pressed && !te->stale )
            {
                s_touch = BTN_MAP;
            }
            else
            {
                s_touch = 0U;
            }
            break;
        }
        case DISPLAY_REPAINT_SIG: {
            kscreen_repaint(&me->screen);
            break;
        }
        default: {
            status = Q_SUPER(&KodeCity_waiting);
            break;
        }
    }
    return status;
}
