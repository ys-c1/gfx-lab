#ifndef RASTER_H
#define RASTER_H

#include <stddef.h>

typedef struct { double x, y, z; } RVert;
typedef struct { RVert v[3]; unsigned char rgb[3]; } RTri;

/* ------------------------------------------------------------------ */
/* Wireframe overlay                                                    */
/*                                                                      */
/* The overlay has its own storage, so it is never overwritten by a     */
/* later fill.  It is built in three explicit stages:                   */
/*                                                                      */
/*   1. rt_wire_stroke()  edge-function stroking into a layer mask      */
/*   2. rt_wire_merge()   layer-number ordering into an overlay image   */
/*   3. rt_wire_blend()   opacity blending of that image onto the frame */
/*                                                                      */
/* Stages 1 and 2 never touch t->rgb or t->z.  Only stage 3 writes      */
/* colour, and it never writes depth.  With every spec->enabled == 0 (or */
/* with no layers at all) the frame is left byte-for-byte as it was.    */
/* ------------------------------------------------------------------ */

#define RT_WIRE_MAX_LAYERS 64

/* Edges shorter than this (in pixels) are rejected.  Exact coincident
 * endpoints are reported separately as RT_WIRE_ERR_DUP_VERTEX. */
#define RT_WIRE_MIN_EDGE 1e-6

typedef struct {
    int layer;             /* layer number, >= 0, unbounded; lower
                            * layer numbers composite first            */
    float opacity;         /* finite, within [0,1]                       */
    unsigned char rgb[3];  /* overlay colour                            */
    double halfwidth;      /* finite, >= 0; covered when dist <= this   */
    int enabled;           /* 0: complete no-op, nothing validated      */
} RWireSpec;

typedef struct {
    int layer_slot;        /* stored layer slot, -1 when none was made  */
    int layer;             /* echo of spec->layer                       */
    float opacity;         /* echo of spec->opacity                     */
    int covered;           /* distinct pixels covered by this layer     */
    int per_edge[3];       /* pixels covered by each edge (edge i is
                            * v[i] -> v[(i+1)%3]); overlaps between
                            * edges are counted once per edge           */
} RWireStrokeStats;

/* Result of stage 2.  The order arrays are the reproducible composite
 * order: ascending layer number, ties broken by stroke order. */
typedef struct {
    int layers;
    int covered;           /* pixels covered by at least one layer      */
    int contributions;     /* sum over pixels of covering layers        */
    int opaque;            /* pixels whose composite alpha is 1         */
    int order[RT_WIRE_MAX_LAYERS];          /* layer slots, in order   */
    int order_layer[RT_WIRE_MAX_LAYERS];    /* layer numbers           */
    float order_opacity[RT_WIRE_MAX_LAYERS];/* opacities               */
    int order_covered[RT_WIRE_MAX_LAYERS];  /* per-layer pixel counts  */
} RWireMergeStats;

typedef struct {
    int blended;           /* pixels with composite alpha > 0           */
    int replaced;          /* of those, composite alpha == 1            */
} RWireBlendStats;

typedef struct {
    RWireMergeStats merge;
    RWireBlendStats blend;
} RWireResolveStats;

/* Error location.  Every rejected value is described by code plus the
 * layer number, layer slot (when one exists), edge / vertex indices and
 * the offending field name.  rt_wire_errfmt() renders all of it. */
typedef struct {
    int code;              /* RT_WIRE_ERR_*                            */
    const char *field;     /* "layer", "opacity", ... or ""            */
    int layer_slot;        /* stored slot, -1 when none                */
    int layer;             /* requested layer number, -1 when unknown  */
    int edge;              /* edge index 0..2, -1 when not edge-specific */
    int vertex;            /* first vertex, -1 when not applicable     */
    int vertex2;           /* second vertex, -1 when not applicable    */
} RWireErr;

enum {
    RT_WIRE_OK = 0,
    RT_WIRE_ERR_ARG = -1,
    RT_WIRE_ERR_LAYER = -2,
    RT_WIRE_ERR_OPACITY = -3,
    RT_WIRE_ERR_HALFWIDTH = -4,
    RT_WIRE_ERR_VERTEX = -5,
    RT_WIRE_ERR_DUP_VERTEX = -6,
    RT_WIRE_ERR_SHORT_EDGE = -7,
    RT_WIRE_ERR_CAPACITY = -8,
    RT_WIRE_ERR_MEM = -9
};

/* One stroked layer.  Internal to the library; exposed only because
 * RTarget is a plain C struct. */
typedef struct RTWireLayer {
    int layer;
    float opacity;
    int seq;               /* stroke order, tie-breaker for equal layer */
    unsigned char rgb[3];
    int covered;
    int per_edge[3];
    int minx, miny, maxx, maxy;   /* actual coverage bounds, inclusive */
    unsigned char *mask;          /* w*h, 1 where covered              */
} RTWireLayer;

typedef struct {
    int w, h;
    float *z;
    unsigned char *rgb;

    /* private overlay state */
    RTWireLayer layers[RT_WIRE_MAX_LAYERS];
    int layer_count;
    int layer_seq;
    unsigned char *ov_cov;   /* union coverage of merged layers         */
    double *ov_pm;           /* premultiplied overlay colour, 3/pixel   */
    double *ov_a;            /* composite overlay alpha, 1/pixel        */
    int ov_staged;           /* merge ran, blend not yet consumed       */
} RTarget;

int rt_init(RTarget *t, int w, int h);
void rt_free(RTarget *t);
int rt_triangle(RTarget *t, const RTri *tri);

/* Stage 1: distance from the centre of pixel (x,y) to segment a-b,
 * computed with the edge function and clamped projection.  Distance is
 * in pixels.  Returns RT_WIRE_OK and writes *dist, or an error. */
int rt_wire_edge_dist(const RVert *a, const RVert *b, int x, int y, double *dist);

/* Stage 1: validate the spec and triangle, stroke all three edges in the
 * caller's vertex order into a new overlay layer.  On any error nothing
 * is stored and no buffer is modified.  A disabled spec is a no-op and
 * is not validated. */
int rt_wire_stroke(RTarget *t, const RTri *tri, const RWireSpec *spec,
                   RWireStrokeStats *stats, RWireErr *err);

/* Stage 2: order all layers by (layer, stroke order) and build the
 * separate overlay image with source-over compositing.  t->rgb and t->z
 * are not touched.  Calling it again rebuilds the same image. */
int rt_wire_merge(RTarget *t, RWireMergeStats *stats, RWireErr *err);

/* Stage 3: blend the merged overlay onto t->rgb:
 *     out = round(overlay_pm + dst * (1 - overlay_alpha))
 * t->z is not touched.  Without a preceding merge this is a no-op. */
int rt_wire_blend(RTarget *t, RWireBlendStats *stats, RWireErr *err);

/* merge + blend. */
int rt_wire_resolve(RTarget *t, RWireResolveStats *stats, RWireErr *err);

/* Drop every stroked layer and pending merge.  The frame is untouched. */
void rt_wire_clear(RTarget *t);

const char *rt_wire_strerror(int code);
int rt_wire_errfmt(const RWireErr *err, char *buf, size_t cap);

int rt_write_ppm(const RTarget *t, const char *path);

#endif
