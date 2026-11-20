#define _POSIX_C_SOURCE 199309L
#include "raster.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

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

static int target_ok(const RTarget *t) {
    return t && t->w > 0 && t->h > 0 && t->z && t->rgb;
}

/* 计算裁剪到目标内的包围盒；完全在屏外返回 0 */
static int bbox(const RTarget *t, const RTri *tri, int *minx, int *maxx, int *miny, int *maxy) {
    int x0 = (int)floor(fmin(fmin(tri->v[0].x, tri->v[1].x), tri->v[2].x));
    int x1 = (int)ceil (fmax(fmax(tri->v[0].x, tri->v[1].x), tri->v[2].x));
    int y0 = (int)floor(fmin(fmin(tri->v[0].y, tri->v[1].y), tri->v[2].y));
    int y1 = (int)ceil (fmax(fmax(tri->v[0].y, tri->v[1].y), tri->v[2].y));
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= t->w) x1 = t->w - 1;
    if (y1 >= t->h) y1 = t->h - 1;
    if (x0 > x1 || y0 > y1) return 0;
    *minx = x0; *maxx = x1; *miny = y0; *maxy = y1;
    return 1;
}

/* 第二阶段：边测试 + 深度测试 + 着色，行为与改造前的 rt_triangle 完全一致。
   *inside 收通过边测试的像素数，可为 NULL。返回实际写入像素数，<0 为错误码。*/
static int shade_triangle(RTarget *t, const RTri *tri, int minx, int maxx, int miny, int maxy,
                          int *inside) {
    int written = 0, in = 0;
    for (int y = miny; y <= maxy; ++y) {
        for (int x = minx; x <= maxx; ++x) {
            double px = x + 0.5, py = y + 0.5;
            double w0 = edge(&tri->v[1], &tri->v[2], px, py);
            double w1 = edge(&tri->v[2], &tri->v[0], px, py);
            double w2 = edge(&tri->v[0], &tri->v[1], px, py);
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            ++in;
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
    if (inside) *inside = in;
    return written;
}

/* 第一阶段：只读深度探测，不写任何缓冲。
   返回能通过深度测试的像素数，0 = 整个三角形被遮挡；<0 为错误码。
   *inside（非 NULL 时）收通过边测试的像素数，两种模式口径一致。*/
static int probe_triangle(const RTarget *t, const RTri *tri, int minx, int maxx, int miny, int maxy,
                          int *inside) {
    int pass = 0, in = 0;
    for (int y = miny; y <= maxy; ++y) {
        for (int x = minx; x <= maxx; ++x) {
            double px = x + 0.5, py = y + 0.5;
            double w0 = edge(&tri->v[1], &tri->v[2], px, py);
            double w1 = edge(&tri->v[2], &tri->v[0], px, py);
            double w2 = edge(&tri->v[0], &tri->v[1], px, py);
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            ++in;
            double sum = w0 + w1 + w2;
            double z = (w0 * tri->v[0].z + w1 * tri->v[1].z + w2 * tri->v[2].z) / sum;
            size_t i = (size_t)y * (size_t)t->w + (size_t)x;
            if ((float)z < t->z[i]) ++pass;
        }
    }
    if (inside) *inside = in;
    return pass;
}

int rt_triangle(RTarget *t, const RTri *tri) {
    if (!target_ok(t) || !tri) return -1;
    for (int k = 0; k < 3; ++k)
        if (!isfinite(tri->v[k].z)) return -3;
    double area = edge(&tri->v[0], &tri->v[1], tri->v[2].x, tri->v[2].y);
    if (fabs(area) < 1e-12) return -2;
    int minx, maxx, miny, maxy;
    if (!bbox(t, tri, &minx, &maxx, &miny, &maxy)) return 0;
    return shade_triangle(t, tri, minx, maxx, miny, maxy, NULL);
}

int rt_tri_visible(const RTarget *t, const RTri *tri) {
    if (!target_ok(t) || !tri) return -1;
    for (int k = 0; k < 3; ++k)
        if (!isfinite(tri->v[k].z)) return -3;
    double area = edge(&tri->v[0], &tri->v[1], tri->v[2].x, tri->v[2].y);
    if (fabs(area) < 1e-12) return -2;
    int minx, maxx, miny, maxy;
    if (!bbox(t, tri, &minx, &maxx, &miny, &maxy)) return 0;
    return probe_triangle(t, tri, minx, maxx, miny, maxy, NULL);
}

void rt_stats_init(RStats *st) {
    if (!st) return;
    st->tris_total = 0;
    st->tris_early_out = 0;
    st->tris_degenerate = 0;
    st->pixels_inside = 0;
    st->pixels_probed = 0;
    st->pixels_shaded = 0;
    st->pixels_filled = 0;
    st->elapsed_sec = 0.0;
    st->error_index = -1;
}

int rt_render(RTarget *t, const RTri *tris, int n, int enable_early, RStats *st) {
    RStats local;
    if (!st) { st = &local; }
    rt_stats_init(st);
    /* 注意：缓冲是否为空在循环内逐三角形检查，以便指出三角形号 */
    if (!tris || n < 0 || !t) return -1;

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    for (int idx = 0; idx < n; ++idx) {
        const RTri *tri = &tris[idx];
        ++st->tris_total;

        if (!target_ok(t)) {
            st->error_index = idx;
            fprintf(stderr, "rt_render: triangle %d: target buffer is empty\n", idx);
            return -4;
        }
        for (int k = 0; k < 3; ++k) {
            if (!isfinite(tri->v[k].z)) {
                st->error_index = idx;
                fprintf(stderr, "rt_render: triangle %d: non-finite depth z=%.17g at vertex %d\n",
                        idx, tri->v[k].z, k);
                return -3;
            }
        }

        double area = edge(&tri->v[0], &tri->v[1], tri->v[2].x, tri->v[2].y);
        if (fabs(area) < 1e-12) { ++st->tris_degenerate; continue; }

        int minx, maxx, miny, maxy;
        if (!bbox(t, tri, &minx, &maxx, &miny, &maxy)) continue;

        if (enable_early) {
            /* 第一阶段：深度提前判可见，只读 */
            int inside = 0;
            int pass = probe_triangle(t, tri, minx, maxx, miny, maxy, &inside);
            st->pixels_probed += inside;
            if (pass == 0) { ++st->tris_early_out; continue; }
        }

        /* 第二阶段：只给通过者着色（关闭早退时每个三角形都走这里）*/
        int inside = 0;
        int written = shade_triangle(t, tri, minx, maxx, miny, maxy, &inside);
        st->pixels_shaded += inside;
        st->pixels_inside = st->pixels_probed + st->pixels_shaded;
        st->pixels_filled += written;
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    st->elapsed_sec = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
    return 0;
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
