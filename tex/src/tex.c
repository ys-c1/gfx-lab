#include "tex.h"
#include <math.h>
#include <stdio.h>

static int finite_err(const char *field, double x) {
    if (!isfinite(x)) {
        fprintf(stderr, "tex_sample error: field '%s' is not finite (value=%g)\n", field, x);
        return -3;
    }
    return 0;
}

/* 层内双线性：atlas 空间 UV（au/av 与层号无关，层内再按该层尺寸换算）。
 * 约定与点采样一致：texel 中心在像素整数点，UV=0 落在首像素中心。
 * 每次调用固定 4 个点采样，边缘钳制。 */
static void bilinear(const MipChain *c, int l, double au, double av,
                     double acc[3], long *taps) {
    int W = c->w[l], H = c->h[l];
    double gx = au * W - 0.5;
    double gy = av * H - 0.5;
    int x0 = (int)floor(gx), y0 = (int)floor(gy);
    double fx = gx - x0, fy = gy - y0;
    int x1 = x0 + 1, y1 = y0 + 1;
    if (x0 < 0) x0 = 0;
    if (x1 < 0) x1 = 0;
    if (y0 < 0) y0 = 0;
    if (y1 < 0) y1 = 0;
    if (x0 >= W) x0 = W - 1;
    if (x1 >= W) x1 = W - 1;
    if (y0 >= H) y0 = H - 1;
    if (y1 >= H) y1 = H - 1;
    const unsigned char *p = c->pix[l];
    const unsigned char *p00 = p + ((size_t)y0 * W + x0) * 3;
    const unsigned char *p10 = p + ((size_t)y0 * W + x1) * 3;
    const unsigned char *p01 = p + ((size_t)y1 * W + x0) * 3;
    const unsigned char *p11 = p + ((size_t)y1 * W + x1) * 3;
    double w00 = (1.0 - fx) * (1.0 - fy);
    double w10 = fx * (1.0 - fy);
    double w01 = (1.0 - fx) * fy;
    double w11 = fx * fy;
    for (int ch = 0; ch < 3; ++ch)
        acc[ch] += w00 * p00[ch] + w10 * p10[ch] + w01 * p01[ch] + w11 * p11[ch];
    *taps += 4;
}

/* 第三段：相邻两层双线性结果按 t 混合（t=0 时只采底层，省 4 taps） */
static void trilinear_probe(const MipChain *c, int l0, double t,
                            double au, double av, double acc[3], long *taps) {
    double lo[3] = {0, 0, 0}, hi[3] = {0, 0, 0};
    bilinear(c, l0, au, av, lo, taps);
    if (t > 0.0) bilinear(c, l0 + 1, au, av, hi, taps);
    for (int ch = 0; ch < 3; ++ch)
        acc[ch] += (1.0 - t) * lo[ch] + t * hi[ch];
}

static unsigned char clamp_byte(double x) {
    if (x < 0.0) x = 0.0;
    if (x > 255.0) x = 255.0;
    return (unsigned char)(x + 0.5);
}

int tex_sample(TexReq *r, unsigned char out[3]) {
    if (!r) {
        fprintf(stderr, "tex_sample error: field 'req' is NULL\n");
        return -1;
    }
    if (!r->chain) {
        fprintf(stderr, "tex_sample error: field 'chain' is NULL\n");
        return -1;
    }
    if (!out) {
        fprintf(stderr, "tex_sample error: field 'out' is NULL\n");
        return -1;
    }
    const MipChain *c = r->chain;
    r->taps = 0;

    /* ---- 图块越界校验，字段名定位到第一个非法字段 ---- */
    const char *bad = NULL;
    if (r->atlas_w <= 0) bad = "atlas_w";
    else if (r->atlas_h <= 0) bad = "atlas_h";
    else if (c->w[0] != r->atlas_w) bad = "atlas_w";
    else if (c->h[0] != r->atlas_h) bad = "atlas_h";
    else if (r->tile_x < 0) bad = "tile_x";
    else if (r->tile_y < 0) bad = "tile_y";
    else if (r->tile_w <= 0) bad = "tile_w";
    else if (r->tile_h <= 0) bad = "tile_h";
    else if (r->tile_x + r->tile_w > r->atlas_w) bad = "tile_w";
    else if (r->tile_y + r->tile_h > r->atlas_h) bad = "tile_h";
    if (bad) {
        fprintf(stderr,
                "tex_sample error: tile out of atlas bounds (field '%s': "
                "atlas %dx%d tile x=%d y=%d w=%d h=%d)\n",
                bad, r->atlas_w, r->atlas_h, r->tile_x, r->tile_y, r->tile_w, r->tile_h);
        return -2;
    }

    if (finite_err("u", r->u)) return -3;
    if (finite_err("v", r->v)) return -3;

    /* ---- 关闭偏置：旧路径，输出与现状逐像素相同 ---- */
    if (!r->bias_enabled) {
        if (r->level < 0 || r->level >= c->levels) {
            fprintf(stderr, "tex_sample error: level out of range (field 'level': %d not in [0,%d))\n",
                    r->level, c->levels);
            return -4;
        }
        /* 恒等图块直接调用 mip_sample：同一整数算术，字节一致 */
        if (r->tile_x == 0 && r->tile_y == 0 &&
            r->tile_w == c->w[0] && r->tile_h == c->h[0]) {
            int rc = mip_sample(c, r->level, r->u, r->v, out);
            r->taps = 1;
            return rc;
        }
        double au = (r->tile_x + r->u * r->tile_w) / (double)r->atlas_w;
        double av = (r->tile_y + r->v * r->tile_h) / (double)r->atlas_h;
        int rc = mip_sample(c, r->level, au, av, out);
        r->taps = 1;
        return rc;
    }

    if (finite_err("bias", r->bias)) return -3;
    if (r->mode != TEX_TRILINEAR && r->mode != TEX_ANISO) {
        fprintf(stderr, "tex_sample error: unknown mode (field 'mode': %d)\n", r->mode);
        return -5;
    }
    if (finite_err("dudx", r->dudx)) return -3;
    if (finite_err("dudy", r->dudy)) return -3;
    if (finite_err("dvdx", r->dvdx)) return -3;
    if (finite_err("dvdy", r->dvdy)) return -3;

    /* ============================================================
     * 第一段：按导数算基础层号
     * rho = 图块内 texel 足迹（Manhattan 边沿，与常规 LOD 公式一致），
     * lambda = log2(rho_max) + bias，钳到 [0, levels)。
     * ============================================================ */
    double sx = (double)r->tile_w * (fabs(r->dudx) + fabs(r->dudy));
    double sy = (double)r->tile_h * (fabs(r->dvdx) + fabs(r->dvdy));
    double rho_max = fmax(sx, sy);
    double rho_min = fmin(sx, sy);
    double lam = rho_max > 0.0 ? log2(rho_max) : 0.0;
    lam += r->bias;
    if (lam < 0.0) lam = 0.0;
    if (lam >= (double)c->levels) {
        fprintf(stderr, "tex_sample error: computed level out of range (field 'lod': %.6f not in [0,%d))\n",
                lam, c->levels);
        return -4;
    }

    /* 图集空间 UV 与层号无关：分子与层宽按同一比例缩放 */
    double u0 = r->u, v0 = r->v;
    if (u0 < 0.0) u0 = 0.0; else if (u0 >= 1.0) u0 = 1.0 - 1e-12;
    if (v0 < 0.0) v0 = 0.0; else if (v0 >= 1.0) v0 = 1.0 - 1e-12;

    double acc[3] = {0, 0, 0};

    if (r->mode == TEX_TRILINEAR) {
        int l0 = (int)floor(lam);
        double t = lam - l0;
        if (l0 >= c->levels - 1) { l0 = c->levels - 1; t = 0.0; }
        double au = (r->tile_x + u0 * r->tile_w) / (double)r->atlas_w;
        double av = (r->tile_y + v0 * r->tile_h) / (double)r->atlas_h;
        trilinear_probe(c, l0, t, au, av, acc, &r->taps);
        for (int ch = 0; ch < 3; ++ch) out[ch] = clamp_byte(acc[ch]);
        return 0;
    }

    /* ============================================================
     * 第二段：按长宽比拆子样本（各向异性）
     * N = round(rho_max/rho_min)，沿长轴均匀放 N 个探针；
     * 子样本层号再减去 log2(N)，把短轴足迹压回 ~1 texel。
     * ============================================================ */
    int N = 1;
    if (rho_min > 0.0 && rho_max > rho_min) {
        N = (int)floor(rho_max / rho_min + 0.5);
        if (N < 1) N = 1;
        if (N > TEX_MAX_ANISO) N = TEX_MAX_ANISO;
    }
    double sub_lam = lam - log2((double)N);
    if (sub_lam < 0.0) sub_lam = 0.0;
    int l0 = (int)floor(sub_lam);
    double t = sub_lam - l0;
    if (l0 >= c->levels - 1) { l0 = c->levels - 1; t = 0.0; }

    int x_major = sx >= sy;
    for (int k = 0; k < N; ++k) {
        double off = (double)k - (double)(N - 1) / 2.0; /* 以采样点为中心 */
        off /= (double)N;                                /* 每个探针间距 1/N 像素 */
        double uk, vk;
        if (x_major) {
            uk = u0 + off * r->dudx;
            vk = v0 + off * r->dvdx;
        } else {
            uk = u0 + off * r->dudy;
            vk = v0 + off * r->dvdy;
        }
        if (uk < 0.0) uk = 0.0; else if (uk >= 1.0) uk = 1.0 - 1e-12;
        if (vk < 0.0) vk = 0.0; else if (vk >= 1.0) vk = 1.0 - 1e-12;
        double au = (r->tile_x + uk * r->tile_w) / (double)r->atlas_w;
        double av = (r->tile_y + vk * r->tile_h) / (double)r->atlas_h;
        /* 第三段在 trilinear_probe 内完成：层内双线性 + 相邻层混合 */
        trilinear_probe(c, l0, t, au, av, acc, &r->taps);
    }
    for (int ch = 0; ch < 3; ++ch) out[ch] = clamp_byte(acc[ch] / (double)N);
    return 0;
}
