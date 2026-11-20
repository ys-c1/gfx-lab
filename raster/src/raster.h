#ifndef RASTER_H
#define RASTER_H

#include <stddef.h>

typedef struct { double x, y, z; } RVert;
typedef struct { RVert v[3]; unsigned char rgb[3]; } RTri;
typedef struct { int w, h; float *z; unsigned char *rgb; unsigned char *wire; } RTarget;

/* Wireframe pass.
 *
 * Stage 1 is rt_edge_dist(): distance from a pixel centre to the edge segment,
 * in pixels.  Stage 2 is rt_wire(): threshold that distance, draw, and count.
 *
 * threshold is the half width in pixels; the nominal line width is 2*threshold.
 * A pixel centre at distance <= threshold is covered.
 */
typedef struct {
    double threshold;      /* >= 0; negative is an error when enabled */
    unsigned char rgb[3];  /* overlay colour */
    int enabled;           /* 0: leave z/rgb/wire untouched, skip validation */
} RWire;

typedef struct {
    int edge[3];  /* covered pixel count per edge, in vertex order */
    int covered;  /* distinct pixels covered by any edge */
} RWireStats;

typedef struct {
    int code;     /* RT_WIRE_* */
    int edge;     /* edge index, -1 when not edge-specific */
    int vertex;   /* first offending vertex, -1 when not vertex-specific */
    int vertex2;  /* second offending vertex, -1 when not applicable */
} RWireErr;

enum {
    RT_WIRE_OK = 0,
    RT_WIRE_ERR_ARG = -1,
    RT_WIRE_ERR_THRESHOLD = -2,
    RT_WIRE_ERR_SHORT_EDGE = -3,
    RT_WIRE_ERR_DUP_VERTEX = -4
};

/* Edges shorter than this (in pixels) are rejected.  Exact coincident
 * endpoints are reported separately as RT_WIRE_ERR_DUP_VERTEX. */
#define RT_WIRE_MIN_EDGE 1e-6

int rt_init(RTarget *t, int w, int h);
void rt_free(RTarget *t);
int rt_triangle(RTarget *t, const RTri *tri);

/* Stage 1: distance from the centre of pixel (x,y) to segment a-b.
 * Returns RT_WIRE_OK and writes *dist, or an RT_WIRE_ERR_* on bad input. */
int rt_edge_dist(const RVert *a, const RVert *b, int x, int y, double *dist);

/* Stage 2: validate, rasterise the three edges in the given vertex order,
 * and fill stats.  Vertex order is never changed.  The overlay writes rgb and
 * the wire mask only; z is never touched, so the fill pass stays intact. */
int rt_wire(RTarget *t, const RTri *tri, const RWire *cfg,
            RWireStats *stats, RWireErr *err);

const char *rt_wire_strerror(int code);
int rt_wire_errfmt(const RWireErr *err, char *buf, size_t cap);
double rt_wire_nominal_width(const RWire *cfg);

int rt_write_ppm(const RTarget *t, const char *path);

#endif
