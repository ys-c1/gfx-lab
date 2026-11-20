#ifndef RASTER_H
#define RASTER_H

/* return codes shared by the plain and textured rasterizers */
#define RT_OK         0
#define RT_ERR_ARG   -1   /* null pointer / bad size / bad flag */
#define RT_ERR_DEGEN -2   /* triangle collapsed to a point or line */
#define RT_ERR_FINITE -3  /* a vertex coordinate is NaN or infinite */
#define RT_ERR_WZERO -4   /* clip-space depth w is zero or non-positive */

/* Relative tolerance on a barycentric coordinate: a pixel center whose
   center lies on a triangle edge can land at ~1e-17 on the wrong side
   through rounding alone. The rasterizer's edge test and the per-pixel
   inverse solve share this tolerance so their coverage agrees. */
#define RT_EDGE_EPS 1e-12

typedef struct { double x, y, z; } RVert;
typedef struct { RVert v[3]; unsigned char rgb[3]; } RTri;
typedef struct { int w, h; float *z; unsigned char *rgb; } RTarget;
int rt_init(RTarget *t, int w, int h);
void rt_free(RTarget *t);
int rt_triangle(RTarget *t, const RTri *tri);
int rt_write_ppm(const RTarget *t, const char *path);

/* ---- textured rasterizer with perspective-correct interpolation ---- */

/* Screen-space vertex: x,y pixel coordinates, w the clip-space depth
   (camera-space distance to the projection plane, strictly > 0),
   u,v the texture coordinates, z the depth-buffer value. */
typedef struct { double x, y, z, w, u, v; } RTexVert;
typedef struct { RTexVert v[3]; unsigned char rgb[3]; } RTexTri;

/* Render target extended with an interpolated uv buffer. */
typedef struct { int w, h; float *z; double *u, *v; unsigned char *rgb; } RTexTarget;
int rtt_init(RTexTarget *t, int w, int h);
void rtt_free(RTexTarget *t);

/* Rasterize one textured triangle.
   correct != 0: three-stage perspective-correct path
       (1) per-vertex 1/w, (2) corrected weights for u,v,z,
       (3) the caller cross-checks against rtt_unproject per pixel.
   correct == 0: plain screen-space barycentric weights, bit-for-bit
       the same math as rt_triangle (affine, warps oblique textures). */
int rtt_triangle(RTexTarget *t, const RTexTri *tri, int correct);

/* Camera-space vertex and triangle used by the per-pixel inverse check. */
typedef struct { double x, y, z, u, v; } RCamVert;
typedef struct { RCamVert v[3]; } RCamTri;

/* Project a camera-space vertex with pinhole intrinsics (focal fx,fy,
   principal point cx,cy); fills a screen-space RTexVert. */
int rtt_project(const RCamVert *cv, double fx, double fy, double cx, double cy,
                RTexVert *out);

/* Per-pixel inverse solve: ray through pixel center (px,py) against the
   camera-space triangle. Returns 1 when the ray hits the triangle
   (uv_out then holds ground-truth texture coords), 0 on a miss,
   negative on a degenerate/non-finite argument. This is the reference
   used to validate the interpolated u,v of rtt_triangle. */
int rtt_unproject(const RCamTri *ct, double fx, double fy, double cx, double cy,
                  double px, double py, double uv_out[2]);

int rtt_write_ppm(const RTexTarget *t, const char *path);
#endif
