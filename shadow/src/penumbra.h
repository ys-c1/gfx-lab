#ifndef PENUMBRA_H
#define PENUMBRA_H

#include <stddef.h>
#include "shadow.h"

/*
 * Curve-weighted penumbra
 * =======================
 *
 * The hard renderer (sh_render) produces a binary silhouette: a receiver is
 * either fully lit or fully dark.  Giving that edge a fixed linear ramp makes
 * it read as a sticker pasted on the image, because the true edge profile of
 * a disk-shaped light is not linear.
 *
 * This module splits soft-shadow rendering into two stages:
 *
 *   stage 1  sh_pen_weights()  distance -> curve weight
 *             For every pixel, measure the signed pixel distance d to the
 *             hard shadow boundary, scale it by the local penumbra half width
 *             h (from the light source radius and the blocker's distance), and
 *             evaluate the curve LUT at x = d / h.  The result is a per-pixel
 *             light weight in [0, 1].
 *
 *   stage 2  sh_pen_blend()    weight -> mix light and dark
 *             out = dark + (light - dark) * weight.  The light and dark values
 *             are the same ones the hard renderer would write; `dark` is 0,
 *             matching the legacy umbra, so the blend reduces to light*weight.
 *
 * With the feature disabled (cfg == NULL or cfg->enable == 0) sh_render_pen()
 * forwards to the untouched sh_render(), so the map -- and any PGM written
 * from it -- is byte-for-byte identical to the pre-feature renderer.
 *
 * Curve presets (ShPenCfg.curve).  x is the normalized edge coordinate in
 * [-1, 1]: +1 fully lit, 0 on the hard boundary, -1 fully shadowed.
 *
 *   SH_CURVE_LINEAR  w = (x + 1) / 2                     the naive ramp
 *   SH_CURVE_SMOOTH  w = smoothstep((x + 1) / 2)         ease in/out
 *   SH_CURVE_DISK    w = area of a unit disk on one side of a line at x
 *                       = 0.5 + (x*sqrt(1-x^2) + asin x) / pi
 *                    This is the exact visible fraction of a disk light
 *                    occluded by a straight edge, so it matches the physical
 *                    per-pixel reference to within geometric approximations.
 *   SH_CURVE_POW     w = ((x + 1) / 2) ^ curve_exp
 *
 * The curve is evaluated through a LUT of `curve_levels` entries sampled
 * uniformly over x in [-1, 1]; `curve_levels` is the reported "curve 档位".
 * The LUT used by the pipeline is the one returned by sh_pen_curve_table(),
 * so callers can reproduce or export exactly the weights that were applied.
 */

#define SH_PEN_MAX_LEVELS 4096

/* Curve presets. */
enum {
    SH_CURVE_LINEAR = 0,
    SH_CURVE_SMOOTH = 1,
    SH_CURVE_DISK   = 2,
    SH_CURVE_POW    = 3,
    SH_CURVE_COUNT
};

/* Return codes.  SH_PEN_OK is 0; every failure is negative. */
enum {
    SH_PEN_OK        =  0,
    SH_PEN_ERR_NULL  = -1,  /* required pointer is NULL                     */
    SH_PEN_ERR_LIGHT = -2,  /* light.radius <= 0 or intensity < 0           */
    SH_PEN_ERR_TYPE  = -3,  /* declared value type does not match the field */
    SH_PEN_ERR_FIELD = -4,  /* unknown field id                             */
    SH_PEN_ERR_RANGE = -5,  /* value outside the field's allowed range      */
    SH_PEN_ERR_MEM   = -6,  /* allocation failure                           */
    SH_PEN_ERR_IO    = -7,  /* table export could not be written            */
    SH_PEN_ERR_STATE = -8   /* operation invalid in the current state       */
};

/* Declared value type for sh_pen_cfg_set().  C has no runtime types, so the
 * caller states the type of the value it passes; a mismatch is rejected with
 * SH_PEN_ERR_TYPE and err->field names the field. */
typedef enum {
    SH_TY_NONE  = 0,
    SH_TY_INT   = 1,
    SH_TY_FLOAT = 2
} ShType;

/* Every configuration field has exactly one type. */
typedef enum {
    SH_F_ENABLE       = 0,  /* int   : 0 = legacy path, byte-identical     */
    SH_F_SRC_RADIUS   = 1,  /* float : area-light radius, world units >= 0  */
    SH_F_WIDTH_SCALE  = 2,  /* float : penumbra width multiplier, > 0       */
    SH_F_CURVE        = 3,  /* int   : SH_CURVE_*                            */
    SH_F_CURVE_EXP    = 4,  /* float : exponent for SH_CURVE_POW, > 0        */
    SH_F_CURVE_LEVELS = 5,  /* int   : LUT entries, 2..SH_PEN_MAX_LEVELS     */
    SH_F_COUNT        = 6
} ShField;

/* Diagnostic.  On failure `field` points at a static field name for errors
 * tied to a field, and is NULL otherwise.  `msg` always explains the problem. */
typedef struct {
    int code;
    const char *field;
    char msg[192];
} ShError;

typedef struct {
    int   enable;        /* 0 = bypass, byte-identical legacy output        */
    float src_radius;    /* disk light radius in world units (pixels here)  */
    float width_scale;   /* multiplier on the analytic penumbra width       */
    int   curve;         /* SH_CURVE_*                                      */
    float curve_exp;     /* exponent for SH_CURVE_POW                       */
    int   curve_levels;  /* LUT size ("curve 档位")                          */
} ShPenCfg;

/* Measurements of one weights/blend/render call.  Counts are reported so a
 * caller can see how much of the map the penumbra actually covers. */
typedef struct {
    long   pixels;             /* w * h                                   */
    long   lit_pixels;         /* weight == 1                             */
    long   umbra_pixels;       /* weight == 0                             */
    long   penumbra_pixels;    /* 0 < weight < 1 ("像素数")               */
    int    curve;              /* preset id                               */
    int    curve_levels;       /* LUT entries                             */
    long   curve_levels_used;  /* distinct LUT indices hit in the field   */
    size_t bytes;              /* peak bytes allocated by the pipeline    */
    double width_px;           /* measured 10-90 penumbra width, pixels   */
    double width_expected_px;  /* analytic 10-90 width at the same spot   */
    double width_err_px;       /* |measured - expected|                   */
} ShPenStats;

/* Field introspection.  sh_pen_field_name returns NULL for an unknown id;
 * sh_pen_field_type returns SH_TY_NONE for an unknown id. */
const char *sh_pen_field_name(ShField f);
ShType      sh_pen_field_type(ShField f);

/* Defaults: enable = 0, src_radius = 6, width_scale = 1, curve = DISK,
 * curve_exp = 2, curve_levels = 256. */
void sh_pen_cfg_default(ShPenCfg *cfg);

/*
 * Set one field.  `ty` must equal sh_pen_field_type(f) and `value` must point
 * at an object of that type:
 *     SH_TY_INT   -> const int *
 *     SH_TY_FLOAT -> const float *
 * Errors, all with err->field naming the field:
 *     NULL cfg/value        SH_PEN_ERR_NULL
 *     unknown field         SH_PEN_ERR_FIELD
 *     wrong declared type   SH_PEN_ERR_TYPE
 *     value out of range    SH_PEN_ERR_RANGE
 * Ranges checked here: enable 0/1, src_radius finite >= 0, width_scale finite
 * > 0, curve in [0, SH_CURVE_COUNT), curve_exp finite > 0, curve_levels in
 * [2, SH_PEN_MAX_LEVELS].  A disabled config skips cross-field checks.
 */
int sh_pen_cfg_set(ShPenCfg *cfg, ShField f, ShType ty, const void *value,
                   ShError *err);

/* Validate the whole config.  Disabled configs are always valid.  Enabled
 * configs require src_radius > 0.  On failure err->field names the field. */
int sh_pen_cfg_validate(const ShPenCfg *cfg, ShError *err);

/* Continuous curve value at x in [-1, 1], clamped to [0, 1].  Values of x
 * outside the band saturate.  NaN yields 0.5. */
float sh_pen_curve(int curve, float curve_exp, float x);

/* Inverse of sh_pen_curve: the x in [-1, 1] where the curve reaches `level`.
 * The curves are all monotone, so bisection is exact to float precision.
 * Returns 0 for a non-finite level. */
float sh_pen_curve_cross(int curve, float curve_exp, float level);

/* Fill out[0..levels-1] with the LUT over x_i = -1 + 2i/(levels-1).
 * `cap` is the capacity of `out`.  Returns the entry count, or a negative
 * SH_PEN_ERR_* code.  The pipeline evaluates exactly this table (with linear
 * interpolation between entries), so exported tables reproduce the weights. */
int sh_pen_curve_table(int curve, float curve_exp, int levels, float *out, int cap);

/* Write the LUT as TSV: index, x, weight, plus a header with the parameters.
 * Returns 0 or SH_PEN_ERR_IO / SH_PEN_ERR_RANGE. */
int sh_pen_curve_export(const char *path, int curve, float curve_exp, int levels);

/* Analytic penumbra half width (pixels) at receiver point (px, py):
 *     src_radius * width_scale * (D_rec - D_blocker) / D_blocker
 * where D_rec is the light-to-receiver distance and D_blocker the light-to-
 * nearest-point-on-any-segment distance.  Zero when there is no blocker or the
 * receiver is not farther than it. */
float sh_pen_halfwidth(const ShLight *light, const ShSeg *segs, int nseg,
                       float src_radius, float width_scale, float px, float py);

/*
 * Stage 1, one pixel.  Measures the signed edge coordinate x = d / h (signed
 * pixel distance to the hard boundary over the local penumbra half width),
 * clamps it to [-1, 1], and evaluates the curve.  This is the per-pixel step
 * the field renderer repeats; a test can reproduce any pixel's weight from the
 * exported curve table by feeding the returned x through the table.  x_out
 * (optional) receives the clamped coordinate.  A pixel with no penumbra band
 * (h == 0) gets x = +1 when lit and x = -1 when shadowed.
 */
int sh_pen_weight_pixel(const ShLight *light, const ShSeg *segs, int nseg,
                        const ShPenCfg *cfg, int px, int py, float *weight,
                        float *x_out);

/*
 * Stage 1: distance -> curve weight.
 * `weight` must hold m->w * m->h floats.  Each entry receives the light weight
 * in [0, 1].  When the config is disabled the weights are the hard mask (1 for
 * lit, 0 for shadowed).  stats may be NULL; when non-NULL the pixel counts,
 * curve levels, peak bytes and a measured penumbra width are filled in.
 */
int sh_pen_weights(ShadowMap *m, const ShLight *light, const ShSeg *segs, int nseg,
                   const ShPenCfg *cfg, float *weight, ShPenStats *stats,
                   ShError *err);

/*
 * Stage 2: weight -> mix light and dark.
 * Rewrites m->buf as dark + (light - dark) * weight, where `light` is the
 * hard renderer's falloff value and `dark` is 0.  weight must have one entry
 * per pixel.  stats, when non-NULL, receives the pixel counts.
 */
int sh_pen_blend(ShadowMap *m, const ShLight *light, const float *weight,
                 ShPenStats *stats);

/*
 * Both stages.  When cfg is NULL or disabled, forwards to sh_render() and the
 * buffer is byte-identical to the legacy renderer.  Otherwise stage 1 then
 * stage 2.  stats/err may be NULL.
 */
int sh_render_pen(ShadowMap *m, const ShLight *light, const ShSeg *segs, int nseg,
                  const ShPenCfg *cfg, ShPenStats *stats, ShError *err);

/*
 * Per-pixel reference: no curve, no LUT, no distance transform.  For every
 * pixel it samples the disk light (deterministic sunflower, `samples` points)
 * with the library's exact ray/segment blocker test, and writes the visible
 * fraction times the same falloff the hard renderer uses.  This is the ground
 * truth the fast path is measured against in shadow/bench.c.
 * Requires src_radius >= 0 and samples > 0.
 */
int sh_render_pen_ref(ShadowMap *m, const ShLight *light, const ShSeg *segs,
                      int nseg, float src_radius, int samples, ShError *err);

/*
 * Measure the 10%-90% penumbra width along one row of a weight field.  The
 * transition band is located by linear interpolation between pixel centres at
 * the first downward 0.9 crossing and the next 0.1 crossing (a rising edge is
 * treated the same way).  The two crossing x coordinates (pixel-centre
 * coordinates) are written to x_hi/x_lo when non-NULL, and the width to
 * *width_px.  Returns SH_PEN_OK, or SH_PEN_ERR_STATE when the row contains no
 * complete transition.
 */
int sh_pen_measure_width(const float *weight, int w, int h, int row,
                         double *width_px, double *x_hi, double *x_lo);

/*
 * Analytic 10%-90% width in pixels for one edge, given the local penumbra half
 * width h: h * (x(0.9) - x(0.1)), where x(level) is sh_pen_curve_cross().
 * This is the value the measured width is compared against.
 */
double sh_pen_expected_width(int curve, float curve_exp, float halfwidth_px);

/* Signed edge coordinate x = d/h at one pixel, using the same geometry and
 * half width as the weight stage.  *x_out is clamped to [-1, 1]; lit side is
 * positive.  Provided for hand checks and reference tables. */
int sh_pen_edge_coord(const ShLight *light, const ShSeg *segs, int nseg,
                      const ShPenCfg *cfg, int px, int py, float *x_out);

/* Compare two equally sized maps: maximum absolute difference and RMS. */
int sh_pen_compare(const ShadowMap *a, const ShadowMap *b,
                   double *max_abs, double *rms);

#endif /* PENUMBRA_H */
