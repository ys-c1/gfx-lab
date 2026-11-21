#include "shadow.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

int sh_init(ShadowMap *m, int w, int h) {
    if (!m || w <= 0 || h <= 0) return -1;
    m->buf = (float*)calloc((size_t)w * (size_t)h, sizeof(float));
    if (!m->buf) return -1;
    m->w = w; m->h = h;
    return 0;
}

void sh_free(ShadowMap *m) { if (m) { free(m->buf); m->buf = NULL; m->w = 0; m->h = 0; } }

int sh_clear(ShadowMap *m) {
    if (!m) return -1;
    for (int i = 0; i < m->w * m->h; ++i) m->buf[i] = 0.0f;
    return 0;
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

int sh_render(ShadowMap *m, const ShLight *light, const ShSeg *segs, int nseg) {
    if (!m || !light || (nseg > 0 && !segs)) return -1;
    if (light->radius <= 0.0f || light->intensity < 0.0f) return -2;
    for (int y = 0; y < m->h; ++y) {
        for (int x = 0; x < m->w; ++x) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            int shadowed = 0;
            for (int i = 0; i < nseg; ++i)
                if (blocked(&segs[i], light->x, light->y, px, py)) { shadowed = 1; break; }
            float v = 0.0f;
            if (!shadowed) {
                float dx = px - light->x, dy = py - light->y;
                float d = sqrtf(dx * dx + dy * dy);
                float fall = 1.0f / (1.0f + d / light->radius);
                v = light->intensity * fall;
            }
            m->buf[(size_t)y * m->w + x] = v;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Multi-light path                                                          */
/* ------------------------------------------------------------------------ */

#include <string.h>

static void err_reset(ShError *e) {
    if (!e) return;
    e->code = SH_OK; e->light = -1; e->field[0] = '\0'; e->msg[0] = '\0';
}

static int err_set(ShError *e, int code, int light, const char *field,
                   const char *what, double value) {
    if (e) {
        e->code = code;
        e->light = light;
        snprintf(e->field, sizeof e->field, "%s", field ? field : "");
        if (what) snprintf(e->msg, sizeof e->msg, "%s (%.9g)", what, value);
        else snprintf(e->msg, sizeof e->msg, "%s", field ? field : "");
    }
    return code;
}

static int bad_float(float v) { return !isfinite(v) || isnan(v); }

/* Validate one light.  Returns SH_OK or SH_ERR_LIGHT with the field named. */
static int check_light(const ShLight *l, int i, ShError *err) {
    char f[40];
    if (bad_float(l->x)) { snprintf(f, sizeof f, "lights[%d].x", i); return err_set(err, SH_ERR_LIGHT, i, f, "not a finite float", l->x); }
    if (bad_float(l->y)) { snprintf(f, sizeof f, "lights[%d].y", i); return err_set(err, SH_ERR_LIGHT, i, f, "not a finite float", l->y); }
    if (bad_float(l->radius) || l->radius <= 0.0f) {
        snprintf(f, sizeof f, "lights[%d].radius", i);
        return err_set(err, SH_ERR_LIGHT, i, f, l->radius <= 0.0f ? "must be > 0" : "not a finite float", l->radius);
    }
    if (bad_float(l->intensity) || l->intensity < 0.0f) {
        snprintf(f, sizeof f, "lights[%d].intensity", i);
        return err_set(err, SH_ERR_LIGHT, i, f, l->intensity < 0.0f ? "must be >= 0" : "not a finite float", l->intensity);
    }
    return SH_OK;
}

static int check_seg(const ShSeg *s, int i, ShError *err) {
    char f[40];
    if (bad_float(s->x0)) { snprintf(f, sizeof f, "segs[%d].x0", i); return err_set(err, SH_ERR_SEG, -1, f, "not a finite float", s->x0); }
    if (bad_float(s->y0)) { snprintf(f, sizeof f, "segs[%d].y0", i); return err_set(err, SH_ERR_SEG, -1, f, "not a finite float", s->y0); }
    if (bad_float(s->x1)) { snprintf(f, sizeof f, "segs[%d].x1", i); return err_set(err, SH_ERR_SEG, -1, f, "not a finite float", s->x1); }
    if (bad_float(s->y1)) { snprintf(f, sizeof f, "segs[%d].y1", i); return err_set(err, SH_ERR_SEG, -1, f, "not a finite float", s->y1); }
    return SH_OK;
}

/* Descending intensity, then x, then y, then radius (all ascending), then the
 * input index.  A strict weak order on the row content. */
static int order_less(const ShLightOrder *a, const ShLightOrder *b) {
    if (a->intensity != b->intensity) return a->intensity > b->intensity;
    if (a->x != b->x) return a->x < b->x;
    if (a->y != b->y) return a->y < b->y;
    if (a->radius != b->radius) return a->radius < b->radius;
    return a->index < b->index;
}

static int order_permutation(const ShLightOrder *o, int n) {
    for (int i = 1; i < n; ++i) if (o[i].index < o[i-1].index) return 1;
    return 0;
}

int sh_lights_order(const ShLight *lights, int nlight,
                    ShLightOrder *order, ShError *err) {
    err_reset(err);
    if (!lights || !order) return err_set(err, SH_ERR_NULL, -1, !lights ? "lights" : "order", "must not be NULL", 0);
    if (nlight <= 0) return err_set(err, SH_ERR_NOLIGHT, -1, "nlight", "must be > 0", (double)nlight);
    if (nlight > SH_MAX_LIGHTS) return err_set(err, SH_ERR_ARG, -1, "nlight", "exceeds SH_MAX_LIGHTS", (double)nlight);
    for (int i = 0; i < nlight; ++i) {
        int rc = check_light(&lights[i], i, err);
        if (rc != SH_OK) return rc;
        order[i].index = i;
        order[i].intensity = lights[i].intensity;
        order[i].x = lights[i].x;
        order[i].y = lights[i].y;
        order[i].radius = lights[i].radius;
    }
    /* Insertion sort: nlight <= SH_MAX_LIGHTS, so this is cheap and, unlike
     * qsort, cannot reorder equal rows (a stable order keeps the export and
     * the accumulation deterministic). */
    for (int i = 1; i < nlight; ++i) {
        ShLightOrder key = order[i];
        int j = i - 1;
        while (j >= 0 && order_less(&key, &order[j])) { order[j+1] = order[j]; --j; }
        order[j+1] = key;
    }
    return SH_OK;
}

/* Stage 2 at pixel (x, y): sum this pixel's contribution over `order`, in
 * order, and count the additions in *ops. */
static float shade_pixel(int x, int y, const ShLight *lights,
                         const ShLightOrder *order, int n,
                         const ShSeg *segs, int nseg, unsigned long long *ops) {
    float px = (float)x + 0.5f, py = (float)y + 0.5f;
    float v = 0.0f;
    for (int k = 0; k < n; ++k) {
        const ShLight *L = &lights[order[k].index];
        int shadowed = 0;
        for (int i = 0; i < nseg; ++i)
            if (blocked(&segs[i], L->x, L->y, px, py)) { shadowed = 1; break; }
        if (!shadowed) {
            float dx = px - L->x, dy = py - L->y;
            float d = sqrtf(dx * dx + dy * dy);
            v += L->intensity / (1.0f + d / L->radius);
        }
        ++*ops;
    }
    return v;
}

int sh_render_lights(ShadowMap *m, const ShLight *lights, int nlight,
                     const ShSeg *segs, int nseg, int enable_multi,
                     ShMultiStats *stats, ShError *err) {
    ShMultiStats local;
    if (!stats) stats = &local;
    memset(stats, 0, sizeof *stats);
    err_reset(err);
    if (!m) return err_set(err, SH_ERR_NULL, -1, "m", "must not be NULL", 0);
    if (!lights) return err_set(err, SH_ERR_NULL, -1, "lights", "must not be NULL", 0);
    if (nlight <= 0) return err_set(err, SH_ERR_NOLIGHT, -1, "nlight", "must be > 0", (double)nlight);
    if (nlight > SH_MAX_LIGHTS) return err_set(err, SH_ERR_ARG, -1, "nlight", "exceeds SH_MAX_LIGHTS", (double)nlight);
    if (nseg < 0) return err_set(err, SH_ERR_ARG, -1, "nseg", "must be >= 0", (double)nseg);
    if (nseg > 0 && !segs) return err_set(err, SH_ERR_NULL, -1, "segs", "must not be NULL when nseg > 0", 0);
    for (int i = 0; i < nlight; ++i) {
        int rc = check_light(&lights[i], i, err);
        if (rc != SH_OK) return rc;
    }
    for (int i = 0; i < nseg; ++i) {
        int rc = check_seg(&segs[i], i, err);
        if (rc != SH_OK) return rc;
    }
    if (!m->buf || m->w <= 0 || m->h <= 0)
        return err_set(err, SH_ERR_ARG, -1, "m", "map is not initialised", 0);

    if (!enable_multi) {
        /* Legacy compatibility path: byte-identical to sh_render(lights[0]). */
        int rc = sh_render(m, &lights[0], segs, nseg);
        if (rc != SH_OK) return err_set(err, SH_ERR_LIGHT, 0, "lights[0]", "rejected by sh_render", rc);
        stats->lights = 1;
        stats->passes = 1;
        stats->pixels = (unsigned long long)m->w * (unsigned long long)m->h;
        stats->accumulate_ops = stats->pixels;
        stats->map_bytes = (size_t)m->w * (size_t)m->h * sizeof(float);
        stats->order[0].index = 0;
        stats->order[0].intensity = lights[0].intensity;
        stats->order[0].x = lights[0].x;
        stats->order[0].y = lights[0].y;
        stats->order[0].radius = lights[0].radius;
        stats->order_bytes = sizeof(ShLightOrder);
        stats->total_bytes = stats->map_bytes + stats->order_bytes;
        stats->peak_extra_bytes = stats->order_bytes;
        return SH_OK;
    }

    ShLightOrder order[SH_MAX_LIGHTS];
    int rc = sh_lights_order(lights, nlight, order, err);
    if (rc != SH_OK) return rc;

    /* Stage 2: stream the accumulation into the map.  No per-light temporary
     * maps are allocated. */
    for (int i = 0; i < m->w * m->h; ++i) m->buf[i] = 0.0f;
    unsigned long long ops = 0;
    for (int y = 0; y < m->h; ++y)
        for (int x = 0; x < m->w; ++x) {
            size_t i = (size_t)y * (size_t)m->w + (size_t)x;
            m->buf[i] = shade_pixel(x, y, lights, order, nlight, segs, nseg, &ops);
        }

    memcpy(stats->order, order, sizeof(ShLightOrder) * (size_t)nlight);
    stats->lights = nlight;
    stats->passes = nlight;
    stats->sorted = order_permutation(order, nlight);
    stats->pixels = (unsigned long long)m->w * (unsigned long long)m->h;
    stats->accumulate_ops = ops;
    stats->map_bytes = (size_t)m->w * (size_t)m->h * sizeof(float);
    stats->order_bytes = sizeof(ShLightOrder) * (size_t)nlight;
    stats->total_bytes = stats->map_bytes + stats->order_bytes;
    stats->peak_extra_bytes = stats->order_bytes;
    stats->avoided_temp_bytes = stats->map_bytes * (size_t)nlight;
    stats->temp_maps = 0;
    return SH_OK;
}

int sh_write_order_table(const char *path, const ShLightOrder *order, int n,
                         const char *scene, int w, int h) {
    ShError err;
    err_reset(&err);
    if (!path || !order) return SH_ERR_NULL;
    if (n < 0) return SH_ERR_ARG;
    FILE *f = fopen(path, "wb");
    if (!f) return SH_ERR_IO;
    fprintf(f, "# scene=%s w=%d h=%d passes=%d\n", scene ? scene : "-", w, h, n);
    fprintf(f, "rank,index,intensity,x,y,radius\n");
    for (int i = 0; i < n; ++i)
        fprintf(f, "%d,%d,%.9g,%.9g,%.9g,%.9g\n", i, order[i].index,
                order[i].intensity, order[i].x, order[i].y, order[i].radius);
    fclose(f);
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
