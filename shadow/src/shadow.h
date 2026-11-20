#ifndef SHADOW_H
#define SHADOW_H

#include <stddef.h>

typedef struct { float x, y, radius, intensity; } ShLight;
typedef struct { float x0, y0, x1, y1; } ShSeg;
typedef struct { int w, h; float *buf; } ShadowMap;

/* Legacy single-light API.  sh_render() is unchanged. */
int sh_init(ShadowMap *m, int w, int h);
void sh_free(ShadowMap *m);
int sh_clear(ShadowMap *m);
int sh_render(ShadowMap *m, const ShLight *light, const ShSeg *segs, int nseg);
int sh_write_pgm(const ShadowMap *m, const char *path);

/* ---- Multi-light API ---------------------------------------------------- */

enum {
    SH_OK        =  0,
    SH_EARG      = -1,  /* bad pointer, bad size, bad segment array          */
    SH_EBADLIGHT = -2,  /* radius <= 0, intensity < 0, or non-finite value;
                           err->light is the offending light index           */
    SH_ENOLIGHT  = -3,  /* nlight == 0; there is no index to report           */
    SH_EDUP      = -4   /* two lights share the same (x, y); err->light is the
                           later index, err->other the earlier one           */
};

typedef struct {
    int  code;       /* SH_OK or one of the negative codes above             */
    int  light;      /* offending light index, -1 when not applicable        */
    int  other;      /* partner index for SH_EDUP, -1 otherwise              */
    char msg[128];   /* human-readable text; names the index as "#<n>"       */
} ShError;

typedef struct {
    int    passes;       /* stage-1 light passes: nlight, or 1 for legacy   */
    size_t temp_pixels;  /* floats written to temporary maps (nlight*w*h)   */
    size_t blend_ops;    /* stage-2 per-light per-pixel adds (nlight*w*h)   */
} ShStats;

/*
 * Two-stage multi-light render.
 *
 * enable_multi == 0:
 *     Legacy compatibility path.  Only lights[0] is rendered, through
 *     sh_render(), so the buffer is bit-identical to the old API.  All
 *     lights are still validated.
 *
 * enable_multi != 0:
 *     Stage 1: for each light i, render it alone into temporary map t_i
 *              (exactly the legacy per-light radiance I_i * fall_i * vis_i).
 *     Stage 2: for each pixel, out = (sum_i t_i) / (sum_i I_i), with the
 *              sum taken in ascending light index order (0, 1, 2, ...).
 *              If sum_i I_i == 0, out = 0.
 *
 *     Intensity therefore acts as a relative weight in multi-light mode.
 *     The result stays in [0, 1] and both lights contribute; the legacy
 *     absolute-intensity behaviour is available through enable_multi == 0.
 *
 * err and stats may be NULL.  Every failure also fills err (when non-NULL)
 * with the offending light index.
 */
int sh_render_lights(ShadowMap *m, const ShLight *lights, int nlight,
                     const ShSeg *segs, int nseg, int enable_multi,
                     ShError *err, ShStats *stats);

#endif
