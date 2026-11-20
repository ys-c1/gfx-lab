#ifndef SHADOW_H
#define SHADOW_H
typedef struct { float x, y, radius, intensity; } ShLight;
typedef struct { float x0, y0, x1, y1; } ShSeg;
typedef struct { int w, h; float *buf; } ShadowMap;

/* samples == SH_SAMPLES_OFF keeps the original single center sample and the
   original arithmetic, so the image is bit-for-bit unchanged.  Any positive
   sample count must be a multiple of four; the canonical four-sample mode is
   the 2x2 grid of pixel quadrant centers.  Coverage is accumulated over the
   subsamples and the pixel color is their mean. */
#define SH_SAMPLES_OFF 0

enum {
    SH_OK = 0,
    SH_ERR_ARG = -1,     /* null pointer, negative nseg, or unallocated map */
    SH_ERR_LIGHT = -2,   /* radius <= 0 or intensity < 0 */
    SH_ERR_SAMPLES = -3, /* sample count negative or not a multiple of four */
    SH_ERR_SIZE = -4     /* zero (or negative) map dimension */
};

typedef struct {
    int samples;         /* subsamples per pixel used (1 when sampling is off) */
    long long total;     /* w * h * samples */
    long long lit;       /* unshadowed subsamples */
    double coverage;     /* lit / total, accumulated over subsamples */
    long long edge_pixels;     /* pixels with 0 < coverage < 1, i.e. pixels the
                                  subsampling actually resolves */
    long long boundary_pixels; /* pixels classified differently from a
                                  4-neighbour (coverage >= 0.5 is lit) */
} ShStats;

int sh_init(ShadowMap *m, int w, int h);
/* On rejection *err_field (when non-NULL) names the offending field:
   "w" or "h". */
int sh_init_ex(ShadowMap *m, int w, int h, const char **err_field);
void sh_free(ShadowMap *m);
int sh_clear(ShadowMap *m);
int sh_render(ShadowMap *m, const ShLight *light, const ShSeg *segs, int nseg);
/* samples: 0 = sampling off, otherwise a positive multiple of four.
   stats and coverage (w*h floats, unshadowed fraction per pixel) are optional.
   On rejection *err_field (when non-NULL) names the offending field:
   "m", "w", "h", "buf", "light", "nseg", "segs" or "samples". */
int sh_render_ex(ShadowMap *m, const ShLight *light, const ShSeg *segs, int nseg,
                 int samples, ShStats *stats, float *coverage,
                 const char **err_field);
int sh_write_pgm(const ShadowMap *m, const char *path);
#endif
