/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file r3d.h
 * @brief A small software 3D renderer for the Kode Dot (and the host).
 *
 * The picture is GW x GH RGB565. The flat ground is drawn first, one row at a
 * time (every row of a camera with no roll meets the ground at one depth),
 * then the polygons with a 16-bit inverse-depth buffer. Nothing above the
 * ground can be hidden by it, so the ground never writes the depth buffer.
 */
#ifndef R3D_H_
#define R3D_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Constants ---- */

#define GW 272      /**< Frame width in pixels. */
#define GH 332      /**< Frame height in pixels. */

/* ---- Types ---- */

/** A 3D vector or point. */
typedef struct
{
    float x;    /**< X component. */
    float y;    /**< Y component (up). */
    float z;    /**< Z component. */
} v3;

/** The camera. */
typedef struct
{
    v3 pos;                /**< Position in the world. */
    v3 right;              /**< Right axis; orthonormal, no roll: right.y == 0 */
    v3 up;                 /**< Up axis; orthonormal with right and fwd. */
    v3 fwd;                /**< Forward axis; orthonormal with right and up. */
    float focal;           /**< Focal length, pixels */
    float cx;              /**< Projection centre, x */
    float cy;              /**< Projection centre, y */
    float far_dist;        /**< Fog is total here */
} r3d_cam;

/**
 * A polygon vertex. pattern: 0 plain, 1 windows (u,v in metres along the
 * wall), 2 stripes, 3 checker, 4 a facade: floors, framed windows, shop fronts.
 */
typedef struct
{
    v3 p;       /**< Position in the world. */
    float u;    /**< Pattern coordinate along the wall, metres. */
    float v;    /**< Pattern coordinate up the wall, metres. */
} r3d_vtx;

/* ---- Vector helpers ---- */

/**
 * @brief Make a vector.
 * @param x X component.
 * @param y Y component.
 * @param z Z component.
 * @return The vector (x, y, z).
 */
static inline v3 v3m(float x, float y, float z)
{
    v3 r = {x, y, z};
    return r;
}

/**
 * @brief Add two vectors.
 * @param a First vector.
 * @param b Second vector.
 * @return a + b.
 */
static inline v3 v3add(v3 a, v3 b)
{
    return v3m(a.x + b.x, a.y + b.y, a.z + b.z);
}

/**
 * @brief Subtract two vectors.
 * @param a First vector.
 * @param b Second vector.
 * @return a - b.
 */
static inline v3 v3sub(v3 a, v3 b)
{
    return v3m(a.x - b.x, a.y - b.y, a.z - b.z);
}

/**
 * @brief Scale a vector.
 * @param a The vector.
 * @param s The scale factor.
 * @return a * s.
 */
static inline v3 v3scale(v3 a, float s)
{
    return v3m(a.x * s, a.y * s, a.z * s);
}

/**
 * @brief Dot product.
 * @param a First vector.
 * @param b Second vector.
 * @return a . b.
 */
static inline float v3dot(v3 a, v3 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

/**
 * @brief Cross product.
 * @param a First vector.
 * @param b Second vector.
 * @return a x b.
 */
static inline v3 v3cross(v3 a, v3 b)
{
    return v3m(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}

/* ---- Memory ---- */

/**
 * @brief Memory the inner loops touch: internal RAM on the board (r3d.c has a
 *        plain malloc fallback, the board overrides it).
 * @param bytes Size to allocate.
 * @return The block, or NULL.
 */
void *kc_fast_alloc(unsigned bytes);

/**
 * @brief Memory read now and then (tables): PSRAM on the board.
 * @param bytes Size to allocate.
 * @return The block, or NULL.
 */
void *kc_slow_alloc(unsigned bytes);

/* ---- Colours ---- */

/**
 * @brief RGB565 helper: pack 8-bit channels, clamped to 0..255.
 * @param r Red, 0..255.
 * @param g Green, 0..255.
 * @param b Blue, 0..255.
 * @return The RGB565 colour.
 */
static inline uint16_t rgb(int r, int g, int b)
{
    if ( 0 > r )
    {
        r = 0;
    }
    if ( 255 < r )
    {
        r = 255;
    }
    if ( 0 > g )
    {
        g = 0;
    }
    if ( 255 < g )
    {
        g = 255;
    }
    if ( 0 > b )
    {
        b = 0;
    }
    if ( 255 < b )
    {
        b = 255;
    }
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

/**
 * @brief Mix two RGB565 colours.
 * @param a The colour at t = 0.
 * @param b The colour at t = 32.
 * @param t Mix amount, 0..32: 0 = a.
 * @return The mixed colour.
 */
uint16_t rgb_mix(uint16_t a, uint16_t b, int t /* 0..32: 0 = a */);

/* ---- The frame ---- */

/** The frame: GW*GH, set by the caller. */
extern uint16_t *r3d_fb;

/**
 * @brief Start a frame: clear the depth buffer and set the camera.
 * @param fb  The frame buffer, GW*GH.
 * @param cam The camera.
 */
void r3d_begin(uint16_t *fb, const r3d_cam *cam);

/**
 * @brief The camera of the current frame.
 * @return The camera.
 */
const r3d_cam *r3d_camera(void);

/* ---- Fog and light ---- */

/**
 * @brief Set the fog: the colour the far distance fades to.
 * @param fog The fog colour.
 */
void r3d_set_fog(uint16_t fog);

extern uint16_t r3d_fog;           /**< The fog colour. */
extern v3 r3d_sun;                 /**< Direction TO the sun, normalised */
extern int r3d_style;              /**< 0 neon, 1 real: the light model */

/* ---- Calls ---- */

/**
 * @brief Project a world point.
 * @param p     The world point.
 * @param sx    Out: screen x.
 * @param sy    Out: screen y.
 * @param depth Out: view depth (may be NULL).
 * @return false when behind the near plane.
 */
bool r3d_project(v3 p, float *sx, float *sy, float *depth);

/**
 * @brief Is a sphere possibly visible?
 * @param c      The centre.
 * @param radius The radius.
 * @return true when it may be on screen.
 */
bool r3d_visible(v3 c, float radius);

/**
 * @brief Draw a convex planar polygon (n <= 6) in one colour, lit by its normal.
 *        pattern: 0 plain, 1 windows (u,v in metres along the wall), 2 stripes,
 *        3 checker, 4 a facade: floors, framed windows, shop fronts.
 * @param v       The vertices.
 * @param n       The number of vertices.
 * @param color   The colour.
 * @param pattern The pattern.
 * @param color2  The second colour of the pattern.
 * @param lit     Light it by its normal.
 */
void r3d_poly(const r3d_vtx *v, int n, uint16_t color, int pattern, uint16_t color2, bool lit);

/**
 * @brief Same, with the shade already decided (no lighting).
 * @param p     The vertices.
 * @param n     The number of vertices.
 * @param color The colour.
 */
void r3d_poly_flat(const v3 *p, int n, uint16_t color);

/**
 * @brief A box with its base centred at c, sizes sx (x) sy (height) sz (z), turned
 *        by yaw about +y. top/side colours; pattern for the side walls.
 * @param c       The centre of the base.
 * @param sx      Size along x.
 * @param sy      Height.
 * @param sz      Size along z.
 * @param yaw     Turn about +y, radians.
 * @param side    The colour of the side walls.
 * @param top     The colour of the top.
 * @param pattern The pattern of the side walls.
 * @param color2  The second colour of the pattern.
 */
void r3d_box(v3 c, float sx, float sy, float sz, float yaw, uint16_t side, uint16_t top,
             int pattern, uint16_t color2);

/**
 * @brief A box whose top is shorter than its base along z (a car's cabin): the top
 *        runs from z = -ltop/2 + shift to +ltop/2 + shift.
 * @param c     The centre of the base.
 * @param sx    Size along x.
 * @param sy    Height.
 * @param lbot  Length of the base along z.
 * @param ltop  Length of the top along z.
 * @param shift Shift of the top along z.
 * @param yaw   Turn about +y, radians.
 * @param side  The colour of the side walls.
 * @param top   The colour of the top.
 */
void r3d_cabin(v3 c, float sx, float sy, float lbot, float ltop, float shift, float yaw,
               uint16_t side, uint16_t top);

/**
 * @brief A camera-facing quad (a sprite made of one colour), centre c, w x h metres.
 * @param c     The centre.
 * @param w     Width, metres.
 * @param h     Height, metres.
 * @param color The colour.
 */
void r3d_billboard(v3 c, float w, float h, uint16_t color);

/**
 * @brief A 3D line (tracers), drawn without depth test on top.
 * @param a     One end.
 * @param b     The other end.
 * @param color The colour.
 */
void r3d_line3(v3 a, v3 b, uint16_t color);

/* ---- 2D drawing straight into the frame (HUD) ---- */

/**
 * @brief Fill a rectangle.
 * @param x Left.
 * @param y Top.
 * @param w Width.
 * @param h Height.
 * @param c The colour.
 */
void r2d_rect(int x, int y, int w, int h, uint16_t c);

/**
 * @brief Blend a rectangle towards a colour.
 * @param x Left.
 * @param y Top.
 * @param w Width.
 * @param h Height.
 * @param c The colour.
 * @param t Mix amount, 0..32.
 */
void r2d_rect_blend(int x, int y, int w, int h, uint16_t c, int t);

/**
 * @brief Draw a line.
 * @param x0 Start x.
 * @param y0 Start y.
 * @param x1 End x.
 * @param y1 End y.
 * @param c  The colour.
 */
void r2d_line(int x0, int y0, int x1, int y1, uint16_t c);

/**
 * @brief Fill a disc.
 * @param cx Centre x.
 * @param cy Centre y.
 * @param r  Radius.
 * @param c  The colour.
 */
void r2d_disc(int cx, int cy, int r, uint16_t c);

/**
 * @brief Draw text in the 5x7 font.
 * @param x     Left.
 * @param y     Top.
 * @param scale Pixel scale.
 * @param c     The colour.
 * @param s     The text.
 */
void r2d_text(int x, int y, int scale, uint16_t c, const char *s);

/**
 * @brief Width of a text in pixels.
 * @param scale Pixel scale.
 * @param s     The text.
 * @return The width in pixels.
 */
int  r2d_text_w(int scale, const char *s);

/**
 * @brief Draw text with a black shadow.
 * @param x     Left.
 * @param y     Top.
 * @param scale Pixel scale.
 * @param c     The colour.
 * @param s     The text.
 */
void r2d_text_shadow(int x, int y, int scale, uint16_t c, const char *s);

/* ---- Stats ---- */

extern uint32_t r3d_tris, r3d_pixels;   /**< Triangles and pixels drawn this frame. */

#ifdef __cplusplus
}
#endif

#endif /* R3D_H_ */
