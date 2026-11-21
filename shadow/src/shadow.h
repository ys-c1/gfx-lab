#ifndef SHADOW_H
#define SHADOW_H

/* ---- legacy grayscale API (unchanged; tint switch off == old behavior) ---- */
typedef struct { float x, y, radius, intensity; } ShLight;
typedef struct { float x0, y0, x1, y1; } ShSeg;
typedef struct { int w, h; float *buf; } ShadowMap;

int sh_init(ShadowMap *m, int w, int h);
void sh_free(ShadowMap *m);
int sh_clear(ShadowMap *m);
int sh_render(ShadowMap *m, const ShLight *light, const ShSeg *segs, int nseg);
int sh_write_pgm(const ShadowMap *m, const char *path);

/* ---- tinted shadow pipeline ----
 *
 * Two stages, exactly as required:
 *   stage 1  shadow base tint derived from the LIGHT COLOR
 *            base[c] = light.rgb[c] * (1 - shadow_strength)
 *   stage 2  mix into the frame by per-pixel OCCLUSION RATIO o in [0,1]
 *            out[c] = frame[c] * (1 - o) + base[c] * o
 *
 * When the tint switch is disabled (ShTune.enable == 0) every new entry
 * point falls back to the legacy gray path and the frame pixels are not
 * touched, so existing behavior is byte-for-byte preserved.
 */
typedef struct { unsigned char r, g, b; } ShColor;

typedef struct {
    int   enable;        /* tint switch: 0 = legacy behavior unchanged       */
    ShColor color;       /* light color, used when color_source == SH_SRC_RGB */
    int   color_source;  /* one of ShColorSource                              */
    int   preset;        /* one of ShPreset,      used when == SH_SRC_PRESET */
    int   swatch_id;     /* swatch card index,    used when == SH_SRC_SWATCH */
    float shadow_strength; /* 0..1, how dark the fully occluded base is      */
    float penumbra;      /* soft-edge width in px; 0 = hard legacy shadow     */
} ShTune;

enum ShColorSource { SH_SRC_RGB = 0, SH_SRC_PRESET = 1, SH_SRC_SWATCH = 2 };
/* SH_PRESET_NONE == 0 so a zero-initialized ShTune never looks "preset set". */
enum ShPreset { SH_PRESET_NONE = 0, SH_PRESET_WARM = 1, SH_PRESET_COOL = 2,
                SH_PRESET_GOLDEN = 3, SH_PRESET_DUSK = 4, SH_PRESET_COUNT = 5 };

/* RGB framebuffer the shadow is composited onto. */
typedef struct { int w, h; unsigned char *rgb; } ShFrame;
int sh_frame_init(ShFrame *f, int w, int h);
void sh_frame_free(ShFrame *f);
int sh_frame_fill(ShFrame *f, unsigned char r, unsigned char g, unsigned char b);

/* Resolve the effective light color for a tune. Preset/swatch lookups never
 * mutate *t; the result is written to *out. Returns negative on error. */
int sh_resolve_color(const ShTune *t, ShColor *out);

/* Validate a tune. On conflict returns SH_ECONFLICT and, when both strings
 * are non-NULL, writes the names of the two conflicting sides, e.g.
 * "color.rgb" and "preset". */
int sh_tune_check(const ShTune *t, char *side_a, int cap_a, char *side_b, int cap_b);

/* Run the two-stage pipeline over an existing frame.
 * Slow gear (gear=0): exact per-pixel, per-segment brute force.
 * Fast gear (gear=1): each segment marks the coarse tiles covered by its
 * exact shadow wedge (the infinite cone behind the segment from the light),
 * and a pixel ray-tests only the segments marking its tile. The wedge test
 * is conservative, so the same occlusion math yields identical output. */
int sh_render_tinted(ShFrame *frame, const ShLight *light,
                     const ShSeg *segs, int nseg,
                     const ShTune *tune, int gear);

/* Per-pixel occlusion ratio in [0,1] (1 = fully blocked). Exact brute force. */
int sh_occlusion(const ShadowMap *m, const ShLight *light,
                 const ShSeg *segs, int nseg, float penumbra, float *out);

int sh_write_ppm_f(const ShFrame *f, const char *path);

/* ---- color cards (swatches) ---- */
int sh_swatch_count(void);
int sh_swatch_get(int id, ShColor *out, char *name, int name_cap);

/* ---- benchmark / verification ---- */
typedef struct {
    int   w, h;
    int   nseg;
    int   gear;                 /* 0 slow, 1 fast                          */
    long  pixels;               /* pixels processed                        */
    long  shadow_pixels;        /* pixels with occlusion > 0               */
    double time_ms;             /* wall-clock render time                  */
    double mean_occlusion;      /* mean occlusion ratio                    */
    double mean_mix;            /* mean actual blend factor applied        */
    double tint_err;            /* mean |rendered - expected| per channel  */
    double max_tint_err;        /* max per-channel absolute error (0..255) */
    double recolor_de;          /* mean CIE76 delta E, recolored vs gray   */
    double recolor_de_max;
} ShBench;

/* Deterministic benchmark scene: nseg random segments, fixed seed.
 * sh_bench_run fills *segs with this same generator, so tests can rebuild
 * the exact scene and run an independent cross-check against it. */
int sh_test_scene(int w, int h, int nseg, ShSeg *segs);

/* Deterministic benchmark run: nseg random segments, fixed seed. */
int sh_bench_run(ShFrame *frame, const ShLight *light, int nseg,
                 const ShTune *tune, int gear, ShBench *out);

/* Exhaustive reference: pure brute-force occlusion recomputed independently
 * of the fast path, used to cross-check gear output pixel by pixel.
 * bg[3] is the backdrop that was present before the shadow was composited. */
int sh_bench_verify_exhaustive(const ShFrame *frame, const ShLight *light,
                               const ShSeg *segs, int nseg,
                               const ShTune *tune, const unsigned char bg[3],
                               long *mismatch, double *max_channel_diff);

/* Error codes */
#define SH_OK          0
#define SH_EINVAL     -1
#define SH_ERANGE     -2
#define SH_ECONFLICT  -3   /* mutually exclusive color configs */
#define SH_ENOSWATCH  -4

const char *sh_strerror(int code);
#endif
