#define _POSIX_C_SOURCE 199309L  /* clock_gettime */
#include "shadow.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int sh_init(ShadowMap *m, int w, int h) {
    if (!m || w <= 0 || h <= 0) return -1;
    m->buf = (float*)calloc((size_t)w * (size_t)h, sizeof(float));
    if (!m->buf) return -1;
    m->w = w; m->h = h;
    return 0;
}

void sh_free(ShadowMap *m) {
    if (!m) return;
    free(m->buf); m->buf = NULL; m->w = 0; m->h = 0;
}

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

/* ---- shared numeric helpers ---------------------------------------- */

/* The legacy writer's exact arithmetic: clamp, scale, round, truncate. */
static unsigned char legacy_byte(float v) {
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    return (unsigned char)(v * 255.0f + 0.5f);
}

/* Values outside [0,1] saturate, exactly as in the legacy writer. Applying
 * this before the code math keeps the target from running past the code
 * range and manufacturing residual error where the byte path has none. */
static double clamp01(float v) {
    if (v < 0.0f) return 0.0;
    if (v > 1.0f) return 1.0;
    return (double)v;
}

/* Nearest code for v already in [0,1]; clamped into the code range. */
static int quant_code(double v, int levels) {
    double top = (double)(levels - 1);
    double c = floor(v * top + 0.5);
    if (c < 0.0) c = 0.0;
    if (c > top) c = top;
    return (int)c;
}

/* The PGM is 8-bit, so a code at any depth is stored as code*255/top. The
 * map is strictly increasing, hence one-to-one: it changes neither band
 * counts nor error measured in code units. */
static unsigned char display_byte(int code, int levels) {
    double top = (double)(levels - 1);
    double b = floor((double)code * 255.0 / top + 0.5);
    if (b < 0.0) b = 0.0;
    if (b > 255.0) b = 255.0;
    return (unsigned char)b;
}

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
}

static int fail(char *err, size_t errlen, const char *fmt, ...) {
    if (err && errlen) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(err, errlen, fmt, ap);
        va_end(ap);
    }
    return -1;
}

/* Only the fields the active mode reads are checked, so a disabled dither
 * never trips on a stale levels/amount it will not use. */
static int validate_settings(const ShDither *d, char *err, size_t errlen) {
    if (!d) return fail(err, errlen, "ShDither: pointer is NULL");
    if (d->enabled != 0 && d->enabled != 1)
        return fail(err, errlen,
                    "ShDither.enabled: got %d, expected 0 or 1 (0 = legacy, 1 = dither)",
                    d->enabled);
    if (d->enabled == 0) return 0;
    if (d->levels < 2 || d->levels > 256)
        return fail(err, errlen,
                    "ShDither.levels: got %d, expected 2..256 (256 = 8-bit output)",
                    d->levels);
    if (!(d->amount >= 0.0 && d->amount <= 1.0))
        return fail(err, errlen,
                    "ShDither.amount: got %g, expected 0.0..1.0 (NaN is rejected)",
                    d->amount);
    return 0;
}

/* ---- band counting ------------------------------------------------- */

/* RMS of the error field after a separable box blur. Banding is the
 * low-frequency part of quantization error; dithering moves the same error
 * energy to high frequencies, which the blur removes. */
static double lowfreq_rms(const double *e, int w, int h, int r) {
    if (r < 1) r = 1;
    double *tmp = (double*)malloc((size_t)w * (size_t)h * sizeof(double));
    if (!tmp) return -1.0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            double s = 0.0;
            int n = 0;
            for (int dx = -r; dx <= r; ++dx) {
                int xx = x + dx;
                if (xx < 0 || xx >= w) continue;
                s += e[(size_t)y * w + xx];
                ++n;
            }
            tmp[(size_t)y * w + x] = s / (double)n;
        }
    }
    double sq = 0.0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            double s = 0.0;
            int n = 0;
            for (int dy = -r; dy <= r; ++dy) {
                int yy = y + dy;
                if (yy < 0 || yy >= h) continue;
                s += tmp[(size_t)yy * w + x];
                ++n;
            }
            double b = s / (double)n;
            sq += b * b;
        }
    }
    free(tmp);
    return sqrt(sq / ((double)w * (double)h));
}

/* A ring band is a maximal run of >= SH_BAND_RUN pixels holding one
 * code, which is also matched code-for-code by the row directly above or
 * below (for horizontal runs) or the column left or right (for vertical
 * runs). That "two rows thick" test is what separates a band you can see
 * from the single-pixel speckle that error diffusion produces: dithering
 * keeps the same average value but breaks every thick plateau into noise.
 *
 * The run must also cover a real change in the ideal value (target code
 * span >= 0.5), so flat areas such as fully shadowed region interiors — a
 * constant black that no quantizer can band — are not counted. Each row of
 * a wide ring counts once, so the number measures how much of the image is
 * organised into visible bands, not how many annuli exist. */
static long long count_ring_bands(const ShadowMap *m, const unsigned char *px,
                                  int levels) {
    const int w = m->w, h = m->h;
    const double top = (double)(levels - 1);
    long long bands = 0;
    for (int y = 0; y < h; ++y) {
        const unsigned char *row = px + (size_t)y * (size_t)w;
        const unsigned char *up  = y > 0 ? row - w : NULL;
        const unsigned char *dn  = y + 1 < h ? row + w : NULL;
        int x = 0;
        while (x < w) {
            int run = 1;
            while (x + run < w && row[x + run] == row[x]) ++run;
            if (run >= SH_BAND_RUN) {
                double lo = 1e30, hi = -1e30;
                for (int i = 0; i < run; ++i) {
                    double t = clamp01(m->buf[(size_t)y * w + x + i]) * top;
                    if (t < lo) lo = t;
                    if (t > hi) hi = t;
                }
                unsigned char v = row[x];
                int thick = 0;
                if (up) { thick = 1; for (int i = 0; i < run; ++i) if (up[x+i] != v) { thick = 0; break; } }
                if (!thick && dn) { thick = 1; for (int i = 0; i < run; ++i) if (dn[x+i] != v) { thick = 0; break; } }
                if (thick && hi - lo >= SH_BAND_SPAN) ++bands;
            }
            x += run;
        }
    }
    for (int x = 0; x < w; ++x) {
        const unsigned char *col = px + x;
        const unsigned char *lf = x > 0 ? col - 1 : NULL;
        const unsigned char *rt = x + 1 < w ? col + 1 : NULL;
        int y = 0;
        while (y < h) {
            int run = 1;
            while (y + run < h && col[(size_t)(y+run) * w] == col[(size_t)y * w]) ++run;
            if (run >= SH_BAND_RUN) {
                double lo = 1e30, hi = -1e30;
                for (int i = 0; i < run; ++i) {
                    double t = clamp01(m->buf[(size_t)(y+i) * w + x]) * top;
                    if (t < lo) lo = t;
                    if (t > hi) hi = t;
                }
                unsigned char v = col[(size_t)y * w];
                int thick = 0;
                if (lf) { thick = 1; for (int i = 0; i < run; ++i) if (lf[(size_t)(y+i) * w] != v) { thick = 0; break; } }
                if (!thick && rt) { thick = 1; for (int i = 0; i < run; ++i) if (rt[(size_t)(y+i) * w] != v) { thick = 0; break; } }
                if (thick && hi - lo >= SH_BAND_SPAN) ++bands;
            }
            y += run;
        }
    }
    return bands;
}

/* ---- stage 1: error diffusion ------------------------------------- */

int sh_dither_error(const ShadowMap *m, const ShDither *d, double *comp,
                    ShDitherStats *st, char *err, size_t errlen) {
    if (!m || !m->buf || m->w <= 0 || m->h <= 0)
        return fail(err, errlen, "ShadowMap: not initialized (w, h or buf is invalid)");
    if (validate_settings(d, err, errlen) != 0) return -1;

    double t0 = now_ms();
    size_t n = (size_t)m->w * (size_t)m->h;
    double *acc = (double*)calloc(n, sizeof(double));
    if (!acc) return fail(err, errlen, "compensation map: allocation failed");

    double sum = 0.0, absum = 0.0, sq = 0.0;
    int levels = d->enabled ? d->levels : 256;
    if (d->enabled) {
        const int w = m->w, h = m->h;
        const double top = (double)(levels - 1);
        /* Serpentine Floyd-Steinberg, all arithmetic in code units. acc[i]
         * holds the residual diffused into pixel i; that is exactly the
         * compensation stage 2 adds. The outbound residual is scaled by
         * amount, so amount == 0 leaves the map all zero and stage 2
         * degenerates to plain rounding. */
        for (int y = 0; y < h; ++y) {
            int ltr = (y & 1) == 0;
            for (int k = 0; k < w; ++k) {
                int x = ltr ? k : w - 1 - k;
                int dir = ltr ? 1 : -1;
                size_t i = (size_t)y * (size_t)w + (size_t)x;
                double v = clamp01(m->buf[i]);
                double c = acc[i];
                double got = (double)quant_code(v + c / top, levels);
                double r = (v * top + c - got) * d->amount;
                sum += c; absum += fabs(c); sq += c * c;
                if (x + dir >= 0 && x + dir < w)
                    acc[i + dir] += r * (7.0 / 16.0);
                if (y + 1 < h) {
                    if (x - dir >= 0 && x - dir < w)
                        acc[i + w - dir] += r * (3.0 / 16.0);
                    acc[i + w] += r * (5.0 / 16.0);
                    if (x + dir >= 0 && x + dir < w)
                        acc[i + w + dir] += r * (1.0 / 16.0);
                }
            }
        }
    }
    /* Disabled: acc stays zero. */

    if (comp) memcpy(comp, acc, n * sizeof(double));
    free(acc);

    if (st) {
        st->levels = levels;
        st->comp_mean = sum / (double)n;
        st->comp_absmean = absum / (double)n;
        st->comp_rms = sqrt(sq / (double)n);
        st->ring_bands = 0;     /* measured in stage 2, from the pixels written */
        st->err_mean = 0.0;
        st->err_rms = 0.0;
        st->err_code_rms = 0.0;
        st->err_lf_rms = 0.0;
        st->stage2_ms = 0.0;
        st->stage1_ms = now_ms() - t0;
    }
    return 0;
}

/* ---- stage 2: perturbed write ------------------------------------- */

int sh_write_pgm_comp(const ShadowMap *m, const char *path, const ShDither *d,
                      const double *comp, ShDitherStats *st,
                      char *err, size_t errlen) {
    if (!m || !m->buf || m->w <= 0 || m->h <= 0)
        return fail(err, errlen, "ShadowMap: not initialized (w, h or buf is invalid)");
    if (!path) return fail(err, errlen, "path: pointer is NULL");
    if (validate_settings(d, err, errlen) != 0) return -1;
    if (d->enabled && !comp)
        return fail(err, errlen,
                    "comp: required when ShDither.enabled == 1 "
                    "(run sh_dither_error first)");

    double t0 = now_ms();
    size_t n = (size_t)m->w * (size_t)m->h;
    unsigned char *px = (unsigned char*)malloc(n ? n : 1);
    double *errbuf = (double*)malloc(n * sizeof(double));
    if (!px || !errbuf) {
        free(px); free(errbuf);
        return fail(err, errlen, "pixel buffer: allocation failed");
    }

    int levels = d->enabled ? d->levels : 256;
    double top = (double)(levels - 1);
    double csum = 0.0, cabs = 0.0, csq = 0.0;
    double esum = 0.0, esq = 0.0, esq_code = 0.0;

    for (size_t i = 0; i < n; ++i) {
        unsigned char b;
        double written_code, target_code;
        if (d->enabled) {
            double v = clamp01(m->buf[i]);
            double c = comp[i];
            target_code = v * top;
            written_code = (double)quant_code(v + c / top, levels);
            b = display_byte((int)written_code, levels);
            csum += c; cabs += fabs(c); csq += c * c;
        } else {
            /* Legacy arithmetic, verbatim, so the switch-off output is
             * byte-for-byte what sh_write_pgm() produces. */
            b = legacy_byte(m->buf[i]);
            written_code = (double)b;
            target_code = clamp01(m->buf[i]) * top;
        }
        double e = target_code - written_code;
        esum += e / top;
        esq += (e / top) * (e / top);
        esq_code += e * e;
        errbuf[i] = e;
        px[i] = b;
    }

    FILE *f = fopen(path, "wb");
    if (!f) { free(px); free(errbuf); return fail(err, errlen, "path: cannot open '%s' for writing", path); }
    fprintf(f, "P5\n%d %d\n255\n", m->w, m->h);
    fwrite(px, 1, n, f);
    fclose(f);

    if (st) {
        st->levels = levels;
        st->comp_mean = d->enabled ? csum / (double)n : 0.0;
        st->comp_absmean = d->enabled ? cabs / (double)n : 0.0;
        st->comp_rms = d->enabled ? sqrt(csq / (double)n) : 0.0;
        st->err_mean = esum / (double)n;
        st->err_rms = sqrt(esq / (double)n);
        st->err_code_rms = sqrt(esq_code / (double)n);
        st->err_lf_rms = lowfreq_rms(errbuf, m->w, m->h, SH_BAND_BLUR);
        st->ring_bands = count_ring_bands(m, px, levels);
        st->stage1_ms = 0.0;
        st->stage2_ms = now_ms() - t0;
    }
    free(px);
    free(errbuf);
    return 0;
}

int sh_write_pgm(const ShadowMap *m, const char *path) {
    if (!m || !path) return -1;
    FILE *f = fopen(path, "wb");
    if (!f) return -2;
    fprintf(f, "P5\n%d %d\n255\n", m->w, m->h);
    for (int i = 0; i < m->w * m->h; ++i) {
        unsigned char b = legacy_byte(m->buf[i]);
        fwrite(&b, 1, 1, f);
    }
    fclose(f);
    return 0;
}
