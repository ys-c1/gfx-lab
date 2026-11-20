#include "shadow.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SH_MAX_ADAPTIVE_SAMPLES 64
#define SH_GOLDEN_ANGLE 2.399963229728653f

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

static void seterr(ShError *err, int code, const char *field, const char *msg) {
    if (!err) return;
    err->code = code;
    snprintf(err->field, sizeof(err->field), "%s", field);
    snprintf(err->message, sizeof(err->message), "%s", msg);
}

static int valid_basic(ShadowMap *m, const ShLight *light, const ShSeg *segs,
                       int nseg, ShError *err) {
    if (!m || !light || (nseg > 0 && !segs)) {
        seterr(err, SH_ERR_ARG, "m/light/segs", "null map, light or segs buffer");
        return SH_ERR_ARG;
    }
    if (nseg < 0) {
        seterr(err, SH_ERR_ARG, "nseg", "nseg must be >= 0");
        return SH_ERR_ARG;
    }
    if (light->radius <= 0.0f) {
        seterr(err, SH_ERR_LIGHT, "light.radius", "light.radius must be > 0");
        return SH_ERR_LIGHT;
    }
    if (light->intensity < 0.0f) {
        seterr(err, SH_ERR_LIGHT, "light.intensity", "light.intensity must be >= 0");
        return SH_ERR_LIGHT;
    }
    for (int i = 0; i < nseg; ++i) {
        const ShSeg *s = &segs[i];
        if (s->x0 == s->x1 && s->y0 == s->y1) {
            char field[40];
            snprintf(field, sizeof(field), "segs[%d]", i);
            seterr(err, SH_ERR_SEGMENT, field,
                   "degenerate occluder segment: x0==x1 and y0==y1");
            return SH_ERR_SEGMENT;
        }
    }
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

/* One hard shadow map for a single point light at (lx, ly). */
static void hard_map(const ShadowMap *m, const ShLight *light, const ShSeg *segs,
                     int nseg, float lx, float ly, float *out) {
    for (int y = 0; y < m->h; ++y) {
        for (int x = 0; x < m->w; ++x) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            int shadowed = 0;
            for (int i = 0; i < nseg; ++i)
                if (blocked(&segs[i], lx, ly, px, py)) { shadowed = 1; break; }
            float v = 0.0f;
            if (!shadowed) {
                float dx = px - lx, dy = py - ly;
                float d = sqrtf(dx * dx + dy * dy);
                float fall = 1.0f / (1.0f + d / light->radius);
                v = light->intensity * fall;
            }
            out[(size_t)y * m->w + x] = v;
        }
    }
}

int sh_render(ShadowMap *m, const ShLight *light, const ShSeg *segs, int nseg) {
    ShError err;
    int rc = valid_basic(m, light, segs, nseg, &err);
    if (rc != SH_OK) return rc;
    hard_map(m, light, segs, nseg, light->x, light->y, m->buf);
    return SH_OK;
}

/* Deterministic stratified point on a disk: golden-angle sunflower sampling. */
static void disk_point(float cx, float cy, float r, int i, int n, float *ox, float *oy) {
    float rr = r * sqrtf(((float)i + 0.5f) / (float)n);
    float a = (float)i * SH_GOLDEN_ANGLE;
    *ox = cx + rr * cosf(a);
    *oy = cy + rr * sinf(a);
}

float sh_penumbra_width(const ShLight *light, const ShSeg *seg, float px, float py) {
    if (!light || !seg) return 0.0f;
    if (light->src_radius <= 0.0f) return 0.0f;
    float rx = px - light->x, ry = py - light->y;
    float sx = seg->x1 - seg->x0, sy = seg->y1 - seg->y0;
    float den = rx * sy - ry * sx;
    if (fabsf(den) < 1e-12f) return 0.0f;
    float u = ((seg->x0 - light->x) * ry - (seg->y0 - light->y) * rx) / den;
    float t = ((seg->x0 - light->x) * sy - (seg->y0 - light->y) * sx) / den;
    /* ray must hit the segment interior before reaching the receiver */
    if (u < 0.0f || u > 1.0f || t <= 0.0f || t >= 1.0f) return 0.0f;
    /* W = source diameter * (receiver distance - blocker distance) / blocker distance */
    return 2.0f * light->src_radius * (1.0f - t) / t;
}

/* Stage 1: largest analytic penumbra bandwidth across all receiver pixels. */
static float max_penumbra_width(const ShadowMap *m, const ShLight *light,
                                const ShSeg *segs, int nseg) {
    float wmax = 0.0f;
    for (int y = 0; y < m->h; ++y)
        for (int x = 0; x < m->w; ++x)
            for (int i = 0; i < nseg; ++i) {
                float w = sh_penumbra_width(light, &segs[i],
                                            (float)x + 0.5f, (float)y + 0.5f);
                if (w > wmax) wmax = w;
            }
    return wmax;
}

static int adaptive_samples(float wmax) {
    int n = (int)ceilf(wmax) + 1;
    if (n < 2) n = 2;
    if (n > SH_MAX_ADAPTIVE_SAMPLES) n = SH_MAX_ADAPTIVE_SAMPLES;
    return n;
}

int sh_adaptive_sample_count(const ShadowMap *m, const ShLight *light,
                             const ShSeg *segs, int nseg) {
    if (!m || !light || nseg < 0 || (nseg > 0 && !segs)) return SH_ERR_ARG;
    return adaptive_samples(max_penumbra_width(m, light, segs, nseg));
}

int sh_render_ex(ShadowMap *m, const ShLight *light, const ShSeg *segs, int nseg,
                 const ShPenumbra *pen, ShError *err) {
    if (err) { err->code = 0; err->field[0] = 0; err->message[0] = 0; }
    int rc = valid_basic(m, light, segs, nseg, err);
    if (rc != SH_OK) return rc;

    int soft = pen && pen->enable;
    if (!soft) {
        hard_map(m, light, segs, nseg, light->x, light->y, m->buf);
        return SH_OK;
    }

    if (light->src_radius <= 0.0f) {
        seterr(err, SH_ERR_LIGHT, "light.src_radius",
               "light.src_radius must be > 0 when penumbra is enabled");
        return SH_ERR_LIGHT;
    }
    if (pen->samples == 0) {
        seterr(err, SH_ERR_SAMPLES, "pen.samples",
               "pen.samples must be != 0 (or SH_SAMPLES_ADAPTIVE)");
        return SH_ERR_SAMPLES;
    }
    if (pen->samples > 0 && pen->samples < 2) {
        seterr(err, SH_ERR_SAMPLES, "pen.samples",
               "pen.samples must be >= 2 when positive");
        return SH_ERR_SAMPLES;
    }

    int nsamp = pen->samples;
    if (nsamp == SH_SAMPLES_ADAPTIVE)
        nsamp = adaptive_samples(max_penumbra_width(m, light, segs, nseg));

    size_t npix = (size_t)m->w * (size_t)m->h;
    float *one = (float*)malloc(npix * sizeof(float));
    if (!one) {
        seterr(err, SH_ERR_MEMORY, "m.buf", "out of memory for umbra scratch map");
        return SH_ERR_MEMORY;
    }
    memset(m->buf, 0, npix * sizeof(float));

    /* Stage 2: N light points along the occluder edge -> N hard umbra maps.
       Stage 3: composite the maps into one soft shadow map. */
    for (int k = 0; k < nsamp; ++k) {
        float lx, ly;
        disk_point(light->x, light->y, light->src_radius, k, nsamp, &lx, &ly);
        hard_map(m, light, segs, nseg, lx, ly, one);
        for (size_t i = 0; i < npix; ++i) m->buf[i] += one[i];
    }
    float inv = 1.0f / (float)nsamp;
    for (size_t i = 0; i < npix; ++i) m->buf[i] *= inv;

    free(one);
    return SH_OK;
}

int sh_render_ref(ShadowMap *m, const ShLight *light, const ShSeg *segs, int nseg,
                  int samples, ShError *err) {
    if (err) { err->code = 0; err->field[0] = 0; err->message[0] = 0; }
    int rc = valid_basic(m, light, segs, nseg, err);
    if (rc != SH_OK) return rc;
    if (light->src_radius <= 0.0f) {
        seterr(err, SH_ERR_LIGHT, "light.src_radius",
               "light.src_radius must be > 0 for reference sampling");
        return SH_ERR_LIGHT;
    }
    if (samples <= 0) {
        seterr(err, SH_ERR_SAMPLES, "samples",
               "per-pixel reference samples must be > 0");
        return SH_ERR_SAMPLES;
    }

    size_t npix = (size_t)m->w * (size_t)m->h;
    float *acc = (float*)calloc(npix, sizeof(float));
    if (!acc) {
        seterr(err, SH_ERR_MEMORY, "m.buf", "out of memory in reference renderer");
        return SH_ERR_MEMORY;
    }
    for (int k = 0; k < samples; ++k) {
        float lx, ly;
        disk_point(light->x, light->y, light->src_radius, k, samples, &lx, &ly);
        for (int y = 0; y < m->h; ++y) {
            for (int x = 0; x < m->w; ++x) {
                float px = (float)x + 0.5f, py = (float)y + 0.5f;
                int shadowed = 0;
                for (int i = 0; i < nseg; ++i)
                    if (blocked(&segs[i], lx, ly, px, py)) { shadowed = 1; break; }
                if (!shadowed) {
                    float dx = px - lx, dy = py - ly;
                    float d = sqrtf(dx * dx + dy * dy);
                    acc[(size_t)y * m->w + x] +=
                        light->intensity / (1.0f + d / light->radius);
                }
            }
        }
    }
    float inv = 1.0f / (float)samples;
    for (size_t i = 0; i < npix; ++i) m->buf[i] = acc[i] * inv;
    free(acc);
    return SH_OK;
}

const char *sh_strerror(const ShError *err) {
    if (!err) return "no error";
    return err->message[0] ? err->message : "ok";
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
