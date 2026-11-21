#include "shadow.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* ---- configurable quantization path ------------------------------------ */

static int quant_validate(const ShQuantCfg *cfg) {
    if (cfg->bits < 0)  return SH_E_BITS_NEG;
    if (cfg->enabled < 0) return SH_E_FLAG_NEG;
    if (cfg->enabled && (cfg->bits < 1 || cfg->bits > 16))
        return SH_E_BITS_RANGE;
    return 0;
}

int sh_quantize(const ShadowMap *m, const ShQuantCfg *cfg,
                unsigned char **out, long *nbytes, ShQuantStats *st) {
    if (!m || !cfg || !out || !nbytes) return -1;
    int rc = quant_validate(cfg);
    if (rc) return rc;

    const long n = (long)m->w * (long)m->h;
    const int bits = cfg->enabled ? cfg->bits : 8;
    const int bytes_per_px = (bits > 8) ? 2 : 1;
    const long need = n * bytes_per_px;

    unsigned char *buf = (unsigned char*)malloc((size_t)need);
    if (!buf) return SH_E_NOMEM;

    /* Stage 1: recompute quantization step from the TARGET bit depth.
       PGM maxval = 2^bits - 1; step is in normalized [0,1] units. */
    const double levels = (double)((1L << bits) - 1L);
    const double step = 1.0 / levels;

    /* Stage 2: per-sample quantization (clamp -> round to nearest level ->
       pack big-endian, matching PGM network byte order). */
    double sum_sq = 0.0;
    for (long i = 0; i < n; ++i) {
        float v = m->buf[i];
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;

        unsigned long code;
        if (cfg->enabled) {
            code = (unsigned long)lround((double)v * levels);
            if ((double)code > levels) code = (unsigned long)levels;
        } else {
            /* disabled: reproduce the legacy 8-bit path expression exactly */
            code = (unsigned long)((double)v * 255.0 + 0.5);
        }

        if (bytes_per_px == 1) {
            buf[i] = (unsigned char)code;
        } else {
            buf[i * 2]     = (unsigned char)((code >> 8) & 0xFFu);
            buf[i * 2 + 1] = (unsigned char)(code & 0xFFu);
        }

        /* Stage 3: accumulate reconstruction noise against the float input.
           Reconstruction maps the integer code back to normalized units. */
        double recon = (double)code / levels;
        double e = recon - (double)v;
        sum_sq += e * e;
    }

    *out = buf;
    *nbytes = need;
    if (st) {
        st->bits = bits;
        st->samples = n;
        st->step = step;
        st->noise_rms = sqrt(sum_sq / (double)n);
    }
    return 0;
}

int sh_write_pgm_q(const ShadowMap *m, const char *path,
                   const ShQuantCfg *cfg, ShQuantStats *st) {
    if (!m || !path || !cfg) return -1;

    /* switch off -> return to the current output, verbatim */
    if (!cfg->enabled)
        return sh_write_pgm(m, path);

    unsigned char *buf = NULL;
    long nbytes = 0;
    int rc = sh_quantize(m, cfg, &buf, &nbytes, st);
    if (rc) return rc;

    FILE *f = fopen(path, "wb");
    if (!f) { free(buf); return -2; }

    const int bits = cfg->bits;
    const unsigned long maxval = (1UL << bits) - 1UL;
    fprintf(f, "P5\n%d %d\n%lu\n", m->w, m->h, maxval);
    if (nbytes > 0) fwrite(buf, 1, (size_t)nbytes, f);
    int failed = ferror(f);
    fclose(f);
    free(buf);
    return failed ? -2 : 0;
}
