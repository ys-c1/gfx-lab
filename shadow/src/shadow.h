#ifndef SHADOW_H
#define SHADOW_H

#include <stddef.h>

/* Legacy single-light API.  Behaviour is unchanged. */
typedef struct { float x, y, radius, intensity; } ShLight;
typedef struct { float x0, y0, x1, y1; } ShSeg;
typedef struct { int w, h; float *buf; } ShadowMap;

int sh_init(ShadowMap *m, int w, int h);
void sh_free(ShadowMap *m);
int sh_clear(ShadowMap *m);
int sh_render(ShadowMap *m, const ShLight *light, const ShSeg *segs, int nseg);
int sh_write_pgm(const ShadowMap *m, const char *path);

/* -------------------------------------------------------------------------
 * Multi-light API
 *
 * Two stages:
 *   1. order the lights by intensity (brightest first),
 *   2. accumulate each light's shadow contribution in that order.
 *
 * The order is derived from the light *values*, never from the caller's array
 * order, so permuting the input does not change the output by a single bit.
 * Ties are broken by x, then y, then radius, then (only for lights with equal
 * values in every field) the input index; equal-valued lights contribute
 * identical values, so that last tie-break cannot change the sum either.
 *
 * Passing enable_multi == 0 delegates to sh_render(lights[0]) and therefore
 * reproduces the legacy single-light output byte for byte.
 * ------------------------------------------------------------------------- */

#define SH_MAX_LIGHTS 64

/* Return codes of the multi-light API.  sh_render() keeps its own -1/-2. */
enum {
    SH_OK        =  0,
    SH_ERR_NULL  = -1,  /* a required pointer was NULL                     */
    SH_ERR_ARG   = -2,  /* nlight / nseg / map out of range                */
    SH_ERR_NOLIGHT = -3,/* nlight == 0                                     */
    SH_ERR_LIGHT = -4,  /* a light field is not usable (field is named)    */
    SH_ERR_SEG   = -5,  /* a segment field is not usable (field is named)  */
    SH_ERR_IO    = -6   /* a file could not be written                     */
};

/* Diagnostic.  `field` always names the offending member, including the array
 * index, e.g. "lights[2].radius", "segs[1].x1", "nlight", "m", "path".
 *
 * C has no run-time type information: a struct field already has a static
 * type.  "Type mismatch" here means a value that does not fit the field's
 * declared float domain (NaN, +/-Inf, radius <= 0, intensity < 0), or a
 * pointer/size argument of the wrong shape; every such rejection names the
 * field. */
typedef struct {
    int code;
    int light;          /* offending light index, -1 when not applicable */
    char field[40];
    char msg[160];
} ShError;

/* One row of the sorted light table. */
typedef struct {
    int index;          /* index in the caller's lights[] array */
    float intensity;    /* value used for ordering              */
    float x, y, radius; /* remaining fields, for the export     */
} ShLightOrder;

/* Per-render statistics.  All sizes are bytes, all counts are exact. */
typedef struct {
    int lights;         /* lights actually shaded                      */
    int passes;         /* accumulation passes (== lights)             */
    int sorted;         /* 1 when the sorted order differs from input  */
    ShLightOrder order[SH_MAX_LIGHTS];
    unsigned long long pixels;        /* w * h                           */
    unsigned long long accumulate_ops;/* pixels * passes                 */
    size_t map_bytes;         /* w*h*sizeof(float)                     */
    size_t order_bytes;       /* lights * sizeof(ShLightOrder)         */
    size_t total_bytes;       /* map_bytes + order_bytes               */
    size_t peak_extra_bytes;  /* order table only; no temporary maps   */
    size_t avoided_temp_bytes;/* lights*map_bytes not allocated        */
    int temp_maps;            /* always 0: streaming accumulation      */
} ShMultiStats;

/* Stage 1, exposed on its own: validate the lights and fill `order` (which
 * must have room for nlight rows) with the deterministic order described
 * above.  Returns SH_OK or a SH_ERR_* code and fills err (optional). */
int sh_lights_order(const ShLight *lights, int nlight,
                    ShLightOrder *order, ShError *err);

/* Export an order table as CSV:
 *   # scene=<scene> w=<w> h=<h> passes=<n>
 *   rank,index,intensity,x,y,radius
 * Returns SH_OK, SH_ERR_NULL/SH_ERR_ARG, or SH_ERR_IO. */
int sh_write_order_table(const char *path, const ShLightOrder *order, int n,
                         const char *scene, int w, int h);

/* Render one or more lights.
 *
 *   enable_multi == 0: validate the arguments and call sh_render(lights[0]);
 *                      the buffer is byte-identical to the legacy API.
 *   enable_multi != 0: sort by intensity, then add each light's contribution
 *                      into a zeroed buffer in sorted order.  Values are the
 *                      raw sum (they may exceed 1; sh_write_pgm clamps).
 *
 * stats and err may be NULL.  On failure the map is left untouched. */
int sh_render_lights(ShadowMap *m, const ShLight *lights, int nlight,
                     const ShSeg *segs, int nseg, int enable_multi,
                     ShMultiStats *stats, ShError *err);

#endif
