#include "shadow.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

int sh_init(ShadowMap *m, int w, int h) {
    if (!m || w <= 0 || h <= 0) return SHADOW_EINVAL;
    m->buf = (float*)calloc((size_t)w * (size_t)h, sizeof(float));
    if (!m->buf) return SHADOW_EINVAL;
    m->w = w; m->h = h;
    return SHADOW_OK;
}

void sh_free(ShadowMap *m) { if (m) { free(m->buf); m->buf = NULL; m->w = 0; m->h = 0; } }

int sh_clear(ShadowMap *m) {
    if (!m) return SHADOW_EINVAL;
    for (int i = 0; i < m->w * m->h; ++i) m->buf[i] = 0.0f;
    return SHADOW_OK;
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

static int shadowed_any(const ShSeg *segs, int nseg,
                        float lx, float ly, float px, float py) {
    for (int i = 0; i < nseg; ++i)
        if (blocked(&segs[i], lx, ly, px, py)) return 1;
    return 0;
}

static float light_value(const ShLight *light, float px, float py) {
    float dx = px - light->x, dy = py - light->y;
    float d = sqrtf(dx * dx + dy * dy);
    float fall = 1.0f / (1.0f + d / light->radius);
    return light->intensity * fall;
}

/* stratified sample offset inside a kernel x kernel pixel window:
   ox = (i - (S-1)/2) * K / S, i in [0,S); symmetric, exact centre at i=(S-1)/2 */
static float sample_offset(int i, int samples, int kernel) {
    return ((float)i - (float)(samples - 1) * 0.5f)
           * (float)kernel / (float)samples;
}

static int check_pcf(const ShPCF *pcf) {
    if (pcf->samples <= 0) return SHADOW_ESAMPLES;
    if (pcf->kernel <= 0 || (pcf->kernel & 1) == 0) return SHADOW_EKERNEL;
    return SHADOW_OK;
}

int sh_render(ShadowMap *m, const ShLight *light, const ShSeg *segs, int nseg) {
    if (!m || !light || (nseg > 0 && !segs)) return SHADOW_EINVAL;
    if (light->radius <= 0.0f || light->intensity < 0.0f) return SHADOW_ELIGHT;
    for (int y = 0; y < m->h; ++y) {
        for (int x = 0; x < m->w; ++x) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            float v = 0.0f;
            if (!shadowed_any(segs, nseg, light->x, light->y, px, py))
                v = light_value(light, px, py);
            m->buf[(size_t)y * m->w + x] = v;
        }
    }
    return SHADOW_OK;
}

int sh_render_pcf(ShadowMap *m, const ShLight *light, const ShSeg *segs,
                  int nseg, const ShPCF *pcf) {
    if (!m || !light || (nseg > 0 && !segs)) return SHADOW_EINVAL;
    if (light->radius <= 0.0f || light->intensity < 0.0f) return SHADOW_ELIGHT;
    if (!pcf)
        return sh_render(m, light, segs, nseg); /* no filter: identical image */

    int rc = check_pcf(pcf);
    if (rc != SHADOW_OK) return rc;

    int S = pcf->samples, K = pcf->kernel;
    float total = (float)(S * S);

    for (int y = 0; y < m->h; ++y) {
        for (int x = 0; x < m->w; ++x) {
            float cx = (float)x + 0.5f, cy = (float)y + 0.5f;
            int lit = 0;
            for (int iy = 0; iy < S; ++iy) {
                float py = cy + sample_offset(iy, S, K);
                for (int ix = 0; ix < S; ++ix) {
                    float px = cx + sample_offset(ix, S, K);
                    lit += !shadowed_any(segs, nseg, light->x, light->y, px, py);
                }
            }
            /* average by coverage ratio, then composite with the centre value */
            float coverage = (float)lit / total;
            m->buf[(size_t)y * m->w + x] = coverage * light_value(light, cx, cy);
        }
    }
    return SHADOW_OK;
}

int sh_kernel_build(ShKernel *k, int samples, int kernel) {
    if (!k) return SHADOW_EINVAL;
    k->ox = k->oy = NULL;
    ShPCF pcf = {samples, kernel};
    int rc = check_pcf(&pcf);
    if (rc != SHADOW_OK) return rc;

    size_t n = (size_t)samples * (size_t)samples;
    k->ox = (float*)malloc(n * sizeof(float));
    k->oy = (float*)malloc(n * sizeof(float));
    if (!k->ox || !k->oy) { free(k->ox); free(k->oy); k->ox = k->oy = NULL; return SHADOW_EINVAL; }

    size_t idx = 0;
    for (int iy = 0; iy < samples; ++iy)
        for (int ix = 0; ix < samples; ++ix, ++idx) {
            k->ox[idx] = sample_offset(ix, samples, kernel);
            k->oy[idx] = sample_offset(iy, samples, kernel);
        }
    k->samples = samples;
    k->kernel = kernel;
    k->soft_width = (float)(kernel - 1);
    return SHADOW_OK;
}

void sh_kernel_free(ShKernel *k) {
    if (!k) return;
    free(k->ox); free(k->oy);
    k->ox = k->oy = NULL;
    k->samples = k->kernel = 0;
    k->soft_width = 0.0f;
}

int sh_kernel_write(const ShKernel *k, const char *path) {
    if (!k || !k->ox || !k->oy || !path) return SHADOW_EINVAL;
    FILE *f = fopen(path, "w");
    if (!f) return SHADOW_EINVAL;
    int S = k->samples;
    fprintf(f, "# shadow PCF kernel table\n");
    fprintf(f, "samples=%d\n", S);
    fprintf(f, "kernel=%d\n", k->kernel);
    fprintf(f, "probe_count=%d\n", S * S);
    fprintf(f, "soft_width_px=%.1f\n", k->soft_width);
    fprintf(f, "# row-major offsets in pixels relative to the pixel centre\n");
    fprintf(f, "# idx iy ix ox oy\n");
    size_t idx = 0;
    for (int iy = 0; iy < S; ++iy)
        for (int ix = 0; ix < S; ++ix, ++idx)
            fprintf(f, "%zu %d %d %.6f %.6f\n", idx, iy, ix, k->ox[idx], k->oy[idx]);
    fclose(f);
    return SHADOW_OK;
}

const char *sh_errstr(int code) {
    switch (code) {
    case SHADOW_OK:        return "ok";
    case SHADOW_EINVAL:    return "invalid argument (null pointer, bad dimension or segment count)";
    case SHADOW_ELIGHT:    return "invalid light: radius must be > 0 and intensity >= 0";
    case SHADOW_ESAMPLES:  return "invalid field pcf.samples: must be > 0";
    case SHADOW_EKERNEL:   return "invalid field pcf.kernel: must be a positive odd number";
    default:               return "unknown error";
    }
}

int sh_write_pgm(const ShadowMap *m, const char *path) {
    if (!m || !path) return SHADOW_EINVAL;
    FILE *f = fopen(path, "wb");
    if (!f) return SHADOW_EINVAL;
    fprintf(f, "P5\n%d %d\n255\n", m->w, m->h);
    for (int i = 0; i < m->w * m->h; ++i) {
        float v = m->buf[i];
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        unsigned char b = (unsigned char)(v * 255.0f + 0.5f);
        fwrite(&b, 1, 1, f);
    }
    fclose(f);
    return SHADOW_OK;
}
