#include "mip.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

enum { TW = 256, TH = 256, SW = 512, SH = 256 };

/* 简单地面投影：horizon 以下为地面，uv 随深度压缩；
 * 各向异性从底部约 1:1 升到地平线附近约 15:1，uv 始终落在 [0,1] 内 */
static void ground_uv(double sx, double sy, double *u, double *v) {
    const double horizon = SH * 0.40;
    double fy = (sy - horizon) / (double)(SH - horizon);
    if (fy < 1e-3) fy = 1e-3;
    double d = 1.0 / fy;
    if (d > 4.0) d = 4.0;
    *u = 0.5 + (sx / (double)SW - 0.5) * d * 0.25;
    *v = 0.95 - 0.9 * sqrt(fy);
}

int main(void) {
    unsigned char *tex = (unsigned char*)malloc((size_t)TW*TH*3);
    if (!tex) return 1;
    for (int y = 0; y < TH; ++y)
        for (int x = 0; x < TW; ++x) {
            int check = ((x / 8) + (y / 8)) & 1;
            unsigned char *p = tex + (y*TW+x)*3;
            if (check) { p[0] = 235; p[1] = 230; p[2] = 220; }
            else       { p[0] = 45;  p[1] = 60;  p[2] = 80;  }
        }
    MipChain c;
    if (mip_build(&c, tex, TW, TH) != 0) return 1;
    printf("levels %d sizes %dx%d .. %dx%d\n", c.levels, c.w[0], c.h[0],
           c.w[c.levels-1], c.h[c.levels-1]);

    unsigned char *img = (unsigned char*)malloc((size_t)SW*SH*3);
    if (!img) return 1;
    const double horizon = SH * 0.40;
    for (int sy = 0; sy < SH; ++sy)
        for (int sx = 0; sx < SW; ++sx) {
            unsigned char *p = img + (sy*SW+sx)*3;
            if (sy < (int)horizon) {  /* 天空 */
                double t = sy / horizon;
                p[0] = (unsigned char)(100 + 80*t);
                p[1] = (unsigned char)(140 + 60*t);
                p[2] = (unsigned char)(200 + 20*t);
                continue;
            }
            double u, v, ux, vx, uy, vy;
            ground_uv(sx + 0.5, sy + 0.5, &u, &v);
            ground_uv(sx + 1.5, sy + 0.5, &ux, &vx);
            ground_uv(sx + 0.5, sy + 1.5, &uy, &vy);
            int max_aniso = sx < SW/2 ? 1 : 16;  /* 左：关闭，右：16x */
            MipAnisoInfo inf;
            if (mip_sample_aniso(&c, u, v, ux-u, vx-v, uy-u, vy-v, max_aniso, p, &inf) != MIP_OK)
                return 1;
        }

    FILE *f = fopen("demo.ppm", "wb");
    if (!f) return 1;
    fprintf(f, "P6\n%d %d\n255\n", SW, SH);
    fwrite(img, 1, (size_t)SW*SH*3, f);
    fclose(f);
    printf("wrote demo.ppm (left: isotropic, right: aniso 16x)\n");

    free(img);
    mip_free(&c);
    free(tex);
    return 0;
}
