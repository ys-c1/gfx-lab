#include "shadow.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

int sh_init(ShadowMap *m, int w, int h) {
    return sh_init_ex(m, w, h, NULL);
}

int sh_init_ex(ShadowMap *m, int w, int h, const char **err_field) {
    if (err_field) *err_field = NULL;
    if (!m) { if (err_field) *err_field = "m"; return SH_ERR_ARG; }
    m->buf = NULL; m->w = 0; m->h = 0;
    if (w <= 0) { if (err_field) *err_field = "w"; return SH_ERR_SIZE; }
    if (h <= 0) { if (err_field) *err_field = "h"; return SH_ERR_SIZE; }
    m->buf = (float*)calloc((size_t)w * (size_t)h, sizeof(float));
    if (!m->buf) { if (err_field) *err_field = "buf"; return SH_ERR_ARG; }
    m->w = w; m->h = h;
    return SH_OK;
}

void sh_free(ShadowMap *m) { if (m) { free(m->buf); m->buf = NULL; m->w = 0; m->h = 0; } }

int sh_clear(ShadowMap *m) {
    if (!m) return SH_ERR_ARG;
    for (int i = 0; i < m->w * m->h; ++i) m->buf[i] = 0.0f;
    return SH_OK;
}

static int blocked(const ShSeg *s, float lx, float ly, float px, float py) {
    float rx = px - lx, ry = py - ly;
    float sx = s->x1 - s->x0, sy = s->y1 - s->y0;
    float den = rx * sy - ry * sx;
    if (fabsf(den) < 1e-9f) return 0;
    float t = ((s->x0 - lx) * sy - (s->y0 - ly) * sx) / den;
    float u = ((s->x0 - lx) * ry - (s->y0 - ly) * rx) / den;
    return t > 0.0f && t < 1.0f && u >= 0.0f && u <= 1.0f;
}

static float shade(const ShLight *light, float px, float py) {
    float dx = px - light->x, dy = py - light->y;
    float d = sqrtf(dx * dx + dy * dy);
    float fall = 1.0f / (1.0f + d / light->radius);
    return light->intensity * fall;
}

/* The original single-center-sample path, kept verbatim so sampling off
   reproduces the previous image bit for bit (same float evaluation order). */
static void render_center(ShadowMap *m, const ShLight *light, const ShSeg *segs, int nseg) {
    for (int y = 0; y < m->h; ++y) {
        for (int x = 0; x < m->w; ++x) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            int shadowed = 0;
            for (int i = 0; i < nseg; ++i)
                if (blocked(&segs[i], light->x, light->y, px, py)) { shadowed = 1; break; }
            float v = 0.0f;
            if (!shadowed) v = shade(light, px, py);
            m->buf[(size_t)y * m->w + x] = v;
        }
    }
}

/* Grid shape for a sample count: the divisor pair of samples whose sides are
   closest together.  Four samples therefore map exactly to the 2x2 quadrant
   centers, sixteen to a 4x4 grid, sixty-four to 8x8, and so on.  Counts that
   are multiples of four but not perfect squares, such as 12, round to the
   nearest near-square grid (3x4). */
static void grid_dims(int samples, int *cols, int *rows) {
    int best = 1, bestgap = samples - 1;
    for (int c = 1; c <= samples; ++c) {
        if (samples % c != 0) continue;
        int r = samples / c;
        if (r < c) break;
        if (r - c < bestgap) { bestgap = r - c; best = c; }
    }
    *cols = best;
    *rows = samples / best;
}

/* Sampling layout: a cols x rows regular grid of subsample centers, with
   cols * rows == samples.  Four samples are the four quadrant centers, so
   each subsample owns a quarter of the pixel and coverage is the exact area
   fraction under the midpoint rule.  Coverage is the fraction of subsamples
   not blocked; the pixel color is the mean of the subsample colors (shadowed
   subsamples contribute 0). */
static void render_subsampled(ShadowMap *m, const ShLight *light, const ShSeg *segs, int nseg,
                              int samples, ShStats *stats, float *coverage) {
    int cols, rows;
    grid_dims(samples, &cols, &rows);
    const size_t n = (size_t)m->w * (size_t)m->h;
    unsigned char *cls = (unsigned char*)malloc(n);
    long long lit_total = 0, edge = 0;

    for (int y = 0; y < m->h; ++y) {
        for (int x = 0; x < m->w; ++x) {
            float sum = 0.0f;
            int lit = 0;
            for (int j = 0; j < rows; ++j) {
                float oy = ((float)j + 0.5f) / (float)rows;
                for (int c = 0; c < cols; ++c) {
                    float ox = ((float)c + 0.5f) / (float)cols;
                    float px = (float)x + ox, py = (float)y + oy;
                    int shadowed = 0;
                    for (int i = 0; i < nseg; ++i)
                        if (blocked(&segs[i], light->x, light->y, px, py)) { shadowed = 1; break; }
                    if (!shadowed) { sum += shade(light, px, py); ++lit; }
                }
            }
            size_t i = (size_t)y * (size_t)m->w + (size_t)x;
            m->buf[i] = sum / (float)samples;
            lit_total += lit;
            if (lit > 0 && lit < samples) ++edge;
            if (cls) cls[i] = (unsigned char)(2 * lit >= samples);
            if (coverage) coverage[i] = (float)lit / (float)samples;
        }
    }

    long long boundary = 0;
    if (cls) {
        for (int y = 0; y < m->h; ++y) {
            for (int x = 0; x < m->w; ++x) {
                size_t i = (size_t)y * (size_t)m->w + (size_t)x;
                int e = 0;
                if (x > 0 && cls[i - 1] != cls[i]) e = 1;
                if (!e && x + 1 < m->w && cls[i + 1] != cls[i]) e = 1;
                if (!e && y > 0 && cls[((size_t)y - 1) * m->w + x] != cls[i]) e = 1;
                if (!e && y + 1 < m->h && cls[((size_t)y + 1) * m->w + x] != cls[i]) e = 1;
                if (e) ++boundary;
            }
        }
        free(cls);
    }

    if (stats) {
        stats->samples = samples;
        stats->total = (long long)n * samples;
        stats->lit = lit_total;
        stats->coverage = (double)lit_total / (double)stats->total;
        stats->edge_pixels = edge;
        stats->boundary_pixels = boundary;
    }
}

int sh_render(ShadowMap *m, const ShLight *light, const ShSeg *segs, int nseg) {
    return sh_render_ex(m, light, segs, nseg, SH_SAMPLES_OFF, NULL, NULL, NULL);
}

int sh_render_ex(ShadowMap *m, const ShLight *light, const ShSeg *segs, int nseg,
                 int samples, ShStats *stats, float *coverage,
                 const char **err_field) {
    if (err_field) *err_field = NULL;
    if (!m) { if (err_field) *err_field = "m"; return SH_ERR_ARG; }
    if (m->w <= 0) { if (err_field) *err_field = "w"; return SH_ERR_SIZE; }
    if (m->h <= 0) { if (err_field) *err_field = "h"; return SH_ERR_SIZE; }
    if (!m->buf) { if (err_field) *err_field = "buf"; return SH_ERR_ARG; }
    if (!light) { if (err_field) *err_field = "light"; return SH_ERR_ARG; }
    if (nseg < 0) { if (err_field) *err_field = "nseg"; return SH_ERR_ARG; }
    if (nseg > 0 && !segs) { if (err_field) *err_field = "segs"; return SH_ERR_ARG; }
    if (light->radius <= 0.0f || light->intensity < 0.0f) {
        if (err_field) *err_field = "light";
        return SH_ERR_LIGHT;
    }
    if (samples < 0 || (samples != 0 && samples % 4 != 0)) {
        if (err_field) *err_field = "samples";
        return SH_ERR_SAMPLES;
    }

    if (stats) { stats->samples = 0; stats->total = 0; stats->lit = 0;
                 stats->coverage = 0.0; stats->edge_pixels = 0;
                 stats->boundary_pixels = 0; }
    if (coverage)
        for (int i = 0; i < m->w * m->h; ++i) coverage[i] = 0.0f;

    if (samples == SH_SAMPLES_OFF) {
        render_center(m, light, segs, nseg);
        if (stats) {
            long long lit = 0;
            for (int i = 0; i < m->w * m->h; ++i) if (m->buf[i] > 0.0f) ++lit;
            stats->samples = 1;
            stats->total = (long long)m->w * m->h;
            stats->lit = lit;
            stats->coverage = (double)lit / (double)stats->total;
            stats->edge_pixels = 0;
            stats->boundary_pixels = 0;
        }
        if (coverage)
            for (int i = 0; i < m->w * m->h; ++i) coverage[i] = m->buf[i] > 0.0f ? 1.0f : 0.0f;
        if (stats) {
            unsigned char *c = (unsigned char*)malloc((size_t)m->w * m->h);
            if (c) {
                for (int i = 0; i < m->w * m->h; ++i) c[i] = m->buf[i] > 0.0f;
                for (int y = 0; y < m->h; ++y)
                    for (int x = 0; x < m->w; ++x) {
                        size_t i = (size_t)y * m->w + x;
                        int e = 0;
                        if (x > 0 && c[i - 1] != c[i]) e = 1;
                        if (!e && x + 1 < m->w && c[i + 1] != c[i]) e = 1;
                        if (!e && y > 0 && c[((size_t)y - 1) * m->w + x] != c[i]) e = 1;
                        if (!e && y + 1 < m->h && c[((size_t)y + 1) * m->w + x] != c[i]) e = 1;
                        stats->boundary_pixels += e;
                    }
                free(c);
            }
        }
        return SH_OK;
    }

    render_subsampled(m, light, segs, nseg, samples, stats, coverage);
    return SH_OK;
}

int sh_write_pgm(const ShadowMap *m, const char *path) {
    if (!m || !path) return -1;
    FILE *f = fopen(path, "wb");
    if (!f) return -2;
    fprintf(f, "P5\n%d %d\n255\n", m->w, m->h);
    for (int i = 0; i < m->w * m->h; ++i) {
        float v = m->buf[i];
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        unsigned char b = (unsigned char)(v * 255.0f + 0.5f);
        fwrite(&b, 1, 1, f);
    }
    fclose(f);
    return 0;
}
