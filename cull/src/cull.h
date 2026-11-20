#ifndef CULL_H
#define CULL_H

/* ------------------------------------------------------------------ */
/* View culling (unchanged)                                            */
/* ------------------------------------------------------------------ */
typedef struct { double x, y, w, h; int layer; } CullBox;
typedef struct { double x, y, w, h; } CullView;
typedef struct { int *ids; int count; } CullResult;
int cull_run(const CullBox *boxes, int n, CullView view, CullResult *out);
int cull_count_within(const CullBox *boxes, int n, CullView view);

/* ------------------------------------------------------------------ */
/* Two-stage occlusion culling                                         */
/*                                                                     */
/* Stage 1: rasterize every occluder rectangle into a configurable     */
/*          screen-space depth map (nearest depth per cell).           */
/* Stage 2: depth-test each candidate against that map and drop the    */
/*          ones that lie behind a nearer occluder over their whole    */
/*          footprint.                                                 */
/*                                                                     */
/* Lower depth == closer to the viewer.  A candidate is occluded by an */
/* occluder only when occluder.depth < candidate.depth (strict).       */
/* ------------------------------------------------------------------ */

#define CULL_OK               0
#define CULL_ERR_ARG         (-1)
#define CULL_ERR_VIEW        (-2)
#define CULL_ERR_EMPTY_DEPTH (-3) /* depth map NULL, not rendered or has no covered cell */
#define CULL_ERR_NONFINITE   (-4) /* non-finite coordinate/depth; *err_index names the item */
#define CULL_ERR_RECT        (-5) /* negative width/height; *err_index names the item */

typedef struct {
    double x, y, w, h; /* screen-space rectangle */
    double depth;      /* larger = farther from the viewer */
} CullSprite;

typedef struct {
    int w, h;                       /* depth-map resolution (cells) */
    double x0, y0, x1, y1;         /* screen region covered by the map */
    double cell_w, cell_h;
    float *depth;                   /* nearest occluder depth per cell */
    unsigned char *cover;           /* 1 when an occluder fully covers the cell */
    int covered_cells;
    int rendered;                   /* set by cull_depth_render */
} CullDepthMap;

typedef struct {
    int *ids;    /* candidates kept visible, ascending */
    int count;   /* number of visible candidates */
    int culled;  /* n - count */
} CullOcclusionResult;

/* Stage 1 setup.  Resolution w x h is independent from the region size. */
int cull_depth_init(CullDepthMap *m, int w, int h,
                    double x0, double y0, double x1, double y1);
void cull_depth_free(CullDepthMap *m);

/* Stage 1: rasterize occluders.  Returns CULL_ERR_NONFINITE / CULL_ERR_RECT
 * with *err_index set to the offending occluder index (0-based). */
int cull_depth_render(CullDepthMap *m, const CullSprite *occluders, int n,
                      int *err_index);

/* Stage 2.  enabled == 0 keeps the visible set unchanged (all candidates,
 * ascending) and does not require a depth map.  When enabled, an empty depth
 * map is an error and *err_index is -1 for map-level failures. */
int cull_occlusion(const CullDepthMap *m, const CullSprite *candidates, int n,
                   int enabled, CullOcclusionResult *out, int *err_index);
void cull_occlusion_free(CullOcclusionResult *r);

/* Exact ground truth: a candidate stays visible iff at least one point of its
 * rectangle is not covered by the union of strictly nearer occluders.
 * Axis-aligned rectangles are handled analytically (coordinate compression),
 * so this is not a sampled approximation.  visible[i] is 1 or 0. */
int cull_reference_visible(const CullSprite *occluders, int nocc,
                           const CullSprite *candidates, int n,
                           unsigned char *visible, int *err_index);

#endif
