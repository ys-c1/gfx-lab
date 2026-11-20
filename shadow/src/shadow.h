#ifndef SHADOW_H
#define SHADOW_H

/* samples < 0: derive the sample count from the stage-1 penumbra bandwidth */
#define SH_SAMPLES_ADAPTIVE (-1)

typedef struct {
    float x, y;        /* light centre, pixel space */
    float radius;      /* falloff radius (> 0) */
    float intensity;   /* >= 0 */
    float src_radius;  /* area-light radius, drives penumbra (> 0 when enabled) */
} ShLight;

typedef struct { float x0, y0, x1, y1; } ShSeg;
typedef struct { int w, h; float *buf; } ShadowMap;

typedef struct {
    int enable;   /* 0 => hard shadow, pixel-identical to sh_render() */
    int samples;  /* light-point count; 0 is an error, SH_SAMPLES_ADAPTIVE ok */
} ShPenumbra;

typedef struct {
    int code;          /* 0 ok, negative error code, see sh_strerror() */
    char field[64];    /* offending field name, e.g. "light.src_radius" */
    char message[192];
} ShError;

int sh_init(ShadowMap *m, int w, int h);
void sh_free(ShadowMap *m);
int sh_clear(ShadowMap *m);

/* Hard shadow only; unchanged behaviour and byte-identical output. */
int sh_render(ShadowMap *m, const ShLight *light, const ShSeg *segs, int nseg);

/* Stage 1-3 soft shadow. pen == NULL or pen->enable == 0 takes the hard path. */
int sh_render_ex(ShadowMap *m, const ShLight *light, const ShSeg *segs, int nseg,
                 const ShPenumbra *pen, ShError *err);

/* Ground truth: per-pixel Monte Carlo over the area-light disk. */
int sh_render_ref(ShadowMap *m, const ShLight *light, const ShSeg *segs, int nseg,
                  int samples, ShError *err);

/* Stage 1 analytic penumbra bandwidth (world/pixel units) at receiver point. */
float sh_penumbra_width(const ShLight *light, const ShSeg *seg, float px, float py);

/* Sample count that SH_SAMPLES_ADAPTIVE resolves to for this scene. */
int sh_adaptive_sample_count(const ShadowMap *m, const ShLight *light,
                             const ShSeg *segs, int nseg);


const char *sh_strerror(const ShError *err);
int sh_write_pgm(const ShadowMap *m, const char *path);

/* error codes */
#define SH_OK 0
#define SH_ERR_ARG -1
#define SH_ERR_LIGHT -2
#define SH_ERR_SEGMENT -3
#define SH_ERR_SAMPLES -4
#define SH_ERR_MEMORY -5
#endif
