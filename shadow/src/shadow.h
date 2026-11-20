#ifndef SHADOW_H
#define SHADOW_H

typedef struct { float x, y, radius, intensity; } ShLight;
typedef struct { float x0, y0, x1, y1; } ShSeg;

/* Bad input reported by a call.  field names the offending input, e.g.
 * "segs[3].y1", "bias" or "slope_clamp".  It is empty when code == 0 and is
 * always NUL-terminated. */
typedef struct {
    int code;      /* 0 on success, negative on failure */
    char field[40];
} ShError;

/* Self-shadow (acne) report, measured in pixels.  A surface pixel is the
 * pixel that holds the segment at each step along the segment's dominant
 * axis.  A stripe is a maximal run of consecutive surface pixels that the
 * segment shadows with its own shadow volume. */
typedef struct {
    int checked;   /* surface pixels sampled */
    int shadowed;  /* of those, self-shadowed */
    int stripes;   /* number of runs of self-shadowed pixels */
    int longest;   /* length of the longest run, in pixels */
} ShAcne;

typedef struct {
    int w, h;
    float *buf;
    /* World units per pixel; the map spans (0,0)-(w*scale,h*scale). */
    float scale;
    /* Slope-scaled offset.  offset_world = bias * min(slope, slope_clamp)
     * * scale, so bias is the offset in pixels per unit of surface slope and
     * is unchanged between resolutions of the same world.  slope is |dy/dx|
     * of the occluding segment.  bias == 0 disables the offset. */
    float bias;
    float slope_clamp;
} ShadowMap;

#define SH_DEFAULT_SCALE 1.0f
#define SH_DEFAULT_BIAS 4.0f
#define SH_DEFAULT_SLOPE_CLAMP 1.0f

/* Negative return codes common to the functions below. */
#define SH_ERR_ARG   (-1)  /* null argument, bad size, or allocation failure */
#define SH_ERR_LIGHT (-2)  /* light position, radius or intensity is bad */
#define SH_ERR_FIELD (-3)  /* a segment field is bad; see ShError.field */

int sh_init(ShadowMap *m, int w, int h);
/* Same, with an explicit world-units-per-pixel scale.  128x128 at scale 1
 * and 512x512 at scale 0.25 both cover a 128x128 world. */
int sh_init_scaled(ShadowMap *m, int w, int h, float scale);
void sh_free(ShadowMap *m);
int sh_clear(ShadowMap *m);

/* Stage 1, configuration.  bias is in pixels per unit slope and must be
 * finite and >= 0; slope_clamp must be finite and > 0.  A negative or
 * non-finite bias is an error named "bias" in err->field; a bad clamp is
 * named "slope_clamp". */
int sh_set_bias(ShadowMap *m, float bias, float slope_clamp, ShError *err);

/* Stage 1, per segment.  Surface slope |dy/dx| of segment index.  A segment
 * with a vertical direction or with a non-finite endpoint has no finite
 * slope; that is an error and err->field names the input. */
int sh_seg_slope(const ShSeg *segs, int index, float *slope, ShError *err);

/* Stage 1, per segment.  The world-unit offset applied for segment index:
 * bias * min(slope, slope_clamp) * scale.  Zero for a horizontal segment
 * and when the bias is zero. */
float sh_seg_offset(const ShadowMap *m, const ShSeg *segs, int index);

/* Stage 2, rendering.  Each occluding segment's offset enters its shadow
 * intersection: a hit closer to the receiving pixel than that offset does
 * not block.  With bias == 0 the output is bit-identical to the renderer
 * without offsets (see shadow/test.c).  sh_render_ex reports the bad field
 * of an invalid segment in err; sh_render returns only SH_ERR_FIELD. */
int sh_render(ShadowMap *m, const ShLight *light, const ShSeg *segs, int nseg);
int sh_render_ex(ShadowMap *m, const ShLight *light, const ShSeg *segs,
                 int nseg, ShError *err);

/* Stage 2, one pixel: 1 when blocked, 0 when lit, negative on bad input.
 * Uses the same rule as sh_render, for hand checks. */
int sh_pixel_blocked(const ShadowMap *m, const ShLight *light,
                     const ShSeg *segs, int nseg, int x, int y);

/* Reference for stage 1: the offset in world units above which no surface
 * pixel of segment index is self-shadowed, i.e. one more than the largest
 * ray distance to a self-hit.  Zero when the segment never self-shadows. */
int sh_seg_needed_offset(const ShadowMap *m, const ShLight *light,
                         const ShSeg *segs, int index, float *needed,
                         ShError *err);

/* Stage 3, statistics.  Count self-shadow stripes on an already rendered
 * map. */
int sh_acne(const ShadowMap *m, const ShLight *light, const ShSeg *segs,
            int nseg, ShAcne *out, ShError *err);

int sh_write_pgm(const ShadowMap *m, const char *path);
#endif
