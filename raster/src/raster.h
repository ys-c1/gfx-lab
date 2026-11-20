#ifndef RASTER_H
#define RASTER_H

#include <stddef.h>

/* Return codes for the triangle entry points.  Values >= 0 are the number
 * of fragments written; values < 0 are errors. */
#define RT_OK             0
#define RT_ERR_ARG       -1  /* NULL target/triangle pointer or bad count   */
#define RT_ERR_DEGENERATE -2 /* zero-area triangle                          */
#define RT_ERR_ALPHA     -3  /* tri->alpha outside [0,1] (or NaN)           */
#define RT_ERR_TARGET    -4  /* target has no colour/depth storage          */
#define RT_ERR_MEM       -5  /* fragment staging buffer allocation failed   */

typedef struct { double x, y, z; } RVert;

typedef struct {
    RVert v[3];
    unsigned char rgb[3];
    float alpha;         /* straight alpha, 0.0 (transparent) .. 1.0 (opaque) */
} RTri;

/* One resolved fragment held between the blend pass and the write-back pass. */
typedef struct {
    size_t idx;          /* framebuffer index */
    float z;
    unsigned char rgb[3];
} RtFrag;

typedef struct {
    int written;         /* fragments that passed the depth test and were stored */
    int blended;         /* of those, fragments mixed with the destination
                            (blend on and 0 < alpha < 1) */
} RtStats;

typedef struct {
    int w, h;
    float *z;
    unsigned char *rgb;
    int blend;           /* 0: overwrite (legacy), 1: alpha blend; default 0   */
    int tris;            /* successful submissions; next automatic triangle id */
    int err_code;        /* code of the most recent submission, RT_OK if none   */
    int err_tri;         /* triangle id tied to the most recent error, or -1    */
    RtStats last_stats;  /* statistics of the most recent submission            */
    RtFrag *frag;        /* internal two-phase staging buffer                   */
    size_t frag_cap;     /* capacity of frag, in fragments                      */
} RTarget;

int rt_init(RTarget *t, int w, int h);
void rt_free(RTarget *t);

/* Enables/disables alpha blending.  Disabled by default; disabled output is
 * pixel-for-pixel identical to the original overwrite rasteriser. */
int rt_set_blend(RTarget *t, int enabled);

/* Draw one triangle.  The triangle id is the target's submission counter. */
int rt_triangle(RTarget *t, const RTri *tri);

/* Draw one triangle with a caller-chosen id, used when reporting errors.
 * stats may be NULL.  Blending disabled, or alpha == 1: overwrite.  Blending
 * enabled with 0 < alpha < 1: mix source with destination, then write back.
 * Blending enabled with alpha == 0: colour and depth are left untouched. */
int rt_triangle_at(RTarget *t, const RTri *tri, int tri_index, RtStats *stats);

/* Draw n triangles whose ids are their positions 0..n-1 in tris[]; stats (if
 * given) aggregates all of them.  On error the triangle is not drawn and
 * t->err_tri gives its position in tris[]. */
int rt_draw(RTarget *t, const RTri *tris, int n, RtStats *stats);

int rt_write_ppm(const RTarget *t, const char *path);
const char *rt_error_string(int code);

#endif
