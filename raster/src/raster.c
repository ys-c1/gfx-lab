#include "raster.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int finite5(double a, double b, double c, double d, double e) {
    return isfinite(a) && isfinite(b) && isfinite(c) && isfinite(d) && isfinite(e);
}
static int finite6(double a, double b, double c, double d, double e, double f) {
    return isfinite(a) && isfinite(b) && isfinite(c)
        && isfinite(d) && isfinite(e) && isfinite(f);
}

static double edge_xy(double ax, double ay, double bx, double by, double px, double py) {
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

int rt_init(RTarget *t, int w, int h) {
    if (!t || w <= 0 || h <= 0) return -1;
    t->w = w; t->h = h;
    t->z = (float*)malloc(sizeof(float) * (size_t)w * (size_t)h);
    t->rgb = (unsigned char*)malloc((size_t)w * (size_t)h * 3);
    if (!t->z || !t->rgb) { rt_free(t); return -1; }
    for (int i = 0; i < w * h; ++i) { t->z[i] = 1e30f; t->rgb[i*3+0] = 0; t->rgb[i*3+1] = 0; t->rgb[i*3+2] = 0; }
    return 0;
}

void rt_free(RTarget *t) {
    if (!t) return;
    free(t->z); free(t->rgb);
    t->z = NULL; t->rgb = NULL; t->w = 0; t->h = 0;
}

static double edge(const RVert *a, const RVert *b, double px, double py) {
    return (b->x - a->x) * (py - a->y) - (b->y - a->y) * (px - a->x);
}

int rt_triangle(RTarget *t, const RTri *tri) {
    if (!t || !tri) return -1;
    double area = edge(&tri->v[0], &tri->v[1], tri->v[2].x, tri->v[2].y);
    if (fabs(area) < 1e-12) return -2;
    int minx = (int)floor(fmin(fmin(tri->v[0].x, tri->v[1].x), tri->v[2].x));
    int maxx = (int)ceil(fmax(fmax(tri->v[0].x, tri->v[1].x), tri->v[2].x));
    int miny = (int)floor(fmin(fmin(tri->v[0].y, tri->v[1].y), tri->v[2].y));
    int maxy = (int)ceil(fmax(fmax(tri->v[0].y, tri->v[1].y), tri->v[2].y));
    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    if (maxx >= t->w) maxx = t->w - 1;
    if (maxy >= t->h) maxy = t->h - 1;
    int written = 0;
    for (int y = miny; y <= maxy; ++y) {
        for (int x = minx; x <= maxx; ++x) {
            double px = x + 0.5, py = y + 0.5;
            double w0 = edge(&tri->v[1], &tri->v[2], px, py);
            double w1 = edge(&tri->v[2], &tri->v[0], px, py);
            double w2 = edge(&tri->v[0], &tri->v[1], px, py);
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            double sum = w0 + w1 + w2;
            double z = (w0 * tri->v[0].z + w1 * tri->v[1].z + w2 * tri->v[2].z) / sum;
            size_t i = (size_t)y * (size_t)t->w + (size_t)x;
            if ((float)z < t->z[i]) {
                t->z[i] = (float)z;
                t->rgb[i*3+0] = tri->rgb[0];
                t->rgb[i*3+1] = tri->rgb[1];
                t->rgb[i*3+2] = tri->rgb[2];
                ++written;
            }
        }
    }
    return written;
}

int rt_write_ppm(const RTarget *t, const char *path) {
    if (!t || !path) return -1;
    FILE *f = fopen(path, "wb");
    if (!f) return -2;
    fprintf(f, "P6\n%d %d\n255\n", t->w, t->h);
    fwrite(t->rgb, 1, (size_t)t->w * (size_t)t->h * 3, f);
    fclose(f);
    return 0;
}

/* ================= textured path ================= */

int rtt_init(RTexTarget *t, int w, int h) {
    if (!t || w <= 0 || h <= 0) return RT_ERR_ARG;
    t->w = w; t->h = h;
    size_t n = (size_t)w * (size_t)h;
    t->z   = (float*)malloc(sizeof(float) * n);
    t->u   = (double*)malloc(sizeof(double) * n);
    t->v   = (double*)malloc(sizeof(double) * n);
    t->rgb = (unsigned char*)malloc(n * 3);
    if (!t->z || !t->u || !t->v || !t->rgb) { rtt_free(t); return RT_ERR_ARG; }
    for (size_t i = 0; i < n; ++i) {
        t->z[i] = 1e30f; t->u[i] = 0.0; t->v[i] = 0.0;
        t->rgb[i*3+0] = 0; t->rgb[i*3+1] = 0; t->rgb[i*3+2] = 0;
    }
    return RT_OK;
}

void rtt_free(RTexTarget *t) {
    if (!t) return;
    free(t->z); free(t->u); free(t->v); free(t->rgb);
    t->z = NULL; t->u = NULL; t->v = NULL; t->rgb = NULL;
    t->w = 0; t->h = 0;
}

/* Vertex-level checks: non-finite coordinates and zero/non-positive depth
   are reported with the offending vertex index. */
static int rtt_validate(const RTexTri *tri) {
    for (int k = 0; k < 3; ++k) {
        const RTexVert *v = &tri->v[k];
        if (!finite6(v->x, v->y, v->z, v->w, v->u, v->v)) {
            fprintf(stderr, "rtt_triangle: vertex %d has non-finite coordinate\n", k);
            return RT_ERR_FINITE;
        }
        if (v->w <= 0.0) {
            if (v->w == 0.0)
                fprintf(stderr, "rtt_triangle: vertex %d has zero depth w\n", k);
            else
                fprintf(stderr, "rtt_triangle: vertex %d has non-positive depth w=%g\n", k, v->w);
            return RT_ERR_WZERO;
        }
    }
    double area = edge_xy(tri->v[0].x, tri->v[0].y, tri->v[1].x, tri->v[1].y,
                          tri->v[2].x, tri->v[2].y);
    if (fabs(area) < 1e-12) {
        /* name the vertex that collapses the triangle: a coincident pair,
           otherwise the middle point of a collinear triple. */
        const double eps2 = 1e-24;
        for (int k = 0; k < 3; ++k) {
            int j = (k + 1) % 3;
            double dx = tri->v[k].x - tri->v[j].x, dy = tri->v[k].y - tri->v[j].y;
            if (dx * dx + dy * dy <= eps2) {
                fprintf(stderr, "rtt_triangle: degenerate, vertex %d coincides with vertex %d\n", j, k);
                return RT_ERR_DEGEN;
            }
        }
        int mid = 1;
        for (int k = 0; k < 3; ++k) {
            int j = (k + 1) % 3, m = (k + 2) % 3;
            double dx = tri->v[j].x - tri->v[k].x, dy = tri->v[j].y - tri->v[k].y;
            double ex = tri->v[m].x - tri->v[k].x, ey = tri->v[m].y - tri->v[k].y;
            if (fabs(dx * ey - dy * ex) < 1e-12) { mid = m; break; }
        }
        fprintf(stderr, "rtt_triangle: degenerate, vertices are collinear (vertex %d lies on the line)\n", mid);
        return RT_ERR_DEGEN;
    }
    return RT_OK;
}

int rtt_triangle(RTexTarget *t, const RTexTri *tri, int correct) {
    if (!t || !tri) return RT_ERR_ARG;
    if (correct != 0 && correct != 1) return RT_ERR_ARG;
    int rc = rtt_validate(tri);
    if (rc != RT_OK) return rc;

    int minx = (int)floor(fmin(fmin(tri->v[0].x, tri->v[1].x), tri->v[2].x));
    int maxx = (int)ceil(fmax(fmax(tri->v[0].x, tri->v[1].x), tri->v[2].x));
    int miny = (int)floor(fmin(fmin(tri->v[0].y, tri->v[1].y), tri->v[2].y));
    int maxy = (int)ceil(fmax(fmax(tri->v[0].y, tri->v[1].y), tri->v[2].y));
    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    if (maxx >= t->w) maxx = t->w - 1;
    if (maxy >= t->h) maxy = t->h - 1;

    /* stage 1: perspective-correct per-vertex reciprocal depth 1/w. */
    double invw[3];
    if (correct)
        for (int k = 0; k < 3; ++k) invw[k] = 1.0 / tri->v[k].w;

    int written = 0;
    for (int y = miny; y <= maxy; ++y) {
        for (int x = minx; x <= maxx; ++x) {
            double px = x + 0.5, py = y + 0.5;
            double w0 = edge_xy(tri->v[1].x, tri->v[1].y, tri->v[2].x, tri->v[2].y, px, py);
            double w1 = edge_xy(tri->v[2].x, tri->v[2].y, tri->v[0].x, tri->v[0].y, px, py);
            double w2 = edge_xy(tri->v[0].x, tri->v[0].y, tri->v[1].x, tri->v[1].y, px, py);
            double sum = w0 + w1 + w2;
            double b0 = w0 / sum, b1 = w1 / sum, b2 = w2 / sum;
            /* shared edge tolerance: rejects only pixels genuinely outside */
            if (b0 < -RT_EDGE_EPS || b1 < -RT_EDGE_EPS || b2 < -RT_EDGE_EPS) continue;
            if (b0 < 0.0) b0 = 0.0;
            if (b1 < 0.0) b1 = 0.0;
            if (b2 < 0.0) b2 = 0.0;
            double bsum = b0 + b1 + b2;
            b0 /= bsum; b1 /= bsum; b2 /= bsum;

            double z, u, v;
            if (correct) {
                /* stage 2: corrected weights (b_i/w_i)/(sum b_j/w_j);
                   perspective-correct interpolation of z,u,v. */
                double c0 = b0 * invw[0], c1 = b1 * invw[1], c2 = b2 * invw[2];
                double csum = c0 + c1 + c2;
                c0 /= csum; c1 /= csum; c2 /= csum;
                z = c0 * tri->v[0].z + c1 * tri->v[1].z + c2 * tri->v[2].z;
                u = c0 * tri->v[0].u + c1 * tri->v[1].u + c2 * tri->v[2].u;
                v = c0 * tri->v[0].v + c1 * tri->v[1].v + c2 * tri->v[2].v;
            } else {
                /* correction off: affine screen-space weights, identical
                   to the per-pixel math of the original rt_triangle. */
                z = b0 * tri->v[0].z + b1 * tri->v[1].z + b2 * tri->v[2].z;
                u = b0 * tri->v[0].u + b1 * tri->v[1].u + b2 * tri->v[2].u;
                v = b0 * tri->v[0].v + b1 * tri->v[1].v + b2 * tri->v[2].v;
            }

            size_t i = (size_t)y * (size_t)t->w + (size_t)x;
            if ((float)z < t->z[i]) {
                t->z[i] = (float)z;
                t->u[i] = u; t->v[i] = v;
                t->rgb[i*3+0] = tri->rgb[0];
                t->rgb[i*3+1] = tri->rgb[1];
                t->rgb[i*3+2] = tri->rgb[2];
                ++written;
            }
        }
    }
    return written;
}

int rtt_project(const RCamVert *cv, double fx, double fy, double cx, double cy,
                RTexVert *out) {
    if (!cv || !out) return RT_ERR_ARG;
    if (!finite5(cv->x, cv->y, cv->z, cv->u, cv->v) ||
        !isfinite(fx) || !isfinite(fy) || !isfinite(cx) || !isfinite(cy)) {
        fprintf(stderr, "rtt_project: non-finite input\n");
        return RT_ERR_FINITE;
    }
    if (cv->z <= 0.0) {
        if (cv->z == 0.0)
            fprintf(stderr, "rtt_project: camera vertex has zero depth z\n");
        else
            fprintf(stderr, "rtt_project: camera vertex behind camera z=%g\n", cv->z);
        return RT_ERR_WZERO;
    }
    out->x = fx * cv->x / cv->z + cx;
    out->y = fy * cv->y / cv->z + cy;
    out->z = cv->z;          /* depth-buffer stores camera-space depth */
    out->w = cv->z;          /* clip-space w equals camera depth */
    out->u = cv->u;
    out->v = cv->v;
    return RT_OK;
}

int rtt_unproject(const RCamTri *ct, double fx, double fy, double cx, double cy,
                  double px, double py, double uv_out[2]) {
    if (!ct || !uv_out) return RT_ERR_ARG;
    if (!finite6(fx, fy, cx, cy, px, py)) {
        fprintf(stderr, "rtt_unproject: non-finite camera intrinsics or pixel\n");
        return RT_ERR_FINITE;
    }
    for (int k = 0; k < 3; ++k) {
        const RCamVert *v = &ct->v[k];
        if (!finite5(v->x, v->y, v->z, v->u, v->v)) {
            fprintf(stderr, "rtt_unproject: non-finite input at vertex %d\n", k);
            return RT_ERR_FINITE;
        }
        if (v->z <= 0.0) {
            fprintf(stderr, "rtt_unproject: vertex %d has non-positive depth z=%g\n", k, v->z);
            return RT_ERR_WZERO;
        }
    }
    /* ray through the pixel center in camera space (camera at origin). */
    double dx = (px - cx) / fx, dy = (py - cy) / fy;
    double rx = dx, ry = dy, rz = 1.0;
    double ax = ct->v[0].x, ay = ct->v[0].y, az = ct->v[0].z;
    double e1x = ct->v[1].x - ax, e1y = ct->v[1].y - ay, e1z = ct->v[1].z - az;
    double e2x = ct->v[2].x - ax, e2y = ct->v[2].y - ay, e2z = ct->v[2].z - az;
    /* solve r*t = a + e1*b1 + e2*b2 via cross products (Moller-Trumbore). */
    double pxv = ry * e2z - rz * e2y;
    double pyv = rz * e2x - rx * e2z;
    double pzv = rx * e2y - ry * e2x;
    double det = e1x * pxv + e1y * pyv + e1z * pzv;
    if (fabs(det) < 1e-15) {
        fprintf(stderr, "rtt_unproject: triangle degenerate or edge-on to the ray\n");
        return RT_ERR_DEGEN;
    }
    double inv = 1.0 / det;
    double tvx = -ax, tvy = -ay, tvz = -az;
    double b1 = (tvx * pxv + tvy * pyv + tvz * pzv) * inv;
    double qx = tvy * e1z - tvz * e1y;
    double qy = tvz * e1x - tvx * e1z;
    double qz = tvx * e1y - tvy * e1x;
    double b2 = (rx * qx + ry * qy + rz * qz) * inv;
    double b0 = 1.0 - b1 - b2;
    double tt = (e2x * qx + e2y * qy + e2z * qz) * inv;
    /* tolerance matches the rasterizer's inclusive edge test */
    if (tt <= 0.0 || b0 < -RT_EDGE_EPS || b1 < -RT_EDGE_EPS || b2 < -RT_EDGE_EPS) return 0;
    if (b0 < 0.0) b0 = 0.0;
    if (b1 < 0.0) b1 = 0.0;
    if (b2 < 0.0) b2 = 0.0;
    double s = b0 + b1 + b2; b0 /= s; b1 /= s; b2 /= s;
    uv_out[0] = b0 * ct->v[0].u + b1 * ct->v[1].u + b2 * ct->v[2].u;
    uv_out[1] = b0 * ct->v[0].v + b1 * ct->v[1].v + b2 * ct->v[2].v;
    return 1;
}

int rtt_write_ppm(const RTexTarget *t, const char *path) {
    if (!t || !path) return RT_ERR_ARG;
    FILE *f = fopen(path, "wb");
    if (!f) return -2;
    fprintf(f, "P6\n%d %d\n255\n", t->w, t->h);
    fwrite(t->rgb, 1, (size_t)t->w * (size_t)t->h * 3, f);
    fclose(f);
    return RT_OK;
}
