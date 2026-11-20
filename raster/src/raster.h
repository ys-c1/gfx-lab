#ifndef RASTER_H
#define RASTER_H

typedef struct { double x, y, z; } RVert;
typedef struct { RVert v[3]; unsigned char rgb[3]; } RTri;
typedef struct { int w, h; float *z; unsigned char *rgb; } RTarget;

/* Default alignment step: half a pixel. Snapping onto a 0.5 px lattice makes
 * the vertices and therefore every edge of the snapped triangle independent of
 * sub-half-pixel motion, which is what removes the per-frame crawl. */
#define RT_ALIGN_HALF_PX 0.5
/* Largest |x| or |y| accepted for a vertex before it counts as an overflow. */
#define RT_MAX_COORD 1.0e9
#define RT_ALIGN_MSG_MAX 96

/* enabled == 0 means "do not align": the original per-vertex floating-point
 * result is produced unchanged. When enabled, grid is the lattice step in
 * pixels and must be finite and > 0. */
typedef struct {
    int enabled;
    double grid;
} RTAlign;

typedef struct {
    int code;                         /* RT_OK or one of the RT_E_* codes */
    int vertex;                       /* offending vertex 0..2, -1 if none */
    double dx[3], dy[3];              /* snapped - input, per vertex */
    double max_displacement;          /* max |dx| or |dy| over all vertices */
    int collapsed;                    /* snapped triangle has no area */
    char msg[RT_ALIGN_MSG_MAX];       /* detail; names the vertex when relevant */
} RTAlignReport;

enum {
    RT_OK = 0,
    RT_E_ARG = -1,       /* null target/triangle/output */
    RT_E_DEGEN = -2,     /* input triangle has zero area */
    RT_E_GRID = -3,      /* alignment enabled with a non-positive/NaN grid */
    RT_E_MISSING = -4,   /* a vertex coordinate is NaN (vertex is missing) */
    RT_E_OVERFLOW = -5   /* a vertex coordinate is infinite or beyond
                            RT_MAX_COORD (vertex number is reported) */
};

int rt_init(RTarget *t, int w, int h);
void rt_free(RTarget *t);
int rt_triangle(RTarget *t, const RTri *tri);

/* Snap the vertices onto the alignment grid and report the offsets. Does not
 * touch any render target. With alignment disabled this copies *tri to *out
 * and reports zero offsets. Returns RT_OK or a negative RT_E_* code. A
 * triangle that collapses onto a lattice line is not an error: *out is
 * copied, report->collapsed is set, and it then rasterizes as zero pixels. */
int rt_align_vertices(const RTri *tri, const RTAlign *align, RTri *out, RTAlignReport *report);

/* rt_align_vertices followed by scan conversion of the snapped triangle.
 * Returns the number of pixels written (>= 0) or an RT_E_* code. Offsets and
 * diagnostics go to report when it is not NULL. */
int rt_triangle_aligned(RTarget *t, const RTri *tri, const RTAlign *align, RTAlignReport *report);

int rt_write_ppm(const RTarget *t, const char *path);

#endif
