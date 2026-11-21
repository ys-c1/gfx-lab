#ifndef SHADOW_H
#define SHADOW_H

/* return codes */
#define SHADOW_OK        0
#define SHADOW_EINVAL   -1   /* null pointer / bad dimension / bad segment count */
#define SHADOW_ELIGHT   -2   /* light.radius <= 0 or light.intensity < 0 */
#define SHADOW_ESAMPLES -3   /* field pcf.samples must be > 0 */
#define SHADOW_EKERNEL  -4   /* field pcf.kernel must be a positive odd number */

typedef struct { float x, y, radius, intensity; } ShLight;
typedef struct { float x0, y0, x1, y1; } ShSeg;
typedef struct { int w, h; float *buf; } ShadowMap;

/* PCF configuration: samples x samples probe points inside a kernel x kernel
   pixel window. samples must be > 0; kernel must be a positive odd number. */
typedef struct { int samples; int kernel; } ShPCF;

/* Exportable kernel table: ox/oy hold samples*samples pixel-space offsets
   relative to the pixel centre. soft_width = kernel - 1 pixels, the full
   width of the generated penumbra band. */
typedef struct {
    int samples;
    int kernel;
    float soft_width;
    float *ox;
    float *oy;
} ShKernel;

int sh_init(ShadowMap *m, int w, int h);
void sh_free(ShadowMap *m);
int sh_clear(ShadowMap *m);

/* hard-edge renderer, unchanged behaviour */
int sh_render(ShadowMap *m, const ShLight *light, const ShSeg *segs, int nseg);

/* multi-sample soft-shadow renderer. pcf == NULL reproduces sh_render
   bit-for-bit. otherwise returns SHADOW_ESAMPLES / SHADOW_EKERNEL with the
   offending field named through sh_errstr. */
int sh_render_pcf(ShadowMap *m, const ShLight *light, const ShSeg *segs,
                  int nseg, const ShPCF *pcf);

/* kernel table build / export. same field validation as sh_render_pcf. */
int  sh_kernel_build(ShKernel *k, int samples, int kernel);
void sh_kernel_free(ShKernel *k);
int  sh_kernel_write(const ShKernel *k, const char *path);

const char *sh_errstr(int code);
int sh_write_pgm(const ShadowMap *m, const char *path);
#endif
