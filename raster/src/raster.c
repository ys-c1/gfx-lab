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

int rt_write_ppm(const RTarget *t, const char *path) {
    if (!t || !path) return -1;
    FILE *f = fopen(path, "wb");
    if (!f) return -2;
    fprintf(f, "P6\n%d %d\n255\n", t->w, t->h);
    fwrite(t->rgb, 1, (size_t)t->w * (size_t)t->h * 3, f);
    fclose(f);
    return 0;
}

static int rl_err(RLine *ln, const char *field) {
    snprintf(ln->err, sizeof ln->err, "%s", field);
    return -1;
}

static int rl_validate(RLine *ln) {
    if (!isfinite(ln->x0)) return rl_err(ln, "x0");
    if (!isfinite(ln->y0)) return rl_err(ln, "y0");
    if (!isfinite(ln->x1)) return rl_err(ln, "x1");
    if (!isfinite(ln->y1)) return rl_err(ln, "y1");
    if (!(ln->width >= RLINE_W_MIN && ln->width <= RLINE_W_MAX)) return rl_err(ln, "width");
    if (ln->levels < RLINE_LEVELS_MIN || ln->levels > RLINE_LEVELS_MAX) return rl_err(ln, "levels");
    if (ln->aa != 0 && ln->aa != 1) return rl_err(ln, "aa");
    return 0;
}

static void rl_put(RTarget *t, int x, int y, const unsigned char rgb[3]) {
    if (x < 0 || y < 0 || x >= t->w || y >= t->h) return;
    size_t i = ((size_t)y * (size_t)t->w + (size_t)x) * 3;
    t->rgb[i+0] = rgb[0]; t->rgb[i+1] = rgb[1]; t->rgb[i+2] = rgb[2];
}

/* 基线：1 像素 Bresenham，aa=0 时走这里，保证与旧画面逐像素一致。 */
static int rl_bresenham(RTarget *t, int x0, int y0, int x1, int y1, const unsigned char rgb[3]) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy, n = 0;
    for (;;) {
        if (x0 >= 0 && y0 >= 0 && x0 < t->w && y0 < t->h) { rl_put(t, x0, y0, rgb); ++n; }
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
    return n;
}

int rt_line(RTarget *t, RLine *ln) {
    if (!t || !ln) return -2;
    ln->err[0] = '\0';
    int v = rl_validate(ln);
    if (v) return v;
    double dx = ln->x1 - ln->x0, dy = ln->y1 - ln->y0;
    double len = hypot(dx, dy);
    if (ln->aa == 0 || len < 1e-9) {
        /* 退化点与基线都画单像素点/线 */
        return rl_bresenham(t, (int)floor(ln->x0 + 0.5), (int)floor(ln->y0 + 0.5),
                            (int)floor(ln->x1 + 0.5), (int)floor(ln->y1 + 0.5), ln->rgb);
    }
    /* 第一段：按斜率算覆盖宽度（垂直方向等效像素宽） */
    double amaj = fmax(fabs(dx), fabs(dy));
    double wcov = ln->width * len / amaj;
    int xmajor = fabs(dx) >= fabs(dy);
    double a0 = xmajor ? ln->x0 : ln->y0, a1 = xmajor ? ln->x1 : ln->y1;
    double b0 = xmajor ? ln->y0 : ln->x0, b1 = xmajor ? ln->y1 : ln->x1;
    if (a0 > a1) { double tmp = a0; a0 = a1; a1 = tmp; tmp = b0; b0 = b1; b1 = tmp; }
    unsigned char *grade = (unsigned char*)calloc((size_t)t->w * (size_t)t->h, 1);
    if (!grade) return -2;
    double half = wcov * 0.5, da = a1 - a0;
    int covered = 0;
    for (int m = (int)floor(a0); m <= (int)floor(a1); ++m) {
        double s = m + 0.5;                 /* 主轴像素中心 */
        if (s < a0 || s > a1) continue;
        double c = b0 + (b1 - b0) * (s - a0) / da;
        double lo = c - half, hi = c + half;
        /* 第二段：按覆盖率把像素分成 levels 档 */
        for (int j = (int)floor(lo); j <= (int)ceil(hi) - 1; ++j) {
            double cov = fmin(hi, j + 1.0) - fmax(lo, (double)j);
            if (cov <= 0.0) continue;
            if (cov > 1.0) cov = 1.0;
            int g = (int)(cov * (ln->levels - 1) + 0.5);
            if (g <= 0) continue;
            int inten = g * 255 / (ln->levels - 1);
            int x = xmajor ? m : j, y = xmajor ? j : m;
            if (x < 0 || y < 0 || x >= t->w || y >= t->h) continue;
            size_t i = (size_t)y * (size_t)t->w + (size_t)x;
            if (inten > grade[i]) grade[i] = (unsigned char)inten;
        }
    }
    for (int y = 0; y < t->h; ++y) {
        for (int x = 0; x < t->w; ++x) {
            size_t i = (size_t)y * (size_t)t->w + (size_t)x;
            int g = grade[i];
            if (!g) continue;
            ++covered;
            for (int k = 0; k < 3; ++k) {
                int val = ln->rgb[k] * g / 255;
                if (val > t->rgb[i*3+k]) t->rgb[i*3+k] = (unsigned char)val;
            }
        }
    }
    free(grade);
    return covered;
}
