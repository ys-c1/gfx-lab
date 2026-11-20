#ifndef RASTER_H
#define RASTER_H
typedef struct { double x, y, z; } RVert;
typedef struct { RVert v[3]; unsigned char rgb[3]; } RTri;
typedef struct {
    int w, h;
    float *z;
    unsigned char *rgb;
    double near_z; /* near plane depth; a vertex with z < near_z lies behind it */
    int clip;      /* 0: reject a triangle crossing the near plane; 1: clip it */
} RTarget;
typedef struct {
    long tris_in;   /* triangles submitted */
    long tris_out;  /* triangles actually rasterized */
    long new_verts; /* new vertices created by near-plane clipping */
    long pixels;    /* pixels that won the z test and were written */
    int err_tri;    /* index of the first rejected triangle, -1 if none */
} RStats;

#define RE_OK     0
#define RE_ARG   -1 /* invalid argument */
#define RE_DEGEN -2 /* coincident vertices or zero-area triangle */
#define RE_NEAR  -3 /* vertex depth below the near plane while clipping is off */
#define RE_EMPTY -4 /* nothing of the triangle remains after clipping */

int rt_init(RTarget *t, int w, int h);
void rt_free(RTarget *t);
int rt_triangle(RTarget *t, const RTri *tri);
int rt_clip_triangle(const RTri *in, double near, RTri *out, int maxout, int *new_verts);
int rt_draw(RTarget *t, const RTri *tris, int n, RStats *st);
const char *rt_strerror(int code);
int rt_write_ppm(const RTarget *t, const char *path);
#endif
