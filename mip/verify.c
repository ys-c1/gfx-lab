/* mip 半纹素中心修正验证程序
 * 输出：两档倍率(1x/2x)下的误差、采样次数、偏移量、纹素坐标、修正次数、
 *       修正前后差值；越界访问检出并报出样本编号；与纸笔推导数值对照。 */
#include "mip.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { W = 8, H = 8 };

/* 独立整数参考实现（不经过 mip 库的浮点路径）：
 * 中心寻址 uv -> raw = floor(p - 0.5)，越界 clamp 到安全纹素。
 * 与库新路径返回值逐字节比对，统计误差。 */
static void ref_center(const MipChain *c, int level, double u, double v, unsigned char out[3]) {
    int Ww = c->w[level], Hh = c->h[level];
    int rx = (int)floor(u * (double)Ww - 0.5);
    int ry = (int)floor(v * (double)Hh - 0.5);
    if (rx < 0) rx = 0;
    if (ry < 0) ry = 0;
    if (rx >= Ww) rx = Ww - 1;
    if (ry >= Hh) ry = Hh - 1;
    for (int ch = 0; ch < 3; ++ch) out[ch] = c->pix[level][(ry*Ww+rx)*3+ch];
}

static int errors_total = 0;
static int oob_ids[4096], oob_n = 0;
static int grid_corr_axis = 0;     /* 全网格修正轴次（含越界轴的 raw/old 差异另算，这里按库统计口径） */
static int grid_corr_samples = 0;  /* 非越界且至少一轴被修正的样本数 */
static long sum_dr = 0, sum_dg = 0;
static int n_diff_rgb = 0;

static void run_gear(const MipChain *c, int N, const char *gear,
                     int exp_oob, int exp_corr_axis, int exp_corr_samples) {
    MipDiag d;
    mip_diag_init(&d, 1);
    int ref_err = 0, ex_oob = 0, diag_samples_corr = 0;
    int prev_oob_n = oob_n;
    long abs_dr = 0, abs_dg = 0;
    sum_dr = sum_dg = 0; n_diff_rgb = 0; grid_corr_axis = grid_corr_samples = 0;

    printf("================ %s 档：N=%d，网格 %d×%d = %d 个样本 ================\n",
           gear, N, N, N, N * N);

    for (int id = 0; id < N * N; ++id) {
        int i = id % N, j = id / N;
        double u = (double)i / (double)N;
        double v = (double)j / (double)N;

        unsigned char neo[3], old[3], ref[3];
        int rc = mip_sample_ex(c, 0, u, v, neo, &d);
        mip_sample(c, 0, u, v, old);
        ref_center(c, 0, u, v, ref);

        if (rc == 1) { ++ex_oob; oob_ids[oob_n++] = id; }
        if (memcmp(neo, ref, 3) != 0) ++ref_err;

        int cx = (d.last_raw_x >= 0 && d.last_raw_x < W && d.last_raw_x != d.last_old_x);
        int cy = (d.last_raw_y >= 0 && d.last_raw_y < H && d.last_raw_y != d.last_old_y);
        if (cx || cy) ++grid_corr_axis;
        if (rc != 1 && (cx || cy)) { ++grid_corr_samples; ++diag_samples_corr; }

        int dr = (int)neo[0] - (int)old[0];
        int dg = (int)neo[1] - (int)old[1];
        abs_dr += abs(dr); abs_dg += abs(dg);
        sum_dr += dr; sum_dg += dg;
        if (neo[0] != old[0] || neo[1] != old[1] || neo[2] != old[2]) ++n_diff_rgb;

        /* 对角线 i=j 逐点详表：偏移量、修正前后纹素坐标、修正前后 RGB 与差值 */
        if (i == j) {
            printf("id=%4d u=%.4f v=%.4f | 偏移 ou=ov=%.6f | 老坐标(%d,%d) 修正raw(%d,%d)%s"
                   " | 老RGB(%3u,%3u,%3u) 新RGB(%3u,%3u,%3u) | 差值 ΔR=%+d ΔG=%+d\n",
                   id, u, v, d.last_ou, d.last_old_x, d.last_old_y,
                   d.last_raw_x, d.last_raw_y, rc == 1 ? " OOB" : "    ",
                   old[0], old[1], old[2], neo[0], neo[1], neo[2], dr, dg);
        }
    }

    int oob_here = oob_n - prev_oob_n;
    printf("--- %s 档统计 ---\n", gear);
    printf("采样次数(诊断计数)   : %d（网格样本 %d）\n", d.samples, N * N);
    printf("纹素中心偏移量 level0: ou=ov=%.6f（即 0.5/%d）\n", 0.5 / W, W);
    printf("误差(新路径 vs 独立参考): %d 处字节不一致\n", ref_err);
    printf("修正轴次(库计数)     : %d | 修正样本数(非OOB且坐标变化): %d\n",
           d.corrections, grid_corr_samples);
    printf("越界检出             : %d 个，编号 =", oob_here);
    for (int k = prev_oob_n; k < oob_n; ++k) printf(" %d", oob_ids[k]);
    printf("\n");
    printf("修正前后差值         : RGB 不同样本 %d 个；Σ|ΔR|=%ld Σ|ΔG|=%ld，带符号 ΣΔR=%ld ΣΔG=%ld（B 恒定 Δ=0）\n",
           n_diff_rgb, abs_dr, abs_dg, sum_dr, sum_dg);

    /* 纸笔推导对照 */
    printf("--- %s 档纸笔推导对照 ---\n", gear);
    int pass = 1;
    if (d.samples != N*N) { printf("  不符: 采样次数 %d != %d\n", d.samples, N*N); pass = 0; }
    if (ref_err != 0)     { printf("  不符: 参考误差 %d != 0\n", ref_err); pass = 0; }
    if (oob_here != exp_oob) { printf("  不符: 越界 %d != 预期 %d\n", oob_here, exp_oob); pass = 0; }
    if (d.corrections != exp_corr_axis) { printf("  不符: 修正轴次 %d != 预期 %d\n", d.corrections, exp_corr_axis); pass = 0; }
    if (grid_corr_samples != exp_corr_samples) { printf("  不符: 修正样本 %d != 预期 %d\n", grid_corr_samples, exp_corr_samples); pass = 0; }
    if (pass) printf("  全部符合纸笔预期（OOB=%d, 修正轴次=%d, 修正样本=%d, 误差=0）\n",
                     exp_oob, exp_corr_axis, exp_corr_samples);
    else errors_total += 1;
    (void)diag_samples_corr;
    printf("\n");
}

/* 关闭新逻辑后必须与老路径字节一致 */
static void check_disabled_byte_identical(const MipChain *c) {
    MipDiag d;
    mip_diag_init(&d, 0);
    int mismatch = 0, n = 0;
    double probes[] = {0.0, 0.03125, 0.1, 0.5, 0.7, 0.9375, 1.0, -0.1, 1.05};
    for (size_t a = 0; a < sizeof(probes)/sizeof(probes[0]); ++a)
        for (size_t b = 0; b < sizeof(probes)/sizeof(probes[0]); ++b) {
            unsigned char p1[3], p2[3];
            int r1 = mip_sample_ex(c, 0, probes[a], probes[b], p1, &d);
            int r2 = mip_sample(c, 0, probes[a], probes[b], p2);
            if (r1 != r2 || memcmp(p1, p2, 3) != 0) ++mismatch;
            ++n;
        }
    printf("关闭新逻辑字节一致性: %d 次采样，%d 处与 mip_sample 不一致（诊断 samples=%d, corrections=%d, oob=%d）\n\n",
           n, mismatch, d.samples, d.corrections, d.oob);
    if (mismatch || d.corrections || d.oob) errors_total += 1;
}

int main(void) {
    unsigned char *src = (unsigned char*)malloc(W * H * 3);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            unsigned char *p = src + (y * W + x) * 3;
            p[0] = (unsigned char)(x * 32);  /* R 随 x：纹素 i 中心值 32i */
            p[1] = (unsigned char)(y * 32);  /* G 随 y */
            p[2] = 128;                       /* B 恒定 */
        }
    MipChain c;
    if (mip_build(&c, src, W, H) != 0) return 2;

    printf("纹理 %dx%d，level0 偏移 ou=ov=0.5/%d=%.6f；level1 尺寸 %dx%d 偏移 %.6f\n",
           W, H, W, 0.5 / W, c.w[1], c.h[1], 0.5 / c.w[1]);
    printf("纸笔推导 1D(W=8): 1x 档 k=0 -> raw=-1 越界; k=1..7 raw=k-1 修正\n");
    printf("                2x 档 k=0 -> raw=-1; 奇 k raw=(k-1)/2 相同; 偶 k>=2 raw=k/2-1 修正\n\n");

    check_disabled_byte_identical(&c);

    /* 预期值来自纸笔推导：
     * 1x: 8x8 网格，OOB 行/列 0 共 15 个；修正轴次 2*7*8=112；修正样本 7*7=49
     * 2x: 16x16 网格，OOB 行/列 0 共 31 个；修正轴次 2*7*16=224；修正样本 非OOB 225 - 未修正 64 = 161 */
    run_gear(&c, W,   "1x", 15, 112, 49);
    run_gear(&c, 2*W, "2x", 31, 224, 161);

    printf("======== 总结：%s ========\n", errors_total == 0 ? "全部对照通过" : "存在不符项");
    mip_free(&c);
    free(src);
    return errors_total ? 1 : 0;
}
