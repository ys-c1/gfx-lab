#ifndef SHADOW_H
#define SHADOW_H

/* ---- base single-map shadow renderer ----------------------------------- */

typedef struct { float x, y, radius, intensity; } ShLight;
typedef struct { float x0, y0, x1, y1; } ShSeg;
typedef struct { int w, h; float *buf; } ShadowMap;

int sh_init(ShadowMap *m, int w, int h);
void sh_free(ShadowMap *m);
int sh_clear(ShadowMap *m);
int sh_render(ShadowMap *m, const ShLight *light, const ShSeg *segs, int nseg);
int sh_write_pgm(const ShadowMap *m, const char *path);

/* ---- cascaded (tiered) shadow maps --------------------------------------
 *
 * Two phases:
 *   1. sh_csm_build  : split the scene by camera distance into tiers and
 *                      allocate one per-tier shadow map (the tier table).
 *   2. sh_csm_render : render every tier at its own resolution, then
 *      sh_csm_compose: sample each output pixel from the tier that owns its
 *                      camera distance and write one final map.
 *
 * Tier boundary convention: tier i owns camera distances d in
 *     cfg[i].near <= d < cfg[i].far          (half-open, right-exclusive)
 * so a point sitting exactly on a boundary belongs to the farther tier.
 * Each tier covers the axis-aligned square centered on the camera with side
 * 2*far; its map is res x res texels, texel scale = (2*far)/res world units.
 * Output pixels whose distance is not covered by any tier (beyond the last
 * far plane, or before the first near plane) are written as 0 and counted in
 * ShCsmStats.uncovered.
 */

typedef struct {
    float near;   /* near camera distance, world units, >= 0              */
    float far;    /* far  camera distance, world units, > near            */
    int   res;    /* per-tier shadow map resolution, square, > 0          */
} ShTierCfg;

typedef struct {
    int   index;        /* tier index, 0 = nearest                        */
    float near, far;    /* distance band [near, far)                      */
    int   res;          /* shadow map resolution (res x res)              */
    float x0, y0;       /* covered world square, min corner (camera space)*/
    float x1, y1;       /* covered world square, max corner               */
    float texel;        /* world units per texel = (2*far)/res            */
} ShTier;

typedef struct {
    float    camx, camy;    /* camera position in world space             */
    int      n;             /* number of tiers                             */
    ShTier  *tiers;         /* tier table, n entries (queryable)           */
    ShadowMap *maps;        /* per-tier shadow maps, n entries             */
} ShCascade;

typedef struct {
    long ray_tests;     /* ray/segment intersection tests performed       */
    long texels;        /* texels rendered across all tier maps           */
    long composed;      /* output pixels written by sh_csm_compose        */
    long uncovered;     /* output pixels owned by no tier (written 0)     */
} ShCsmStats;

/* Error codes returned by the CSM functions. sh_csm_strerror names the
 * offending field where applicable. */
#define SH_OK            0
#define SH_ERR_ARG      -1   /* generic null/bad pointer argument          */
#define SH_ERR_TIERS    -2   /* field: tiers  (number of tiers <= 0)       */
#define SH_ERR_RANGE    -3   /* field: near/far (inverted or negative band)*/
#define SH_ERR_RES      -4   /* field: res    (tier resolution <= 0)        */
#define SH_ERR_MEM      -5   /* allocation failure                          */
#define SH_ERR_LIGHT    -6   /* light radius <= 0 or intensity < 0          */

const char *sh_csm_strerror(int code);

/* Phase 1: validate the tier configuration, build the queryable tier table
 * and allocate per-tier maps. Returns SH_OK or one of SH_ERR_*. */
int sh_csm_build(ShCascade *c, float camx, float camy,
                 const ShTierCfg *cfg, int ntiers);
void sh_csm_free(ShCascade *c);

/* Phase 2a: render every tier map at its own resolution. */
int sh_csm_render(ShCascade *c, const ShLight *light,
                  const ShSeg *segs, int nseg, ShCsmStats *st);

/* Phase 2b: composite the tier maps into an output world-space map of
 * out->w x out->h texels covering world box [ox0,oy0]-[ox1,oy1]. */
int sh_csm_compose(const ShCascade *c, ShadowMap *out,
                   float ox0, float oy0, float ox1, float oy1,
                   ShCsmStats *st);

/* Return the tier index owning camera distance d, or -1 if no tier covers
 * it. Exposes the [near,far) boundary rule for callers and tests. */
int sh_csm_pick(const ShCascade *c, float d);

/* Fetch the tier table: copies the internal pointer to *out and the tier
 * count to *n. The table stays owned by the cascade. */
int sh_csm_table(const ShCascade *c, const ShTier **out, int *n);

#endif
