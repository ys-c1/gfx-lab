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

/* ---- transfer functions ------------------------------------------------- */

int sh_gamma_valid(float gamma) {
    return isfinite(gamma) && gamma > 0.0f;
}

float sh_srgb_encode(float linear) {
    if (!(linear > 0.0f)) return 0.0f; /* also traps NaN */
    if (linear >= 1.0f) return 1.0f;
    if (linear <= 0.0031308f) return 12.92f * linear;
    return 1.055f * powf(linear, 1.0f / 2.4f) - 0.055f;
}

float sh_srgb_decode(float encoded) {
    if (!(encoded > 0.0f)) return 0.0f;
    if (encoded >= 1.0f) return 1.0f;
    if (encoded <= 0.04045f) return encoded / 12.92f;
    return powf((encoded + 0.055f) / 1.055f, 2.4f);
}

float sh_encode(float linear, int srgb, float gamma) {
    if (!srgb) return linear;
    if (gamma == SH_SRGB_GAMMA) return sh_srgb_encode(linear);
    if (gamma == 1.0f) return linear;
    if (!(linear > 0.0f)) return 0.0f;
    if (linear >= 1.0f) return 1.0f;
    return powf(linear, 1.0f / gamma);
}

float sh_decode(float encoded, int srgb, float gamma) {
    if (!srgb) return encoded;
    if (gamma == SH_SRGB_GAMMA) return sh_srgb_decode(encoded);
    if (gamma == 1.0f) return encoded;
    if (!(encoded > 0.0f)) return 0.0f;
    if (encoded >= 1.0f) return 1.0f;
    return powf(encoded, gamma);
}

/* ---- quantization ------------------------------------------------------- */

int sh_quant_table_size(int bits) {
    if (bits == 8) return 256;
    if (bits == 16) return 65536;
    return SH_E_BITS;
}

int sh_quant_table(int bits, int srgb, float gamma, float *table) {
    if (!table) return SH_E_ARG;
    int n = sh_quant_table_size(bits);
    if (n < 0) return n;
    if (!sh_gamma_valid(gamma)) return SH_E_GAMMA;
    float maxcode = (float)(n - 1);
    for (int i = 0; i < n; ++i)
        table[i] = sh_decode((float)i / maxcode, srgb, gamma);
    return SH_OK;
}

int sh_quantize_code(float linear, int bits, int srgb, float gamma,
                     unsigned int *code) {
    if (!code) return SH_E_ARG;
    int n = sh_quant_table_size(bits);
    if (n < 0) return n;
    if (!sh_gamma_valid(gamma)) return SH_E_GAMMA;
    float maxcode = (float)(n - 1);
    float e = sh_encode(linear, srgb, gamma);
    if (!(e > 0.0f)) e = 0.0f;
    if (e > 1.0f) e = 1.0f;
    /* Round half up. For bits==8, srgb==0 this is exactly the old writer. */
    unsigned int c = (unsigned int)(e * maxcode + 0.5f);
    if (c > (unsigned int)maxcode) c = (unsigned int)maxcode;
    *code = c;
    return SH_OK;
}

int sh_quantize(const ShadowMap *m, int bits, int srgb, float gamma,
                void **out, size_t *out_size) {
    if (!m || !out) return SH_E_ARG;
    int n = sh_quant_table_size(bits);
    if (n < 0) return n;
    if (!sh_gamma_valid(gamma)) return SH_E_GAMMA;
    if (!m->buf || m->w <= 0 || m->h <= 0) return SH_E_IMG;

    size_t count = (size_t)m->w * (size_t)m->h;
    size_t elem = (bits == 8) ? 1 : 2;
    void *p = malloc(count * elem);
    if (!p) return SH_E_NOMEM;
    float maxcode = (float)(n - 1);
    if (bits == 8) {
        unsigned char *d = (unsigned char*)p;
        for (size_t i = 0; i < count; ++i) {
            float e = sh_encode(m->buf[i], srgb, gamma);
            if (!(e > 0.0f)) e = 0.0f;
            if (e > 1.0f) e = 1.0f;
            d[i] = (unsigned char)(e * maxcode + 0.5f);
        }
    } else {
        unsigned short *d = (unsigned short*)p;
        for (size_t i = 0; i < count; ++i) {
            float e = sh_encode(m->buf[i], srgb, gamma);
            if (!(e > 0.0f)) e = 0.0f;
            if (e > 1.0f) e = 1.0f;
            d[i] = (unsigned short)(e * maxcode + 0.5f);
        }
    }
    *out = p;
    if (out_size) *out_size = count * elem;
    return SH_OK;
}

int sh_quantize_u8(const ShadowMap *m, int srgb, float gamma,
                   unsigned char **out, size_t *out_size) {
    if (!out) return SH_E_ARG;
    void *p = NULL;
    int rc = sh_quantize(m, 8, srgb, gamma, &p, out_size);
    *out = (unsigned char*)p;
    return rc;
}

int sh_quantize_u16(const ShadowMap *m, int srgb, float gamma,
                    unsigned short **out, size_t *out_size) {
    if (!out) return SH_E_ARG;
    void *p = NULL;
    int rc = sh_quantize(m, 16, srgb, gamma, &p, out_size);
    *out = (unsigned short*)p;
    return rc;
}

int sh_write_pgm(const ShadowMap *m, const char *path, int bits, int srgb,
                 float gamma) {
    if (!path) return SH_E_ARG;
    void *pix = NULL;
    size_t nbytes = 0;
    int rc = sh_quantize(m, bits, srgb, gamma, &pix, &nbytes);
    if (rc != SH_OK) return rc;
    FILE *f = fopen(path, "wb");
    if (!f) { free(pix); return SH_E_IO; }
    int ok;
    if (bits == 8) {
        ok = fprintf(f, "P5\n%d %d\n255\n", m->w, m->h) > 0;
        ok = ok && fwrite(pix, 1, nbytes, f) == nbytes;
    } else {
        ok = fprintf(f, "P5\n%d %d\n65535\n", m->w, m->h) > 0;
        /* Netpbm wants big-endian samples for maxval > 255. */
        unsigned short *s = (unsigned short*)pix;
        for (size_t i = 0; ok && i < nbytes / 2; ++i) {
            unsigned char be[2] = { (unsigned char)(s[i] >> 8),
                                    (unsigned char)(s[i] & 0xFF) };
            ok = fwrite(be, 1, 2, f) == 2;
        }
    }
    if (fclose(f) != 0) ok = 0;
    free(pix);
    return ok ? SH_OK : SH_E_IO;
}

const char *sh_strerror(int rc) {
    switch (rc) {
    case SH_OK:      return "ok";
    case SH_E_ARG:   return "invalid argument";
    case SH_E_IO:    return "i/o error";
    case SH_E_BITS:  return "unsupported bit depth (expected 8 or 16)";
    case SH_E_GAMMA: return "invalid gamma (expected finite and > 0)";
    case SH_E_NOMEM: return "out of memory";
    case SH_E_IMG:   return "image has no pixels";
    default:         return "unknown error";
    }
}

const char *sh_field_name(int rc) {
    switch (rc) {
    case SH_E_BITS:  return "bits";
    case SH_E_GAMMA: return "gamma";
    default:         return NULL;
    }
}
