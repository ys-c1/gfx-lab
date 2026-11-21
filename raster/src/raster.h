#ifndef RASTER_H
#define RASTER_H
typedef struct { double x, y, z; } RVert;
typedef struct { RVert v[3]; unsigned char rgb[3]; } RTri;
typedef struct { int w, h; float *z; unsigned char *rgb; } RTarget;
typedef struct { double x, y, z; double size; unsigned char rgb[3]; } RSprite;
int rt_init(RTarget *t, int w, int h);
void rt_free(RTarget *t);
int rt_triangle(RTarget *t, const RTri *tri);
/* Draw n point sprites.
   focal > 0: perspective scaling on, screen size = size * focal / z.
   focal == 0: scaling off, every sprite drawn at its fixed size.
   Each pixel is written with its coverage of the sprite square:
   color = coverage * sprite + (1 - coverage) * previous, z-tested
   against the sprite depth.
   Returns the number of sprites drawn, or -1 on error; on error *bad
   (when non-NULL) is the index of the offending sprite (size == 0, or
   non-finite depth while scaling), or -1 for invalid arguments. */
int rt_sprites(RTarget *t, const RSprite *sp, int n, double focal, int *bad);
int rt_write_ppm(const RTarget *t, const char *path);
#endif
