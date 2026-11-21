#include "raster.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

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

/* 第一段：按顶点色调解出系数 */
int rt_tint_solve(const RTri *tri, const RTint *tint, RTintPlan *plan, char *err, size_t errcap) {
    if (err && errcap) err[0] = '\0';
    if (!tri || !tint || !plan) {
        if (err && errcap) snprintf(err, errcap, "rt_tint_solve: null argument (tri=%p tint=%p plan=%p)",
                                    (void*)tri, (void*)tint, (void*)plan);
        return -1;
    }
    if (!tint->table || tint->n == 0) {
        if (err && errcap) snprintf(err, errcap, "rt_tint_solve: tint table is empty");
        return -1;
    }
    for (int i = 0; i < 3; ++i) {
        if (tint->vi[i] < 0 || (size_t)tint->vi[i] >= tint->n) {
            if (err && errcap) snprintf(err, errcap, "vertex %d: index %d out of tint table range [0,%zu)",
                                        i, tint->vi[i], tint->n);
            return -3;
        }
    }
    for (int i = 0; i < 3; ++i) {
        double tv = tint->table[tint->vi[i]];
        if (!isfinite(tv) || tv < 0.0 || tv > 1.0) {
            if (err && errcap) snprintf(err, errcap, "vertex %d: tint table[%d]=%.17g not in [0,1]",
                                        i, tint->vi[i], tv);
            return -4;
        }
    }
    double area = edge(&tri->v[0], &tri->v[1], tri->v[2].x, tri->v[2].y);
    if (fabs(area) < 1e-12) {
        if (err && errcap) snprintf(err, errcap, "rt_tint_solve: degenerate triangle (area=%g)", area);
        return -2;
    }
    for (int i = 0; i < 3; ++i) {
        const RVert *p = &tri->v[(i + 1) % 3];
        const RVert *q = &tri->v[(i + 2) % 3];
        plan->a[i] = (p->y - q->y) / area;
        plan->b[i] = (q->x - p->x) / area;
        plan->c[i] = (p->x * q->y - q->x * p->y) / area;
        plan->tint[i] = tint->table[tint->vi[i]];
        plan->vi[i] = tint->vi[i];
    }
    return 0;
}

/* 第二段：按重心坐标插值 */
void rt_tint_weights(const RTintPlan *plan, double px, double py, double w[3]) {
    for (int i = 0; i < 3; ++i)
        w[i] = plan->a[i] * px + plan->b[i] * py + plan->c[i];
}

double rt_tint_interp(const RTintPlan *plan, const double w[3]) {
    return w[0] * plan->tint[0] + w[1] * plan->tint[1] + w[2] * plan->tint[2];
}

/* 第三段：乘到最终颜色 */
static unsigned char apply_tint(unsigned char c, double k) {
    int v = (int)lround(c * k);
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    return (unsigned char)v;
}

static void bbox_of(const RTri *tri, const RTarget *t, int *minx, int *miny, int *maxx, int *maxy) {
    *minx = (int)floor(fmin(fmin(tri->v[0].x, tri->v[1].x), tri->v[2].x));
    *maxx = (int)ceil(fmax(fmax(tri->v[0].x, tri->v[1].x), tri->v[2].x));
    *miny = (int)floor(fmin(fmin(tri->v[0].y, tri->v[1].y), tri->v[2].y));
    *maxy = (int)ceil(fmax(fmax(tri->v[0].y, tri->v[1].y), tri->v[2].y));
    if (*minx < 0) *minx = 0;
    if (*miny < 0) *miny = 0;
    if (*maxx >= t->w) *maxx = t->w - 1;
    if (*maxy >= t->h) *maxy = t->h - 1;
}

int rt_triangle_tint(RTarget *t, const RTri *tri, const RTint *tint) {
    if (!tint) return rt_triangle(t, tri); /* 不启用：输出保持原样 */
    if (!t || !tri) return -1;
    RTintPlan plan;
    int rc = rt_tint_solve(tri, tint, &plan, NULL, 0);
    if (rc != 0) return rc;
    int minx, miny, maxx, maxy;
    bbox_of(tri, t, &minx, &miny, &maxx, &maxy);
    int written = 0;
    for (int y = miny; y <= maxy; ++y) {
        for (int x = minx; x <= maxx; ++x) {
            double w[3];
            rt_tint_weights(&plan, x + 0.5, y + 0.5, w);
            if (w[0] < 0 || w[1] < 0 || w[2] < 0) continue;
            double z = w[0] * tri->v[0].z + w[1] * tri->v[1].z + w[2] * tri->v[2].z;
            size_t i = (size_t)y * (size_t)t->w + (size_t)x;
            if ((float)z < t->z[i]) {
                double k = rt_tint_interp(&plan, w);
                t->z[i] = (float)z;
                t->rgb[i*3+0] = apply_tint(tri->rgb[0], k);
                t->rgb[i*3+1] = apply_tint(tri->rgb[1], k);
                t->rgb[i*3+2] = apply_tint(tri->rgb[2], k);
                ++written;
            }
        }
    }
    return written;
}

int rt_triangle_tint_bf(RTarget *t, const RTri *tri, const RTint *tint) {
    if (!tint) return rt_triangle(t, tri);
    if (!t || !tri) return -1;
    RTintPlan plan; /* 仅用于校验与取色调，插值每像素重算 */
    int rc = rt_tint_solve(tri, tint, &plan, NULL, 0);
    if (rc != 0) return rc;
    int minx, miny, maxx, maxy;
    bbox_of(tri, t, &minx, &miny, &maxx, &maxy);
    int written = 0;
    for (int y = miny; y <= maxy; ++y) {
        for (int x = minx; x <= maxx; ++x) {
            double px = x + 0.5, py = y + 0.5;
            double w0 = edge(&tri->v[1], &tri->v[2], px, py);
            double w1 = edge(&tri->v[2], &tri->v[0], px, py);
            double w2 = edge(&tri->v[0], &tri->v[1], px, py);
            double sum = w0 + w1 + w2;
            w0 /= sum; w1 /= sum; w2 /= sum;
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            double z = w0 * tri->v[0].z + w1 * tri->v[1].z + w2 * tri->v[2].z;
            size_t i = (size_t)y * (size_t)t->w + (size_t)x;
            if ((float)z < t->z[i]) {
                double k = w0 * plan.tint[0] + w1 * plan.tint[1] + w2 * plan.tint[2];
                t->z[i] = (float)z;
                t->rgb[i*3+0] = apply_tint(tri->rgb[0], k);
                t->rgb[i*3+1] = apply_tint(tri->rgb[1], k);
                t->rgb[i*3+2] = apply_tint(tri->rgb[2], k);
                ++written;
            }
        }
    }
    return written;
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
