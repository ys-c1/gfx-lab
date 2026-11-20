#ifndef RASTER_H
#define RASTER_H

typedef struct { double x, y, z; } RVert;
typedef struct { RVert v[3]; unsigned char rgb[3]; } RTri;

/* Winding convention: signed area is the usual cross product
 *   A2 = (v1-v0) x (v2-v0)
 *   A  = A2 / 2
 * A > 0 is counter-clockwise (CCW) in the vertex coordinate system as
 * supplied.  If the caller uses image coordinates (y growing downwards),
 * a positive area appears clockwise on screen; the labels below always
 * refer to the numeric sign of the supplied coordinates, never to screen
 * appearance. */
typedef enum {
    RT_WIND_DEGENERATE = 0,
    RT_WIND_CCW,            /* signed area > 0 */
    RT_WIND_CW              /* signed area < 0 */
} RTWinding;

typedef enum {
    /* No backface culling.  This is the legacy path: it is byte-for-byte
     * identical to the pre-culling rasterizer (which only filled the
     * positive-area side because of its fixed edge test). */
    RT_CULL_NONE = 0,
    /* Keep only CCW (positive signed area) triangles. */
    RT_CULL_KEEP_CCW,
    /* Keep only CW (negative signed area) triangles. */
    RT_CULL_KEEP_CW,
    /* Explicit two-sided mode: fill both windings. */
    RT_CULL_BOTH
} RTCullMode;

typedef enum {
    RT_CULL_REASON_NONE = 0,        /* not culled and not rejected */
    RT_CULL_REASON_BACKFACE_CW,     /* culled: CW triangle while keeping CCW */
    RT_CULL_REASON_BACKFACE_CCW,    /* culled: CCW triangle while keeping CW */
    RT_CULL_REASON_COINCIDENT,      /* rejected: two vertices share x/y */
    RT_CULL_REASON_ZERO_AREA,       /* rejected: A2 == 0 exactly */
    RT_CULL_REASON_DEGENERATE_WINDING, /* rejected: |A2| < 1e-12 or non-finite */
    RT_CULL_REASON_COUNT
} RTCullReason;

typedef struct {
    int w, h;
    float *z;
    unsigned char *rgb;
    RTCullMode cull;
} RTarget;

typedef struct {
    double signed_area2;   /* cross product, = 2 * signed_area */
    double signed_area;    /* cross product / 2 */
    RTWinding winding;
    RTCullReason reason;
    int covered;           /* pixels accepted by the fill rule */
    int filled;            /* pixels that passed the depth test and were written */
} RTTriInfo;

typedef struct {
    int triangles;
    int drawn;             /* not culled and not rejected */
    int culled;            /* backface culls */
    int rejected;          /* degenerate rejects */
    int reason_counts[RT_CULL_REASON_COUNT];
    int *culled_idx;       /* input indices of culled triangles */
    int culled_n;
    int *rejected_idx;     /* input indices of rejected triangles */
    int rejected_n;
    RTTriInfo *info;       /* one entry per input triangle (may be NULL for n==0) */
    long long covered;     /* sum of info[i].covered */
    long long filled;      /* sum of info[i].filled */
    double elapsed_ms;     /* wall time spent in rt_batch */
} RTBatchStats;

int rt_init(RTarget *t, int w, int h);
void rt_free(RTarget *t);
int rt_clear(RTarget *t);
void rt_set_cull(RTarget *t, RTCullMode mode);
RTCullMode rt_get_cull(const RTarget *t);

/* Geometry-only classification.  *_area may be NULL.  Returns RTCull_REASON_NONE
 * for a usable triangle, otherwise the rejection reason.  Winding is set even
 * for rejected triangles (RT_WIND_DEGENERATE when no sign is meaningful). */
RTCullReason rt_classify(const RTri *tri, double *signed_area,
                         double *signed_area2, RTWinding *winding);

int rt_triangle(RTarget *t, const RTri *tri);
int rt_triangle_info(RTarget *t, const RTri *tri, RTTriInfo *info);
int rt_batch(RTarget *t, const RTri *tris, int n, RTBatchStats *stats);
void rt_stats_free(RTBatchStats *stats);

const char *rt_winding_name(RTWinding w);
const char *rt_cull_reason_name(RTCullReason r);

int rt_write_ppm(const RTarget *t, const char *path);
#endif
