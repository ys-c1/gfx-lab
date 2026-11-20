#ifndef SHADOW_H
#define SHADOW_H

/* Maximum number of vertices a convex light frustum may have. */
#define SH_FRUSTUM_MAX 8

/* Return / error codes.  Values -1 and -2 keep the original meaning. */
enum {
    SH_OK = 0,
    SH_E_ARG = -1,             /* null / invalid argument                     */
    SH_E_LIGHT = -2,           /* light radius or intensity out of range      */
    SH_E_EMPTY_FRUSTUM = -3,   /* < 3 vertices, or zero area                  */
    SH_E_NO_SEGMENTS = -4,     /* nseg <= 0                                   */
    SH_E_NONFINITE = -5,       /* NaN / infinity in a coordinate              */
    SH_E_FRUSTUM_COVER = -6,   /* frustum does not contain light + map        */
    SH_E_CAPACITY = -7,        /* candidate table too small                   */
    SH_E_NOMEM = -8,           /* allocation failure                          */
    SH_E_IO = -9               /* PGM output could not be opened / written    */
};

typedef struct { float x, y, radius, intensity; } ShLight;
typedef struct { float x0, y0, x1, y1; } ShSeg;
typedef struct { int w, h; float *buf; } ShadowMap;

typedef struct { float x, y; } ShPoint;

/* Convex polygon, vertices in either winding order.  A light frustum built by
 * sh_frustum_from_light() is CCW. */
typedef struct {
    int n;                       /* 3 .. SH_FRUSTUM_MAX */
    ShPoint v[SH_FRUSTUM_MAX];
} ShFrustum;

/* Detailed diagnostic.  index identifies the offending element:
 *   - segment index   for SH_E_NONFINITE in a segment
 *   - vertex index    for SH_E_NONFINITE / SH_E_EMPTY_FRUSTUM in a frustum
 *   - 0..3            for SH_E_FRUSTUM_COVER (map corner order:
 *                     (0,0), (w,0), (w,h), (0,h)); -1 means the light
 *   - -1              when there is no meaningful index (zero segments,
 *                     non-finite light)
 * msg is a static string and is never NULL on error. */
typedef struct {
    int code;
    int index;
    const char *msg;
} ShError;

/* Counters for the two-layer path. */
typedef struct {
    int input;       /* segments submitted                         */
    int culled;      /* rejected by the frustum layer              */
    int candidates;  /* retained and tested against pixels         */
} ShCullStats;

typedef struct {
    int enabled;          /* 0: test every segment (legacy behaviour) */
    ShFrustum frustum;    /* used when enabled != 0                   */
} ShCullOpts;

int sh_init(ShadowMap *m, int w, int h);
void sh_free(ShadowMap *m);
int sh_clear(ShadowMap *m);

/* Build the light frustum: the convex hull of the light position and the four
 * corners of the shadow map.  Every ray from the light to a pixel centre lies
 * inside this polygon, so any segment outside it cannot occlude any pixel. */
int sh_frustum_from_light(const ShadowMap *m, const ShLight *light,
                          ShFrustum *out, ShError *err);

/* Layer 1 -- culling.  Validates the frustum and every segment, then writes
 * the indices (ascending, original order) of the segments that intersect or
 * lie inside the frustum to cand[0..*stats.candidates).  cand_cap must be at
 * least nseg for the always-succeeds path; otherwise SH_E_CAPACITY is
 * returned as soon as the table overflows.
 *
 * The caller must guarantee that the frustum contains the light and the whole
 * map (sh_render_ex() checks this); then "segment does not touch the frustum"
 * implies "segment blocks no pixel", so the layer is loss-free. */
int sh_cull_segments(const ShFrustum *f, const ShSeg *segs, int nseg,
                     int *cand, int cand_cap, ShCullStats *stats, ShError *err);

/* Layer 2 -- rendering.  Tests pixel centres only against the listed
 * candidates; cand may be NULL when ncand == 0.  The candidate order must be
 * the original segment order for results to be bit-identical to the
 * brute-force path. */
int sh_render_candidates(ShadowMap *m, const ShLight *light,
                         const ShSeg *segs, const int *cand, int ncand);

/* Two-layer entry point.  opts == NULL or opts->enabled == 0 renders every
 * segment, exactly like the original implementation.  When enabled, the
 * candidate table is built first and only candidates are tested per pixel.
 * stats may be NULL.  err may be NULL but then the offending index is lost. */
int sh_render_ex(ShadowMap *m, const ShLight *light, const ShSeg *segs, int nseg,
                 const ShCullOpts *opts, ShCullStats *stats, ShError *err);

/* Legacy entry point: no culling.  Pixel results are bit-identical to the
 * pre-culling implementation; nseg == 0 is now rejected (SH_E_NO_SEGMENTS). */
int sh_render(ShadowMap *m, const ShLight *light, const ShSeg *segs, int nseg);

int sh_write_pgm(const ShadowMap *m, const char *path);

#endif
