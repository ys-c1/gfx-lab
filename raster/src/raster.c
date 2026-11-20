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

int rt_triangle_ex(RTarget *t, const RTri *tri, const RDepthMask *mask, int tri_id) {
    if (!t || !tri) return -1;
    if (t->w <= 0 || t->h <= 0 || !t->z || !t->rgb) {
        fprintf(stderr, "rt_triangle_ex: triangle %d: target buffer is empty\n", tri_id);
        return -3;
    }
    int mask_on = 0, write_depth = 1;
    if (mask) {
        if (mask->enabled != 0 && mask->enabled != 1) {
            fprintf(stderr, "rt_triangle_ex: triangle %d: invalid depth mask (enabled=%d)\n",
                    tri_id, mask->enabled);
            return -4;
        }
        if (mask->enabled) {
            if (mask->write_depth != 0 && mask->write_depth != 1) {
                fprintf(stderr, "rt_triangle_ex: triangle %d: invalid depth mask (write_depth=%d)\n",
                        tri_id, mask->write_depth);
                return -4;
            }
            mask_on = 1;
            write_depth = mask->write_depth;
        }
    }
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
            /* 第一段：深度测试 */
            if ((float)z >= t->z[i]) continue;
            /* 第二段：按掩码决定写不写深度；颜色照常写 */
            if (!mask_on || write_depth) t->z[i] = (float)z;
            t->rgb[i*3+0] = tri->rgb[0];
            t->rgb[i*3+1] = tri->rgb[1];
            t->rgb[i*3+2] = tri->rgb[2];
            ++written;
        }
    }
    return written;
}

int rt_triangle(RTarget *t, const RTri *tri) {
    return rt_triangle_ex(t, tri, NULL, -1);
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
