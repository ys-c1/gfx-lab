#ifndef SHADOW_H
#define SHADOW_H
typedef struct { float x, y, radius, intensity; } ShLight;
typedef struct { float x0, y0, x1, y1; } ShSeg;
typedef struct { int w, h; float *buf; } ShadowMap;

/* configurable quantization path: target bit depth + on/off switch */
typedef struct {
    int bits;     /* target bit depth per pixel, 1..16 (PGM maxval = 2^bits-1) */
    int enabled;  /* 1 = staged requantize path, 0 = legacy fixed 8-bit path */
} ShQuantCfg;

typedef struct {
    int bits;         /* bit depth actually used */
    long samples;     /* number of pixels quantized */
    double step;      /* quantization step in normalized [0,1] units */
    double noise_rms; /* RMS of reconstruction noise (reconstructed - input), normalized */
} ShQuantStats;

/* error codes: negatives are reported per offending parameter */
#define SH_E_BITS_NEG   (-3) /* cfg.bits < 0 */
#define SH_E_BITS_RANGE (-4) /* cfg.bits not in [1,16] while enabled */
#define SH_E_FLAG_NEG   (-5) /* cfg.enabled < 0 */
#define SH_E_NOMEM      (-6) /* output buffer allocation failed */

int sh_init(ShadowMap *m, int w, int h);
void sh_free(ShadowMap *m);
int sh_clear(ShadowMap *m);
int sh_render(ShadowMap *m, const ShLight *light, const ShSeg *segs, int nseg);
int sh_write_pgm(const ShadowMap *m, const char *path);

/* three-stage pipeline: (1) step from target bits, (2) per-sample quantize,
   (3) noise statistics. allocates *out; caller frees. */
int sh_quantize(const ShadowMap *m, const ShQuantCfg *cfg,
                unsigned char **out, long *nbytes, ShQuantStats *st);

/* like sh_write_pgm, but through the configurable path.
   when cfg->enabled == 0 it calls sh_write_pgm verbatim (byte-identical). */
int sh_write_pgm_q(const ShadowMap *m, const char *path,
                   const ShQuantCfg *cfg, ShQuantStats *st);
#endif
