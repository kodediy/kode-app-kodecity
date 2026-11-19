/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file sim.c
 * @brief Play the game on a desktop from a script and write the frames it asks for, for the bench.
 *
 * usage: sim <script> <outdir>
 * Script lines: `<frame> <buttons>` (u d l r a b m, or - for none), `<frame> shot`
 * (writes outdir/f<frame>.ppm) or `<frame> star` (one more police star).
 * The game runs at 30 frames a second of game time. KC_MAP and KC_STYLE pick
 * the city and the look.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../../main/engine/game.h"
#include "../../main/engine/r3d.h"

#define SIM_LINES_MAX 512   /**< Script lines read at most.            */
#define SIM_WORD_MAX  32    /**< Longest word on a line, NUL included. */
#define SIM_PATH_MAX  256   /**< Longest output path.                  */
#define SIM_SHOT      1     /**< A line that writes a frame.           */
#define SIM_STAR      2     /**< A line that adds a police star.       */

static uint16_t s_fb[GW * GH];

/**
 * @brief Write the frame as a binary PPM.
 * @param path Where.
 */
static void write_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");

    if ( NULL == f )
    {
        return;
    }
    (void)fprintf(f, "P6 %d %d 255\n", GW, GH);
    for ( int i = 0; (GW * GH) > i; i++ )
    {
        uint16_t c = s_fb[i];
        unsigned char p[3] = {(unsigned char)((c >> 11) << 3), (unsigned char)(((c >> 5) & 63) << 2),
                              (unsigned char)((c & 31) << 3)};

        (void)fwrite(p, 1, 3, f);
    }
    (void)fclose(f);
}

/**
 * @brief The game's buttons spelled by a script word.
 * @param word Letters u d l r a b m.
 * @return The BTN_* bits.
 */
static uint32_t word_buttons(const char *word)
{
    uint32_t b = 0U;

    for ( const char *c = word; '\0' != *c; c++ )
    {
        switch ( *c )
        {
            case 'u':
                b |= BTN_UP;
                break;
            case 'd':
                b |= BTN_DOWN;
                break;
            case 'l':
                b |= BTN_LEFT;
                break;
            case 'r':
                b |= BTN_RIGHT;
                break;
            case 'a':
                b |= BTN_A;
                break;
            case 'b':
                b |= BTN_B;
                break;
            case 'm':
                b |= BTN_MAP;
                break;
            default:
                break;
        }
    }
    return b;
}

/**
 * @brief A number from the environment.
 * @param name The variable.
 * @return Its value, 0 when it is not set.
 */
static int env_int(const char *name)
{
    const char *v = getenv(name);
    int n = 0;

    if ( NULL != v )
    {
        n = atoi(v);
    }
    return n;
}

int main(int argc, char **argv)
{
    static int fr[SIM_LINES_MAX], kind[SIM_LINES_MAX];
    static uint32_t bt[SIM_LINES_MAX];
    char word[SIM_WORD_MAX];
    int n = 0;

    if ( 3 > argc )
    {
        (void)fprintf(stderr, "usage: sim <script> <outdir>\n");
        return 2;
    }
    FILE *s = fopen(argv[1], "r");

    if ( NULL == s )
    {
        return 1;
    }
    while ( (SIM_LINES_MAX > n) && (2 == fscanf(s, "%d %31s", &fr[n], word)) )
    {
        kind[n] = 0;
        bt[n] = 0U;
        if ( 0 == strcmp(word, "shot") )
        {
            kind[n] = SIM_SHOT;
        }
        else if ( 0 == strcmp(word, "star") )
        {
            kind[n] = SIM_STAR;
        }
        else if ( 0 != strcmp(word, "-") )
        {
            bt[n] = word_buttons(word);
        }
        n++;
    }
    (void)fclose(s);
    if ( 0 == n )
    {
        return 1;
    }
    int last = fr[n - 1];

    game_init(7);
    if ( (NULL != getenv("KC_MAP")) || (NULL != getenv("KC_STYLE")) )
    {
        game_set_options(env_int("KC_MAP"), env_int("KC_STYLE"));
    }
    uint32_t btn = 0U;
    int k = 0;
    clock_t t0 = clock();
    double worst = 0.0;

    for ( int f = 0; f <= last; f++ )
    {
        bool want_shot = false;

        while ( (k < n) && (fr[k] == f) )
        {
            if ( SIM_SHOT == kind[k] )
            {
                want_shot = true;
            }
            else if ( SIM_STAR == kind[k] )
            {
                game_debug(1);
            }
            else
            {
                btn = bt[k];
            }
            k++;
        }
        clock_t a = clock();

        game_frame(s_fb, 1.0f / 30.0f, btn);
        double ms = (double)(clock() - a) * 1000.0 / CLOCKS_PER_SEC;

        if ( ms > worst )
        {
            worst = ms;
        }
        if ( want_shot )
        {
            char p[SIM_PATH_MAX];

            (void)snprintf(p, sizeof p, "%s/f%04d.ppm", argv[2], f);
            write_ppm(p);
            (void)printf("f%d: %s\n", f, game_status());
        }
    }
    (void)printf("avg %.2f ms, worst %.2f ms per frame (host)\n",
                 (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC / (last + 1), worst);
    return 0;
}
