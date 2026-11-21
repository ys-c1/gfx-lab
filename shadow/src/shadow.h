#ifndef SHADOW_H
#define SHADOW_H

#include <stddef.h>

typedef struct { float x, y, radius, intensity; } ShLight;
typedef struct { float x0, y0, x1, y1; } ShSeg;
typedef struct { int w, h; float *buf; } ShadowMap;

/*
 * Quantization / dither settings for the two-stage write.
 *
 *   enabled == 0: legacy output. sh_dither_error() fills the map with zeros,
 *                 and sh_write_pgm_comp() reproduces the old 8-bit writer
 *                 byte for byte. The levels/amount values are not used.
 *   enabled == 1: quantize to `levels` codes. The written code is perturbed
 *                 by the compensation map from sh_dither_error().
 *
 *   levels: number of output codes, 2..256 (256 = 8-bit output).
 *   amount: error-diffusion strength in [0,1]. 0 means every compensation is
 *           zero, i.e. plain rounding at `levels` codes.
 */
typedef struct {
    int enabled;
    int levels;
    double amount;
} ShDither;

/*
 * A ring band is a maximal run of at least SH_BAND_RUN pixels of one code
 * that (a) is matched code-for-code by the adjacent row, for horizontal
 * runs, or column, for vertical runs, and (b) spans a real change in the
 * ideal value. Condition (a) is the two-row thickness that separates a
 * visible band from dither speckle; condition (b) keeps constant regions,
 * such as fully shadowed interiors, out of the count. One row of a wide ring
 * counts once, so this measures how much of the image is organised into
 * visible bands, not how many annuli exist.
 *
 * Banding is low-frequency quantization error, so ShDitherStats also reports
 * the RMS of the error after a box blur (err_lf_rms). Dithering pushes error
 * to high frequencies and leaves this near zero; plain quantization of a
 * gradient leaves it large. The two numbers should agree on direction.
 *
 * The map is always written as 8-bit PGM. A `levels`-code image is stored
 * with code * 255 / (levels-1) rounded, so a 32-code and a 256-code render
 * can be compared in one format. Band counts are unaffected because that
 * map is strictly increasing.
 */
#define SH_BAND_RUN 4
#define SH_BAND_SPAN 0.5  /* code units the run must cover; a plateau of one
                             code holds source values within +-0.5 of it */
#define SH_BAND_BLUR 4   /* box-blur radius for err_lf_rms, pixels */

typedef struct {
    int levels;           /* effective code count */
    long long ring_bands; /* visible band segments, see SH_BAND_RUN */
    double comp_mean;     /* signed mean compensation, code units */
    double comp_absmean;  /* mean |compensation|, code units */
    double comp_rms;      /* RMS compensation, code units */
    double err_mean;      /* signed mean of (target - written) / (levels-1) */
    double err_rms;       /* RMS of (target - written) / (levels-1) */
    double err_code_rms;  /* RMS of (target - written), code units */
    double err_lf_rms;    /* RMS of the same error after a box blur of radius
                             SH_BAND_BLUR, in code units; the banding signal */
    double stage1_ms;     /* filled by sh_dither_error() */
    double stage2_ms;     /* filled by sh_write_pgm_comp() */
} ShDitherStats;

int sh_init(ShadowMap *m, int w, int h);
void sh_free(ShadowMap *m);
int sh_clear(ShadowMap *m);
int sh_render(ShadowMap *m, const ShLight *light, const ShSeg *segs, int nseg);
int sh_write_pgm(const ShadowMap *m, const char *path);

/*
 * Stage 1 -- error diffusion. Fills comp[0 .. w*h-1] with the compensation,
 * in code units, that stage 2 must add to each sample before rounding. The
 * buffer is the diffusion accumulator itself, so it is fully deterministic
 * and can be inspected or saved. With enabled == 0 every entry is 0.
 *
 * Stage 2 -- perturbed write. Adds comp to each sample, rounds to `levels`
 * codes and writes the PGM. comp may be NULL only when enabled == 0.
 * With enabled == 0 the output is byte-for-byte identical to
 * sh_write_pgm(). `amount` is applied in stage 1 only.
 *
 * Return 0 on success. Both return -1 for a bad map or setting, with err
 * (when non-NULL) naming the offending field and its accepted range. Stage 2
 * further returns -2 when the file cannot be opened, also reported in err.
 * err is always NUL-terminated on failure.
 */
int sh_dither_error(const ShadowMap *m, const ShDither *d, double *comp,
                    ShDitherStats *st, char *err, size_t errlen);
int sh_write_pgm_comp(const ShadowMap *m, const char *path, const ShDither *d,
                      const double *comp, ShDitherStats *st,
                      char *err, size_t errlen);

#endif
