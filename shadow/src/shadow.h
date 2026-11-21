#ifndef SHADOW_H
#define SHADOW_H
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
typedef struct { float x, y, radius, intensity; } ShLight;
typedef struct { float x0, y0, x1, y1; } ShSeg;
typedef struct { int w, h; float *buf; } ShadowMap;

int sh_init(ShadowMap *m, int w, int h);
void sh_free(ShadowMap *m);
int sh_clear(ShadowMap *m);
int sh_render(ShadowMap *m, const ShLight *light, const ShSeg *segs, int nseg);
int sh_write_pgm(const ShadowMap *m, const char *path);

/* ---- shaped (area) lights with directional soft shadows ---- */

/* shape: point emitter vs segment (strip) emitter */
#define SH_SHAPE_POINT    0x01
#define SH_SHAPE_SEGMENT  0x02
/* mask build mode: bin-stratified sampling vs all emitter samples */
#define SH_MASK_STRATIFIED 0x10
#define SH_MASK_EXHAUSTIVE 0x20
/* quality tier */
#define SH_TIER_LOW  0x40   /* low speed: few bins/samples, cheap */
#define SH_TIER_HIGH 0x80   /* high speed: more bins/samples */

typedef struct {
    ShLight base;       /* x/y = emitter center, radius/intensity reused */
    int enable;         /* switch: 0 = legacy point-light path, unchanged */
    int shape;          /* SH_SHAPE_* */
    int mask_mode;      /* SH_MASK_* */
    int tier;           /* SH_TIER_* */
    float theta;        /* emitter orientation (radians, strip axis) */
    float half_len;     /* segment emitter half length */
    int bins;           /* direction bins (0 = pick from tier) */
    int samples;        /* emitter samples (0 = pick from tier) */
    char err[160];      /* conflict / validation message */
} ShShapeCfg;

typedef struct {
    int bins;           /* B: direction bins */
    int samples;        /* S: emitter samples */
    float theta, half_len;
    unsigned char *sel; /* B*S, 1 when sample s selected for bin b */
    float *weight;      /* B*S, normalized per-bin weight of selected samples */
} ShDirMask;

/* human-readable message for the last failing sh_* call (validated calls only) */
const char *sh_errmsg(void);

/* stage 1: build the per-direction sample mask from emitter orientation */
int sh_mask_build(const ShShapeCfg *cfg, ShDirMask *out);
void sh_mask_free(ShDirMask *mask);
int sh_mask_write_csv(const ShDirMask *mask, const char *path);

/* stage 2+3: directional visibility weighting, then composite the shadow map */
int sh_render_shaped(ShadowMap *m, const ShShapeCfg *cfg,
                     const ShSeg *segs, int nseg);

/* dispatcher: enable == 0 reproduces sh_render() exactly */
int sh_render_cfg(ShadowMap *m, const ShShapeCfg *cfg,
                  const ShSeg *segs, int nseg);
#endif
