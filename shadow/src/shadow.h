#ifndef SHADOW_H
#define SHADOW_H

typedef struct { float x, y, radius, intensity; } ShLight;
typedef struct { float x0, y0, x1, y1; } ShSeg;
typedef struct { int w, h; float *buf; } ShadowMap;

int sh_init(ShadowMap *m, int w, int h);
void sh_free(ShadowMap *m);
int sh_clear(ShadowMap *m);
int sh_render(ShadowMap *m, const ShLight *light, const ShSeg *segs, int nseg);
int sh_write_pgm(const ShadowMap *m, const char *path);

/*
 * Animated lights / incremental shadow maps
 * -----------------------------------------
 * A frame is produced in two stages:
 *   1. interpolate the light between the previous and the next key frame,
 *   2. update only the blocks whose cached value or cached visibility can
 *      have changed.
 *
 * Turning interpolation off makes sh_anim_step() take the "next" light
 * verbatim and do a full sh_render(), i.e. exactly the old behaviour.
 *
 * Visibility is cached as a per-pixel mask.  A block is recomputed (mask and
 * value) only if the light's motion can carry a pixel across a blocker
 * endpoint; otherwise a stale value is re-shaded from the cached mask, and
 * if even that is within `eps` the block is left untouched.
 */

/* Return codes of the interpolation / animation API. */
enum {
    SH_OK             =  0,
    SH_ERR_NULL       = -1,  /* a required pointer was NULL           */
    SH_ERR_T_RANGE    = -2,  /* interpolation coefficient not in [0,1] */
    SH_ERR_FRAME_SPAN = -3,  /* frame interval <= 0                    */
    SH_ERR_LIGHT      = -4,  /* radius/intensity/NaN invalid           */
    SH_ERR_MEM        = -5,  /* allocation failure                     */
    SH_ERR_IO         = -6,  /* file could not be written              */
    SH_ERR_ARG        = -7   /* other invalid argument                 */
};

/* Diagnostic for the animated API.  On failure `param` names the offending
 * argument ("t", "frame_span", "prev", "next", ...) and `msg` says why. */
typedef struct {
    int code;
    const char *param;
    char msg[192];
} ShError;

/* Per-step statistics of the incremental renderer. */
typedef struct {
    int frame;          /* zero-based step index                      */
    int interpolated;   /* 1 when the light position was interpolated */
    int full_refresh;   /* 1 when the whole map was re-rendered       */
    int dirty_blocks;   /* blocks written this step (any kind)        */
    int recompute_blocks;/* blocks whose occlusion was recomputed     */
    int refade_blocks;  /* occlusion unchanged, falloff re-evaluated  */
    int skip_blocks;    /* untouched: value bound within tolerance    */
    int total_blocks;   /* blocks in the map                          */
    long dirty_pixels;  /* pixels written this step (either kind)     */
    long recompute_pixels;
    long refade_pixels;
    long total_pixels;  /* w * h                                      */
    float max_err_bound;/* largest accumulated per-block error bound  */
} ShStepStats;

/* Incremental shadow map state.  Public so callers can read `map`/`light`;
 * treat the remaining fields as opaque. */
typedef struct {
    ShadowMap map;
    ShSeg *segs;
    int nseg;
    int block;              /* block edge in pixels                  */
    int bw, bh;             /* blocks along x / y                    */
    ShLight light;          /* light of the last completed step      */
    int have_frame;         /* map contains a rendered frame         */
    int frame;              /* number of completed steps             */
    unsigned char *dirty;   /* 0 skip, 1 refade, 2 recomputed        */
    unsigned char *cand;    /* blocks a visibility flip can reach    */
    float *err_bound;       /* per-block accumulated error bound     */
    unsigned char *occ;     /* 1 = pixel occluded at `light`         */
    float eps;              /* maximum tolerated value error         */
} ShAnim;

/* Default tolerance: one 8-bit grey level. */
#define SH_EPS_DEFAULT (1.0f / 255.0f)

int sh_anim_init(ShAnim *a, int w, int h, int block);
void sh_anim_free(ShAnim *a);
int sh_anim_set_scene(ShAnim *a, const ShSeg *segs, int nseg);
int sh_anim_set_eps(ShAnim *a, float eps);

/*
 * Produce one frame.
 *   prev/next   key-frame lights (prev may be NULL when interpolate == 0)
 *   t           interpolation coefficient, must be finite and in [0,1]
 *   frame_span  frames between the key frames, must be > 0
 *   interpolate 0 -> use *next and full render (legacy behaviour)
 *               1 -> interpolate, then update only dirty blocks
 * Returns SH_OK or one of the SH_ERR_* codes and fills `err` (optional).
 */
int sh_anim_step(ShAnim *a, const ShLight *prev, const ShLight *next, float t,
                 int frame_span, int interpolate, ShStepStats *stats, ShError *err);

/* Linear light interpolation.  t == 0/1 return prev/next bit-exactly. */
int sh_lerp_light(const ShLight *prev, const ShLight *next, float t, int frame_span,
                  ShLight *out, ShError *err);

/* Write the interpolated light path as CSV:
 * frame,t,x,y,radius,intensity.  `frames` samples include both endpoints. */
int sh_write_trajectory(const char *path, const ShLight *prev, const ShLight *next,
                        int frames, int frame_span, ShError *err);

/* Compare two maps of equal size: max absolute error, mean absolute error,
 * root-mean-square error.  Output pointers may be NULL. */
int sh_compare_maps(const ShadowMap *a, const ShadowMap *b,
                    double *max_err, double *mean_err, double *rmse);

#endif
