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

int rt_triangle(RTarget *t, const RTri *tri) {
    if (!t || !tri) return -1;
    double area = edge(&tri->v[0], &tri->v[1], tri->v[2].x, tri->v[2].y);
    if (fabs(area) < 1e-12) return -2;
    int minx = (int)floor(fmin(fmin(tri->v[0].x, tri->v[1].x), tri->v[2].x));
    int maxx = (int)ceil(fmax(fmax(tri->v[0].x, tri->v[1].x), tri->v[2].x));
    int miny = (int)floor(fmin(fmin(tri->v[0].y, tri->v[1].y), tri->v[2].y));
    int maxy = (int)ceil(fmax(fmax(tri->v[0].y, tri->v[1].y), tri->v[2].y));
    if (minx < 0) minx = 0; if (miny < 0) miny = 0;
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

int rt_sprites(RTarget *t, const RSprite *sp, int n, double focal, int *bad) {
    if (bad) *bad = -1;
    if (!t || n < 0 || (n > 0 && !sp)) return -1;
    if (focal < 0.0 || !isfinite(focal)) return -1;
    for (int i = 0; i < n; ++i) {
        const RSprite *s = &sp[i];
        if (!(s->size > 0.0)) { if (bad) *bad = i; return -1; }
        if (focal > 0.0 && !isfinite(s->z)) { if (bad) *bad = i; return -1; }
        double ss = focal > 0.0 ? s->size * focal / s->z : s->size;
        if (!isfinite(ss) || ss <= 0.0) { if (bad) *bad = i; return -1; }
        double x0 = s->x - ss * 0.5, x1 = s->x + ss * 0.5;
        double y0 = s->y - ss * 0.5, y1 = s->y + ss * 0.5;
        if (x1 <= 0.0 || y1 <= 0.0 || x0 >= t->w || y0 >= t->h) continue;
        int px0 = x0 > 0.0 ? (int)floor(x0) : 0;
        int py0 = y0 > 0.0 ? (int)floor(y0) : 0;
        int px1 = x1 < t->w ? (int)ceil(x1) - 1 : t->w - 1;
        int py1 = y1 < t->h ? (int)ceil(y1) - 1 : t->h - 1;
        for (int py = py0; py <= py1; ++py) {
            double oy = fmin(y1, py + 1.0) - fmax(y0, (double)py);
            if (oy <= 0.0) continue;
            for (int px = px0; px <= px1; ++px) {
                double ox = fmin(x1, px + 1.0) - fmax(x0, (double)px);
                if (ox <= 0.0) continue;
                double c = ox * oy;
                size_t idx = (size_t)py * (size_t)t->w + (size_t)px;
                if (!((float)s->z < t->z[idx])) continue;
                t->z[idx] = (float)s->z;
                for (int k = 0; k < 3; ++k) {
                    double v = c * s->rgb[k] + (1.0 - c) * t->rgb[idx * 3 + k];
                    t->rgb[idx * 3 + k] = (unsigned char)(v + 0.5);
                }
            }
        }
    }
    return n;
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
