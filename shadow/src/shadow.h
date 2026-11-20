#ifndef SHADOW_H
#define SHADOW_H
#include <stddef.h>

typedef struct { float x, y, radius, intensity; } ShLight;
typedef struct { float x0, y0, x1, y1; } ShSeg;
typedef struct { int w, h; float *buf; } ShadowMap;

/* Status codes. sh_init/sh_clear/sh_render keep their own codes. */
#define SH_OK        0
#define SH_E_ARG    (-1)  /* NULL pointer or unusable argument        */
#define SH_E_IO     (-2)  /* file could not be opened/written         */
#define SH_E_BITS   (-3)  /* field "bits": only 8 and 16 are allowed  */
#define SH_E_GAMMA  (-4)  /* field "gamma": must be finite and > 0    */
#define SH_E_NOMEM  (-5)
#define SH_E_IMG    (-6)  /* shadow map has zero/negative extent      */

/* Default display exponent. Exactly this value selects the IEC 61966-2-1
 * sRGB curve (12.92 / 1.055 / 0.055 piecewise); another positive exponent
 * selects a pure power curve, linear^(1/gamma). gamma == 1 is identity. */
#define SH_SRGB_GAMMA 2.4f

int sh_init(ShadowMap *m, int w, int h);
void sh_free(ShadowMap *m);
int sh_clear(ShadowMap *m);
int sh_render(ShadowMap *m, const ShLight *light, const ShSeg *segs, int nseg);

/* --- linear <-> display transfer --------------------------------------- */
/* gamma is validated even when srgb == 0, so a bad config is never silent. */
int   sh_gamma_valid(float gamma);
float sh_srgb_encode(float linear);                 /* exact sRGB, gamma 2.4 */
float sh_srgb_decode(float encoded);
float sh_encode(float linear, int srgb, float gamma); /* srgb==0 -> identity */
float sh_decode(float encoded, int srgb, float gamma);

/* --- quantization table -------------------------------------------------
 * The table is indexed by the destination code and holds the linear value
 * that code reconstructs to. Entry count = sh_quant_table_size(bits):
 *   bits 8  -> 256 entries,   table[i] = i / 255
 *   bits 16 -> 65536 entries, table[i] = i / 65535
 * with an sRGB (or gamma) decode applied when srgb != 0.
 * Forward mapping (used by the quantizer) is
 *   code = round(encode(linear) * maxcode), clamped to [0, maxcode].
 */
int sh_quant_table_size(int bits);                    /* 256/65536 or SH_E_BITS */
int sh_quant_table(int bits, int srgb, float gamma, float *table);

/* Quantize one linear sample. *code is in [0, 255] or [0, 65535]. */
int sh_quantize_code(float linear, int bits, int srgb, float gamma,
                     unsigned int *code);

/* Quantize the shadow map's linear buffer. The sRGB/gamma transfer runs
 * first, then the result is clamped and rounded to `bits`. *out_size is the
 * payload size (w*h for 8 bit, 2*w*h for 16 bit).
 * With bits==8, srgb==0 the byte values are bit-identical to the original
 * linear writer (same float arithmetic, same rounding). */
int sh_quantize(const ShadowMap *m, int bits, int srgb, float gamma,
                void **out, size_t *out_size);
int sh_quantize_u8(const ShadowMap *m, int srgb, float gamma,
                   unsigned char **out, size_t *out_size);
int sh_quantize_u16(const ShadowMap *m, int srgb, float gamma,
                    unsigned short **out, size_t *out_size);

/* Write a binary PGM. 8 bit uses maxval 255, 16 bit uses maxval 65535 with
 * big-endian samples as required by Netpbm. */
int sh_write_pgm(const ShadowMap *m, const char *path, int bits, int srgb,
                 float gamma);

/* Human-readable status / the offending field ("bits", "gamma"), or NULL. */
const char *sh_strerror(int rc);
const char *sh_field_name(int rc);
#endif
