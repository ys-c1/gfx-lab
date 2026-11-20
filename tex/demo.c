#define _POSIX_C_SOURCE 199309L
#include "tex.h"
#include "tex_ref.h"
#include "atlas.h"
#include "atlas.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { TILE = 32, N_TILES = 8 };

static unsigned char tile_data[N_TILES][TILE * TILE * 3];

static void make_tiles(void) {
    for (int t = 0; t < N_TILES; ++t)
        for (int y = 0; y < TILE; ++y)
            for (int x = 0; x < TILE; ++x) {
                unsigned char *p = &tile_data[t][(y * TILE + x) * 3];
                p[0] = (unsigned char)(((x / 2 + y / 2 + t) % 2) ? 235 : 18);
                p[1] = (unsigned char)((x * 7 + y * 11 + t * 29) % 256);
                p[2] = (unsigned char)((x * 13 + t * 41) % 256);
            }
}

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void write_ppm(const char *path, const unsigned char *px, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    fwrite(px, 3, (size_t)w * h, f);
    fclose(f);
}

typedef struct {
    MipChain chain;
    int aw, ah;
    AtlasPos pos[N_TILES];
    int bench_tile; /* 参与对账的图块下标 */
} Scene;

static int build_scene(Scene *s, int aw) {
    make_tiles();
    AtlasRect rects[N_TILES];
    /* 每个图块宽度取满一行，消除横向接缝，只留可控的行间接缝 */
    int tw = aw / 4;
    for (int i = 0; i < N_TILES; ++i) { rects[i].w = tw; rects[i].h = TILE; }
    AtlasStats stats;
    if (atlas_pack(rects, N_TILES, aw, s->pos, &stats) != 0) return 1;
    int ah = stats.shelves * TILE;
    s->aw = aw; s->ah = ah;
    unsigned char *atlas = (unsigned char*)calloc((size_t)aw * ah * 3, 1);
    for (int i = 0; i < N_TILES; ++i) {
        /* 把 32x32 源纹理拉伸到行宽 tw（最近邻），保持各图块内容一致 */
        for (int y = 0; y < TILE; ++y)
            for (int x = 0; x < tw; ++x) {
                int sx = x * TILE / tw;
                for (int ch = 0; ch < 3; ++ch)
                    atlas[((size_t)(s->pos[i].y + y) * aw + (s->pos[i].x + x)) * 3 + ch] =
                        tile_data[i][(y * TILE + sx) * 3 + ch];
            }
    }
    if (mip_build(&s->chain, atlas, aw, ah) != 0) { free(atlas); return 1; }
    free(atlas);
    s->bench_tile = 2;
    return 0;
}

enum { SW = 200, SH = 200, REPS = 50 };

static void set_common(TexReq *r, const Scene *s, double u, double v,
                       double dudx, double dvdy, TexMode mode) {
    memset(r, 0, sizeof(*r));
    int i = s->bench_tile;
    r->chain = &s->chain;
    r->atlas_w = s->aw; r->atlas_h = s->ah;
    r->tile_x = s->pos[i].x; r->tile_y = s->pos[i].y;
    r->tile_w = s->aw / 4; r->tile_h = TILE;
    r->bias_enabled = 1; r->bias = 0.0; r->mode = mode;
    r->u = u; r->v = v;
    r->dudx = dudx; r->dvdx = 0.0;
    r->dudy = 0.0; r->dvdy = dvdy;
}

static void run_row(const Scene *s, int mag, int aniso_case, TexMode mode,
                    unsigned char *img, int emit) {
    int i = s->bench_tile;
    int tw = s->aw / 4;
    double dudx = (double)mag / tw;
    double dvdy = aniso_case ? 1.0 / TILE : (double)mag / TILE;
    unsigned char ref[3], out[3];
    double sq = 0.0; long taps = 0; int n = 0;
    double t0 = now_sec();
    for (int rep = 0; rep < REPS; ++rep)
        for (int sy = 0; sy < SH; ++sy)
            for (int sx = 0; sx < SW; ++sx) {
                double u = ((double)sx + 0.5) / SW;
                double v = ((double)sy + 0.5) / SH;
                if (mode == TEX_LEGACY_POINT) {
                    /* 关闭偏置的现状路径：固定层号点采样（层号取整到最近 mip） */
                    TexReq r = {0};
                    r.chain = &s->chain; r.atlas_w = s->aw; r.atlas_h = s->ah;
                    r.tile_x = s->pos[i].x; r.tile_y = s->pos[i].y;
                    r.tile_w = tw; r.tile_h = TILE;
                    r.bias_enabled = 0;
                    double rho = fmax((fabs(dudx) + 0.0) * tw, (0.0 + fabs(dvdy)) * TILE);
                    r.level = rho > 0.0 ? (int)(log2(rho) + 0.5) : 0;
                    if (r.level >= s->chain.levels) r.level = s->chain.levels - 1;
                    if (r.level < 0) r.level = 0;
                    r.u = u; r.v = v;
                    if (tex_sample(&r, out) != 0) exit(2);
                    taps += r.taps;
                } else {
                    TexReq r;
                    set_common(&r, s, u, v, dudx, dvdy, mode);
                    if (tex_sample(&r, out) != 0) exit(2);
                    taps += r.taps;
                }
                if (rep == 0) {
                    if (tex_ref_sample(&s->chain, s->aw, s->ah,
                                       s->pos[i].x, s->pos[i].y, tw, TILE,
                                       u, v, dudx, 0.0, 0.0, dvdy, ref) != 0) exit(3);
                    for (int ch = 0; ch < 3; ++ch) {
                        double d = (double)out[ch] - (double)ref[ch];
                        sq += d * d; ++n;
                    }
                    if (emit) {
                        unsigned char *p = img + ((size_t)sy * SW + sx) * 3;
                        p[0] = out[0]; p[1] = out[1]; p[2] = out[2];
                    }
                }
            }
    double dt = now_sec() - t0;
    double rmse = sqrt(sq / (double)n);
    double psnr = rmse > 0.0 ? 20.0 * log10(255.0 / rmse) : 99.0;
    const char *mname = mode == TEX_LEGACY_POINT ? "legacy-point"
                       : mode == TEX_TRILINEAR ? "trilinear  " : "aniso-8    ";
    printf("  %s  RMSE=%7.3f  PSNR=%6.2f dB  taps/px=%6.2f  time=%.3f ms\n",
           mname, rmse, psnr, (double)taps / (double)(REPS * SW * SH),
           dt * 1000.0 / REPS);
}

int main(void) {
    int widths[2] = {128, 512};
    int mags[2] = {2, 8};
    unsigned char *strip = (unsigned char*)malloc((size_t)SW * SH * 3 * 3);

    printf("benchmark: screen %dx%d, %d reps/case, reference = base-level footprint box average\n",
           SW, SH, REPS);
    for (int wi = 0; wi < 2; ++wi) {
        Scene s;
        if (build_scene(&s, widths[wi]) != 0) {
            printf("scene build failed for width %d\n", widths[wi]);
            return 1;
        }
        printf("\natlas width=%d height=%d (tile %dx%d, levels=%d)\n",
               s.aw, s.ah, s.aw / 4, TILE, s.chain.levels);
        for (int mi = 0; mi < 2; ++mi) {
            for (int ac = 0; ac < 2; ++ac) {
                printf(" mag=%dx footprint=%s\n", mags[mi], ac ? "anisotropic(8:1)" : "isotropic");
                run_row(&s, mags[mi], ac, TEX_LEGACY_POINT, strip, 0);
                run_row(&s, mags[mi], ac, TEX_TRILINEAR,
                        strip + (size_t)SW * SH * 3, mags[mi] == 2 && !ac && wi == 0);
                run_row(&s, mags[mi], ac, TEX_ANISO,
                        strip + (size_t)SW * SH * 3 * 2, mags[mi] == 2 && !ac && wi == 0);
            }
        }
        char path[64];
        snprintf(path, sizeof(path), "demo-atlas%d.ppm", widths[wi]);
        /* 竖排三联：legacy / trilinear / aniso（最近一组 2x 各向同性画面） */
        unsigned char *vert = (unsigned char*)malloc((size_t)SW * SH * 3 * 3);
        for (int k = 0; k < 3; ++k)
            memcpy(vert + (size_t)k * SW * SH * 3, strip + (size_t)k * SW * SH * 3,
                   (size_t)SW * SH * 3);
        write_ppm(path, vert, SW, SH * 3);
        free(vert);
        mip_free(&s.chain);
        printf(" wrote %s (top=legacy, middle=trilinear, bottom=aniso)\n", path);
    }
    free(strip);
    return 0;
}
