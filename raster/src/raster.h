#ifndef RASTER_H
#define RASTER_H
typedef struct { double x, y, z; } RVert;
typedef struct { RVert v[3]; unsigned char rgb[3]; } RTri;
typedef struct {
    int w, h;
    float *z;
    unsigned char *rgb;
    int clip_on;                /* 0 = clipping disabled (default) */
    int cx, cy, cw, ch;         /* clip rect, in pixels */
} RTarget;

/* filled:   pixels actually written (z-test passed)
 * clipped:  samples covered by the original triangle inside the target
 *           but rejected by the clip rect (coverage-based, z-independent) */
typedef struct { int filled; int clipped; } RTriStat;

int rt_init(RTarget *t, int w, int h);
void rt_free(RTarget *t);
void rt_clear(RTarget *t);

/* rt_set_clip error codes (rt_clip_strerror names the failing item):
 *  0 ok
 * -1 null target
 * -2 rect is empty (zero width or height)
 * -3 negative width or height
 * -4 rect exceeds target bounds */
int rt_set_clip(RTarget *t, int x, int y, int w, int h);
void rt_clear_clip(RTarget *t);
const char *rt_clip_strerror(int rc);

/* With clipping disabled these are pixel-identical; rt_triangle_ex also
 * reports fill/clip statistics (st may be NULL). Both return the number of
 * pixels written, -1 bad args, -2 degenerate triangle. */
int rt_triangle(RTarget *t, const RTri *tri);
int rt_triangle_ex(RTarget *t, const RTri *tri, RTriStat *st);

int rt_write_ppm(const RTarget *t, const char *path);
#endif
