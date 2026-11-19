/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file sound.c
 * @brief A tiny synthesiser for Neon Bay.
 *
 * The engine is a pulse wave whose pitch follows the speed, with a little
 * noise for the exhaust. The siren wails between two pitches. One-shots are
 * shaped noise and tones. The radio plays an original synthwave loop: A minor,
 * F, C, G at 104 beats a minute - bass on the eighths, an arpeggio on the
 * sixteenths, a kick and a clap.
 */
#include "sound.h"

#include <math.h>
#include <string.h>

static int s_rate = 22050;
static volatile float s_eng_on, s_eng_speed, s_siren, s_music, s_heli;
static volatile uint32_t s_req[SFX_COUNT];
static uint32_t s_done[SFX_COUNT];

void snd_engine(float on, float speed)
{
    s_eng_on = on;
    s_eng_speed = speed;
}

void snd_siren(float v)
{
    s_siren = v;
}

void snd_music(float v)
{
    s_music = v;
}

void snd_heli(float v)
{
    s_heli = v;
}

void snd_play(int sfx)
{
    if ( (0 <= sfx) && (SFX_COUNT > sfx) )
    {
        s_req[sfx]++;
    }
}

static uint32_t s_noise = 0x1234567u;
static inline float noise(void)
{
    s_noise ^= s_noise << 13;
    s_noise ^= s_noise >> 17;
    s_noise ^= s_noise << 5;
    return (float)(int32_t)s_noise * (1.0f / 2147483648.0f);
}

/* one-shot voices */
typedef struct { int kind; float t, len, ph, lp; } voice_t;
#define NV 8
static voice_t s_v[NV];

static void start(int kind)
{
    static const float LEN[SFX_COUNT] = {0.16f, 1.3f, 0.35f, 0.45f, 0.12f, 0.15f, 0.7f};
    int pick = 0;
    float oldest = -1;
    for ( int i = 0; NV > i; i++ )
    {
        if ( 0 > s_v[i].kind )
        {
            pick = i;
            break;
        }
        if ( s_v[i].t > oldest )
        {
            oldest = s_v[i].t;
            pick = i;
        }
    }
    s_v[pick] = (voice_t){kind, 0, LEN[kind], 0, 0};
}

void snd_init(int rate)
{
    s_rate = rate;
    for ( int i = 0; NV > i; i++ )
    {
        s_v[i].kind = -1;
    }
    memset(s_done, 0, sizeof s_done);
}

/* ---- the radio --------------------------------------------------------- */

static const float BPM = 104.0f;
/* A minor - F - C - G: roots and arpeggio notes as semitones from A2 */
static const int ROOT[4] = {0, -4, 3, -2};
static const int ARP[4][4] = {{12, 15, 19, 24}, {8, 12, 15, 20}, {15, 19, 22, 27}, {10, 14, 17, 22}};
static double s_mt;      /* music time, seconds */
static float s_bass_ph, s_arp_ph, s_kick_ph, s_lead_lp;

static float note_hz(int semis)
{
    return 110.0f * powf(2.0f, semis / 12.0f);
}

static float music_sample(float dt)
{
    s_mt += dt;
    float beat = (float)(s_mt * BPM / 60.0);
    int bar = (int)(beat / 4.0f) % 4;
    float in_beat = beat - floorf(beat);
    float eighth = beat * 2.0f, in_eighth = eighth - floorf(eighth);
    float six = beat * 4.0f, in_six = six - floorf(six);
    int step = (int)six & 3;
    /* bass: a saw an octave down, plucked on every eighth */
    s_bass_ph += note_hz(ROOT[bar] - 12) * dt;
    s_bass_ph -= floorf(s_bass_ph);
    float bass = (s_bass_ph * 2.0f - 1.0f) * expf(-in_eighth * 3.0f) * 0.55f;
    /* arpeggio: a square, short */
    s_arp_ph += note_hz(ARP[bar][step]) * dt;
    s_arp_ph -= floorf(s_arp_ph);
    float sq;
    if ( 0.5f > s_arp_ph )
    {
        sq = 1.0f;
    }
    else
    {
        sq = -1.0f;
    }
    float arp = sq * expf(-in_six * 5.0f) * 0.16f;
    s_lead_lp += (arp - s_lead_lp) * 0.35f;
    /* kick on every beat: a falling sine */
    float kf = 120.0f * expf(-in_beat * 18.0f) + 42.0f;
    s_kick_ph += kf * dt;
    float kick = sinf(s_kick_ph * 6.2831853f) * expf(-in_beat * 7.0f) * 0.7f;
    /* a clap on two and four */
    int bi = (int)beat & 3;
    float clap;
    if ( (1 == bi) || (3 == bi) )
    {
        clap = noise() * expf(-in_beat * 14.0f) * 0.25f;
    }
    else
    {
        clap = 0.0f;
    }
    return bass + s_lead_lp + kick + clap;
}

/* ---- the mix ------------------------------------------------------------ */

static float s_eng_ph, s_eng_lp, s_siren_ph, s_siren_t, s_out_lp, s_heli_ph, s_heli_lp;

void snd_render(int16_t *out, int frames)
{
    for ( int k = 0; SFX_COUNT > k; k++ )
    {
        uint32_t r = s_req[k];
        while ( s_done[k] != r )
        {
            start(k);
            s_done[k]++;
        }
    }
    float dt = 1.0f / (float)s_rate;
    float eng_on = s_eng_on, sp = s_eng_speed, siren = s_siren, music = s_music, heli = s_heli;
    float eng_hz = 38.0f + sp * 3.2f;
    for ( int i = 0; i < frames; i++ )
    {
        float mix = 0.0f;
        if ( 0.01f < eng_on )
        {
            s_eng_ph += eng_hz * dt;
            s_eng_ph -= floorf(s_eng_ph);
            float pulse;
            if ( 0.3f > s_eng_ph )
            {
                pulse = 1.0f;
            }
            else
            {
                pulse = -0.45f;
            }
            float e = pulse + noise() * 0.35f;
            s_eng_lp += (e - s_eng_lp) * 0.18f;
            mix += s_eng_lp * 0.28f * eng_on;
        }
        if ( 0.01f < siren )
        {
            s_siren_t += dt;
            float w = 0.5f + 0.5f * sinf(s_siren_t * 3.2f);
            s_siren_ph += (620.0f + 420.0f * w) * dt;
            s_siren_ph -= floorf(s_siren_ph);
            float wail;
            if ( 0.5f > s_siren_ph )
            {
                wail = 0.14f;
            }
            else
            {
                wail = -0.14f;
            }
            mix += wail * siren;
        }
        if ( 0.01f < heli )
        {
            s_heli_ph += 13.0f * dt;
            s_heli_ph -= floorf(s_heli_ph);
            s_heli_lp += (noise() - s_heli_lp) * 0.05f;
            mix += s_heli_lp * 2.2f * expf(-s_heli_ph * 6.0f) * heli;
        }
        if ( 0.01f < music )
        {
            mix += music_sample(dt) * 0.6f * music;
        }
        for ( int v = 0; NV > v; v++ )
        {
            voice_t *vc = &s_v[v];
            if ( 0 > vc->kind )
            {
                continue;
            }
            float t = vc->t, x = 0;
            switch ( vc->kind )
            {
                case SFX_GUN:
                {
                    float env = expf(-t * 38.0f);
                    vc->lp += (noise() - vc->lp) * 0.6f;
                    float click;
                    if ( 0.004f > t )
                    {
                        click = 1.0f;
                    }
                    else
                    {
                        click = 0;
                    }
                    x = (vc->lp * 1.4f + click) * env;
                    break;
                }
                case SFX_BOOM:
                {
                    float ramp;
                    if ( 0.02f > t )
                    {
                        ramp = t / 0.02f;
                    }
                    else
                    {
                        ramp = 1.0f;
                    }
                    float env = expf(-t * 3.2f) * ramp;
                    vc->lp += (noise() - vc->lp) * (0.08f + 0.1f * expf(-t * 4.0f));
                    x = vc->lp * 3.2f * env;
                    break;
                }
                case SFX_CRASH:
                {
                    float env = expf(-t * 11.0f);
                    vc->lp += (noise() - vc->lp) * 0.25f;
                    vc->ph += (90.0f - t * 120.0f) * dt;
                    x = (vc->lp * 1.3f + sinf(vc->ph * 6.2831853f) * 0.6f) * env;
                    break;
                }
                case SFX_COIN:
                {
                    float f;
                    if ( 0.08f > t )
                    {
                        f = 988.0f;
                    }
                    else
                    {
                        f = 1319.0f;
                    }
                    vc->ph += f * dt;
                    float amp;
                    if ( 0.5f > (vc->ph - floorf(vc->ph)) )
                    {
                        amp = 0.3f;
                    }
                    else
                    {
                        amp = -0.3f;
                    }
                    x = amp * expf(-t * 5.0f);
                    break;
                }
                case SFX_HIT:
                {
                    vc->ph += (220.0f - t * 900.0f) * dt;
                    x = sinf(vc->ph * 6.2831853f) * expf(-t * 25.0f) * 0.7f;
                    break;
                }
                case SFX_DOOR:
                {
                    vc->lp += (noise() - vc->lp) * 0.3f;
                    x = vc->lp * expf(-t * 30.0f) * 0.9f;
                    break;
                }
                case SFX_FAIL:
                {
                    float f = 330.0f - t * 200.0f;
                    vc->ph += f * dt;
                    float amp;
                    if ( 0.5f > (vc->ph - floorf(vc->ph)) )
                    {
                        amp = 0.22f;
                    }
                    else
                    {
                        amp = -0.22f;
                    }
                    x = amp * (1.0f - t / vc->len);
                    break;
                }
            }
            mix += x;
            vc->t += dt;
            if ( vc->t >= vc->len )
            {
                vc->kind = -1;
            }
        }
        /* soft clip, a touch of smoothing */
        s_out_lp += (mix - s_out_lp) * 0.7f;
        float y = s_out_lp;
        y = y / (1.0f + fabsf(y));
        int16_t s = (int16_t)(y * 30000.0f);
        out[2 * i] = s;
        out[2 * i + 1] = s;
    }
}

void snd_render_mono(int16_t *out, int frames)
{
    int16_t st[2 * 128];
    while ( 0 < frames )
    {
        int n;
        if ( 128 < frames )
        {
            n = 128;
        }
        else
        {
            n = frames;
        }
        snd_render(st, n);
        for ( int i = 0; i < n; i++ )
        {
            out[i] = st[2 * i];
        }
        out += n;
        frames -= n;
    }
}
