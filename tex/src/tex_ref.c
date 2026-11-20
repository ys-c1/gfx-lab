#include "tex_ref.h"
#include <math.h>
#include <stdio.h>

static unsigned char ref_clamp_byte(double x) {
    if (x < 0.0) x = 0.0;
    if (x > 255.0) x = 255.0;
    return (unsigned char)(x + 0.5);
}

/* 参考：只看基图层。footprint 是图块 texel 空间里的覆盖盒，
 * 盒与每个 texel 的重叠面积即权重；盒小于 1 texel 时退化为双线性。 */
int tex_ref_sample(const MipChain *chain, int atlas_w, int atlas_h,
                   int tile_x, int tile_y, int tile_w, int tile_h,
                   double u, double v, double dudx, double dudy,
                   double dvdx, double dvdy, unsigned char out[3]) {
    if (!chain || !out) {
        fprintf(stderr, "tex_ref_sample error: NULL argument\n");
        return -1;
    }
    if (atlas_w <= 0 || atlas_h <= 0 ||
        chain->w[0] != atlas_w || chain->h[0] != atlas_h ||
        tile_x < 0 || tile_y < 0 || tile_w <= 0 || tile_h <= 0 ||
        tile_x + tile_w > atlas_w || tile_y + tile_h > atlas_h) {
        fprintf(stderr,
                "tex_ref_sample error: tile out of atlas bounds "
                "(atlas %dx%d tile x=%d y=%d w=%d h=%d)\n",
                atlas_w, atlas_h, tile_x, tile_y, tile_w, tile_h);
        return -2;
    }
    const unsigned char *base = chain->pix[0];

    double ex = (fabs(dudx) + fabs(dudy)) * (double)tile_w;
    double ey = (fabs(dvdx) + fabs(dvdy)) * (double)tile_h;
    double cx = u * (double)tile_w;
    double cy = v * (double)tile_h;

    /* 放大（足迹 < 1 texel）：双线性，约定与生产侧 bilinear() 相同 */
    if (ex <= 1.0 && ey <= 1.0) {
        double gx = cx - 0.5, gy = cy - 0.5;
        int x0 = (int)floor(gx), y0 = (int)floor(gy);
        double fx = gx - x0, fy = gy - y0;
        int x1 = x0 + 1, y1 = y0 + 1;
        if (x0 < 0) x0 = 0;
        if (x1 < 0) x1 = 0;
        if (y0 < 0) y0 = 0;
        if (y1 < 0) y1 = 0;
        if (x0 >= tile_w) x0 = tile_w - 1;
        if (x1 >= tile_w) x1 = tile_w - 1;
        if (y0 >= tile_h) y0 = tile_h - 1;
        if (y1 >= tile_h) y1 = tile_h - 1;
        double w00 = (1.0 - fx) * (1.0 - fy), w10 = fx * (1.0 - fy);
        double w01 = (1.0 - fx) * fy, w11 = fx * fy;
        for (int ch = 0; ch < 3; ++ch) {
            int ax0 = tile_x + x0, ax1 = tile_x + x1;
            int ay0 = tile_y + y0, ay1 = tile_y + y1;
            double s = w00 * base[((size_t)ay0 * atlas_w + ax0) * 3 + ch]
                     + w10 * base[((size_t)ay0 * atlas_w + ax1) * 3 + ch]
                     + w01 * base[((size_t)ay1 * atlas_w + ax0) * 3 + ch]
                     + w11 * base[((size_t)ay1 * atlas_w + ax1) * 3 + ch];
            out[ch] = ref_clamp_byte(s);
        }
        return 0;
    }

    /* 缩小：覆盖盒面积加权平均，盒钳在图块边界内 */
    double lox = cx - ex / 2.0, hix = cx + ex / 2.0;
    double loy = cy - ey / 2.0, hiy = cy + ey / 2.0;
    if (lox < 0.0) lox = 0.0;
    if (hix > (double)tile_w) hix = (double)tile_w;
    if (loy < 0.0) loy = 0.0;
    if (hiy > (double)tile_h) hiy = (double)tile_h;
    int ix0 = (int)floor(lox), ix1 = (int)ceil(hix);
    int iy0 = (int)floor(loy), iy1 = (int)ceil(hiy);
    if (ix0 < 0) ix0 = 0;
    if (iy0 < 0) iy0 = 0;
    if (ix1 > tile_w) ix1 = tile_w;
    if (iy1 > tile_h) iy1 = tile_h;

    double sum[3] = {0, 0, 0}, wsum = 0.0;
    for (int ty = iy0; ty < iy1; ++ty) {
        double wy = fmin(hiy, (double)(ty + 1)) - fmax(loy, (double)ty);
        if (wy <= 0.0) continue;
        for (int tx = ix0; tx < ix1; ++tx) {
            double wx = fmin(hix, (double)(tx + 1)) - fmax(lox, (double)tx);
            if (wx <= 0.0) continue;
            double w = wx * wy;
            int ax = tile_x + tx, ay = tile_y + ty;
            const unsigned char *p = base + ((size_t)ay * atlas_w + ax) * 3;
            for (int ch = 0; ch < 3; ++ch) sum[ch] += w * p[ch];
            wsum += w;
        }
    }
    if (wsum <= 0.0) return -1;
    for (int ch = 0; ch < 3; ++ch) out[ch] = ref_clamp_byte(sum[ch] / wsum);
    return 0;
}
