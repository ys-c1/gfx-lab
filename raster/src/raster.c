#define _POSIX_C_SOURCE 199309L
#include "raster.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

void rt_config_default(RConfig *cfg) {
    if (!cfg) return;
    cfg->enable_prepass = 0;
    cfg->track_stats = 0;
    cfg->depth_eps = 0.0f;
}

int rt_config_validate(const RConfig *cfg, char *err_field, size_t err_field_cap) {
    if (!cfg) return -1;
    const char *bad = NULL;
    if (cfg->enable_prepass != 0 && cfg->enable_prepass != 1) bad = "enable_prepass";
    else if (cfg->track_stats != 0 && cfg->track_stats != 1) bad = "track_stats";
    else if (!isfinite(cfg->depth_eps) || cfg->depth_eps < 0.0f || cfg->depth_eps > 1.0f)
        bad = "depth_eps";
    if (bad) {
        if (err_field && err_field_cap) snprintf(err_field, err_field_cap, "%s", bad);
        return -3;
    }
    return 0;
}

int rt_init(RTarget *t, int w, int h) {
    if (!t || w <= 0 || h <= 0) return -1;
    t->w = w; t->h = h;
    t->z = NULL; t->rgb = NULL; t->zprev = NULL;
    rt_config_default(&t->cfg);
    rt_reset_stats(t);
    t->z = (float*)malloc(sizeof(float) * (size_t)w * (size_t)h);
    t->rgb = (unsigned char*)malloc((size_t)w * (size_t)h * 3);
    if (!t->z || !t->rgb) { rt_free(t); return -1; }
    for (int i = 0; i < w * h; ++i) {
        t->z[i] = 1e30f;
        t->rgb[i*3+0] = 0;
        t->rgb[i*3+1] = 0;
        t->rgb[i*3+2] = 0;
    }
    return 0;
}

void rt_free(RTarget *t) {
    if (!t) return;
    free(t->z); free(t->rgb); free(t->zprev);
    t->z = NULL; t->rgb = NULL; t->zprev = NULL;
    t->w = 0; t->h = 0;
}

int rt_clear(RTarget *t) {
    if (!t) return -1;
    for (int i = 0; i < t->w * t->h; ++i) {
        t->z[i] = 1e30f;
        t->rgb[i*3+0] = 0;
        t->rgb[i*3+1] = 0;
        t->rgb[i*3+2] = 0;
    }
    rt_reset_stats(t);
    return 0;
}

int rt_set_config(RTarget *t, const RConfig *cfg, char *err_field, size_t err_field_cap) {
    if (!t || !cfg) return -1;
    int rc = rt_config_validate(cfg, err_field, err_field_cap);
    if (rc != 0) return rc;
    if (cfg->enable_prepass && !t->zprev) {
        t->zprev = (float*)malloc(sizeof(float) * (size_t)t->w * (size_t)t->h);
        if (!t->zprev) return -4;
    }
    t->cfg = *cfg;
    rt_reset_stats(t);
    return 0;
}

void rt_reset_stats(RTarget *t) {
    if (!t) return;
    memset(&t->stats, 0, sizeof(t->stats));
}

int rt_get_stats(const RTarget *t, RStats *out) {
    if (!t || !out) return -1;
    *out = t->stats;
    return 0;
}

static double edge(const RVert *a, const RVert *b, double px, double py) {
    return (b->x - a->x) * (py - a->y) - (b->y - a->y) * (px - a->x);
}

typedef struct { int minx, maxx, miny, maxy; } BBox;

static int bbox_of(RTarget *t, const RTri *tri, BBox *b) {
    b->minx = (int)floor(fmin(fmin(tri->v[0].x, tri->v[1].x), tri->v[2].x));
    b->maxx = (int)ceil(fmax(fmax(tri->v[0].x, tri->v[1].x), tri->v[2].x));
    b->miny = (int)floor(fmin(fmin(tri->v[0].y, tri->v[1].y), tri->v[2].y));
    b->maxy = (int)ceil(fmax(fmax(tri->v[0].y, tri->v[1].y), tri->v[2].y));
    if (b->minx < 0) b->minx = 0;
    if (b->miny < 0) b->miny = 0;
    if (b->maxx >= t->w) b->maxx = t->w - 1;
    if (b->maxy >= t->h) b->maxy = t->h - 1;
    return b->minx <= b->maxx && b->miny <= b->maxy;
}

/* 单遍：深度+颜色同一循环, 即历史实现, 保持逐像素行为不变 */
static int draw_single(RTarget *t, const RTri *tri, const BBox *b) {
    int written = 0;
    double t0 = t->cfg.track_stats ? now_sec() : 0.0;
    for (int y = b->miny; y <= b->maxy; ++y) {
        for (int x = b->minx; x <= b->maxx; ++x) {
            double px = x + 0.5, py = y + 0.5;
            double w0 = edge(&tri->v[1], &tri->v[2], px, py);
            double w1 = edge(&tri->v[2], &tri->v[0], px, py);
            double w2 = edge(&tri->v[0], &tri->v[1], px, py);
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            ++t->stats.covered;
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
    if (t->cfg.track_stats) t->stats.color_sec += now_sec() - t0;
    t->stats.depth_hits += written;
    t->stats.written += written;
    return written;
}

/*
 * 两段式:
 *   预通道  只写深度; 同时把进入测试前的旧深度快照到 zprev
 *   颜色遍  按快照的旧深度拒绝被遮像素, 只写颜色, 不再触碰深度缓冲
 * depth_eps=0 时拒绝条件与单遍的 (float)z < zbuf 位精确等价。
 */
static int draw_prepass(RTarget *t, const RTri *tri, const BBox *b) {
    float eps = t->cfg.depth_eps;
    double t0 = t->cfg.track_stats ? now_sec() : 0.0;
    for (int y = b->miny; y <= b->maxy; ++y) {
        for (int x = b->minx; x <= b->maxx; ++x) {
            double px = x + 0.5, py = y + 0.5;
            double w0 = edge(&tri->v[1], &tri->v[2], px, py);
            double w1 = edge(&tri->v[2], &tri->v[0], px, py);
            double w2 = edge(&tri->v[0], &tri->v[1], px, py);
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            ++t->stats.covered;
            double sum = w0 + w1 + w2;
            double z = (w0 * tri->v[0].z + w1 * tri->v[1].z + w2 * tri->v[2].z) / sum;
            size_t i = (size_t)y * (size_t)t->w + (size_t)x;
            t->zprev[i] = t->z[i];
            if ((float)z < t->z[i]) {
                t->z[i] = (float)z;
                ++t->stats.depth_hits;
            }
        }
    }
    if (t->cfg.track_stats) t->stats.prepass_sec += now_sec() - t0;

    int written = 0;
    t0 = t->cfg.track_stats ? now_sec() : 0.0;
    for (int y = b->miny; y <= b->maxy; ++y) {
        for (int x = b->minx; x <= b->maxx; ++x) {
            double px = x + 0.5, py = y + 0.5;
            double w0 = edge(&tri->v[1], &tri->v[2], px, py);
            double w1 = edge(&tri->v[2], &tri->v[0], px, py);
            double w2 = edge(&tri->v[0], &tri->v[1], px, py);
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            double sum = w0 + w1 + w2;
            double z = (w0 * tri->v[0].z + w1 * tri->v[1].z + w2 * tri->v[2].z) / sum;
            size_t i = (size_t)y * (size_t)t->w + (size_t)x;
            if (!((float)z < t->zprev[i] - eps)) {
                ++t->stats.rejected;
                continue;
            }
            t->rgb[i*3+0] = tri->rgb[0];
            t->rgb[i*3+1] = tri->rgb[1];
            t->rgb[i*3+2] = tri->rgb[2];
            ++written;
        }
    }
    if (t->cfg.track_stats) t->stats.color_sec += now_sec() - t0;
    t->stats.written += written;
    return written;
}

int rt_triangle(RTarget *t, const RTri *tri) {
    if (!t || !tri) return -1;
    double area = edge(&tri->v[0], &tri->v[1], tri->v[2].x, tri->v[2].y);
    if (fabs(area) < 1e-12) return -2;
    BBox b;
    if (!bbox_of(t, tri, &b)) return 0;
    if (t->cfg.enable_prepass) {
        if (!t->zprev) return -4;
        return draw_prepass(t, tri, &b);
    }
    return draw_single(t, tri, &b);
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
