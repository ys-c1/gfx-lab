#include "shadow.h"
#include "softshadow.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

/* 独立参考实现：朴素二维高斯卷积（直接 2D，不走可分离），
 * 与被测的两次一维卷积是两条独立代码路径，用于逐像素对照。 */
static void ref_gaussian2d(const float *src, float *dst, int w, int h,
                           int r, const float *k1d, int border) {
    int klen = 2 * r + 1;
    float *k2d = malloc((size_t)klen * klen * sizeof(float));
    for (int j = 0; j < klen; ++j)
        for (int i = 0; i < klen; ++i)
            k2d[j * klen + i] = k1d[i] * k1d[j];   /* 可分离核 = 外积 */
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            float acc = 0.0f;
            for (int j = -r; j <= r; ++j) {
                for (int i = -r; i <= r; ++i) {
                    int sx = x + i, sy = y + j;
                    float v;
                    if (border == SB_BORDER_ZERO) {
                        v = (sx < 0 || sy < 0 || sx >= w || sy >= h)
                            ? 0.0f : src[sy * w + sx];
                    } else {
                        if (sx < 0) sx = 0; else if (sx >= w) sx = w - 1;
                        if (sy < 0) sy = 0; else if (sy >= h) sy = h - 1;
                        v = src[sy * w + sx];
                    }
                    acc += k2d[(j + r) * klen + (i + r)] * v;
                }
            }
            dst[y * w + x] = acc;
        }
    }
    free(k2d);
}

static int feq(float a, float b, float eps) { return fabsf(a - b) <= eps; }

int main(void) {
    /* ---- 1. 核：归一化、对称、奇长、可手算的具体权重 ---- */
    float k[9];
    int len = sb_kernel(1, 0.5f, k);
    CK(len == 3);
    CK(feq(k[0] + k[1] + k[2], 1.0f, 1e-6f));
    CK(feq(k[0], k[2], 1e-7f));
    /* r=1,sigma=0.5：w0=e^0=1，w1=e^-2≈0.135335；归一化后 w1≈0.78677 */
    float e2 = expf(-2.0f);
    CK(feq(k[1], 1.0f / (1.0f + 2.0f * e2), 1e-5f));
    CK(feq(k[0], e2 / (1.0f + 2.0f * e2), 1e-5f));
    CK(sb_kernel(2, 0.5f, k) == 5);              /* 2r+1 恒奇 */
    CK(sb_kernel(0, 0.5f, k) == 1 && feq(k[0], 1.0f, 1e-7f));

    /* ---- 2. 报错：半径为零 / 核长为偶，错误信息指出字段 ---- */
    CK(sb_check_kernel_len(0, 1) == SB_ERR_RADIUS_ZERO);
    CK(sb_check_kernel_len(4, 8) == SB_ERR_KERNEL_EVEN);   /* 偶核长 */
    CK(sb_check_kernel_len(4, 10) == SB_ERR_KERNEL_EVEN);  /* 偶且不匹配 */
    CK(sb_check_kernel_len(4, 7) == SB_ERR_KERNEL_EVEN);   /* 奇但 != 2r+1 */
    CK(sb_check_kernel_len(4, 9) == SB_OK);
    printf("  %s\n", sb_error_msg(SB_ERR_RADIUS_ZERO));
    printf("  %s\n", sb_error_msg(SB_ERR_KERNEL_EVEN));

    SbConfig bad_r = {.enable=1, .blur_radius_px=0, .blur_radius_min=0, .dist_begin=5.0f, .dist_scale=1.0f, .sigma_scale=0.5f, .border_mode=SB_BORDER_CLAMP};
    CK(sb_validate_config(&bad_r) == SB_ERR_RADIUS_ZERO);
    SbConfig bad_sigma = {.enable=1, .blur_radius_px=3, .blur_radius_min=0, .dist_begin=5.0f, .dist_scale=1.0f, .sigma_scale=0.0f, .border_mode=SB_BORDER_CLAMP};
    CK(sb_validate_config(&bad_sigma) == SB_ERR_SIGMA);
    SbConfig bad_border = {.enable=1, .blur_radius_px=3, .blur_radius_min=0, .dist_begin=5.0f, .dist_scale=1.0f, .sigma_scale=0.5f, .border_mode=99};
    CK(sb_validate_config(&bad_border) == SB_ERR_BORDER);
    SbConfig bad_min = {.enable=1, .blur_radius_px=3, .blur_radius_min=4, .dist_begin=5.0f, .dist_scale=1.0f, .sigma_scale=0.5f, .border_mode=SB_BORDER_CLAMP};
    CK(sb_validate_config(&bad_min) == SB_ERR_RADIUS_MIN);
    SbConfig ok_min = {.enable=1, .blur_radius_px=3, .blur_radius_min=3, .dist_begin=5.0f, .dist_scale=1.0f, .sigma_scale=0.5f, .border_mode=SB_BORDER_CLAMP};
    CK(sb_validate_config(&ok_min) == SB_OK);
    printf("  %s\n", sb_error_msg(SB_ERR_RADIUS_MIN));

    /* ---- 3. 不做模糊时画面照旧（enable=0，逐像素相同） ---- */
    ShadowMap hard, soft, out;
    CK(sh_init(&hard, 16, 16) == 0);
    CK(sh_init(&soft, 16, 16) == 0);
    CK(sh_init(&out, 16, 16) == 0);
    ShLight light = {8.0f, 2.0f, 12.0f, 1.0f};
    ShSeg seg = {1.0f, 9.0f, 15.0f, 9.0f};
    CK(sh_render(&hard, &light, &seg, 1) == 0);
    SbConfig off = {.enable=0, .blur_radius_px=0, .blur_radius_min=0, .dist_begin=5.0f, .dist_scale=1.0f, .sigma_scale=0.5f, .border_mode=SB_BORDER_CLAMP};
    float dist[256], radius[256];
    CK(sb_distance_field(&hard, &seg, 1, &off, dist, radius, NULL) == SB_OK);
    CK(sb_composite(&hard, &soft, &off, &out) == SB_OK);
    int same = 1;
    for (int i = 0; i < 256; ++i) if (out.buf[i] != hard.buf[i]) same = 0;
    CK(same);
    sh_free(&hard); sh_free(&soft); sh_free(&out);

    /* ---- 4. 可分离模糊 == 朴素二维卷积（恒定半径，逐像素对照） ---- */
    enum { W = 12, H = 12, R = 2 };
    float src[W * H], rad[W * H], got[W * H], expect[W * H];
    for (int i = 0; i < W * H; ++i) { src[i] = (float)((i * 37) % 11) / 10.0f; rad[i] = (float)R; }
    float bigk[2 * R + 1];
    CK(sb_kernel(R, 0.5f, bigk) == 5);
    ShadowMap sm = {W, H, src}, dm = {W, H, got};
    SbConfig cfg = {.enable=1, .blur_radius_px=R, .blur_radius_min=R, .dist_begin=0.0f, .dist_scale=1.0f, .sigma_scale=0.5f, .border_mode=SB_BORDER_CLAMP};
    CK(sb_blur_separable(&sm, &dm, rad, R, bigk, &cfg) == SB_OK);
    ref_gaussian2d(src, expect, W, H, R, bigk, SB_BORDER_CLAMP);
    int maxerr_i = 0; float maxerr = 0.0f;
    for (int i = 0; i < W * H; ++i) {
        float e = fabsf(got[i] - expect[i]);
        if (e > maxerr) { maxerr = e; maxerr_i = i; }
    }
    printf("  separable vs naive-2d (clamp) max abs err = %.2e at %d\n", maxerr, maxerr_i);
    CK(maxerr < 1e-5f);
    CK(cfg.sample_count == (long)W * H * (2 * R + 1) * 2);  /* 每像素两 pass 各 5 tap */

    float gotz[W * H], expectz[W * H];
    ShadowMap dmz = {W, H, gotz};
    SbConfig cfgsz = {.enable=1, .blur_radius_px=R, .blur_radius_min=R, .dist_begin=0.0f, .dist_scale=1.0f, .sigma_scale=0.5f, .border_mode=SB_BORDER_ZERO};
    CK(sb_blur_separable(&sm, &dmz, rad, R, bigk, &cfgsz) == SB_OK);
    ref_gaussian2d(src, expectz, W, H, R, bigk, SB_BORDER_ZERO);
    float maxerrz = 0.0f;
    for (int i = 0; i < W * H; ++i) {
        float e = fabsf(gotz[i] - expectz[i]);
        if (e > maxerrz) maxerrz = e;
    }
    printf("  separable vs naive-2d (zero)  max abs err = %.2e\n", maxerrz);
    CK(maxerrz < 1e-5f);

    /* ---- 5. 手算一个具体像素：3 点核 r=1，clamp ---- */
    float s5[5] = {0.2f, 0.4f, 0.6f, 0.8f, 1.0f};
    float k3[3]; sb_kernel(1, 0.5f, k3);
    float hand_h = k3[0] * 0.4f + k3[1] * 0.6f + k3[2] * 0.8f;  /* 位置2的水平卷积 */
    float rad1[5] = {1, 1, 1, 1, 1}, out1[5];
    ShadowMap s5m = {5, 1, s5}, o5m = {5, 1, out1};
    SbConfig c1 = {.enable=1, .blur_radius_px=1, .blur_radius_min=1, .dist_begin=0, .dist_scale=1, .sigma_scale=0.5f, .border_mode=SB_BORDER_CLAMP};
    CK(sb_blur_separable(&s5m, &o5m, rad1, 1, k3, &c1) == SB_OK);
    CK(feq(out1[2], hand_h, 1e-6f));
    printf("  hand-conv pixel[2] = %.6f, pipeline = %.6f\n", hand_h, out1[2]);

    /* ---- 6. 端到端 + 软边宽度（小尺寸冒烟，完整两分辨率在 demo 跑） ---- */
    ShadowMap hard64, soft64, fin64;
    CK(sh_init(&hard64, 64, 64) == 0);
    CK(sh_init(&soft64, 64, 64) == 0);
    CK(sh_init(&fin64, 64, 64) == 0);
    ShLight l64 = {32.0f, 8.0f, 30.0f, 1.0f};
    ShSeg seg64 = {8.0f, 30.0f, 56.0f, 30.0f};
    CK(sh_render(&hard64, &l64, &seg64, 1) == 0);
    float *d64 = malloc(64 * 64 * sizeof(float));
    float *r64 = malloc(64 * 64 * sizeof(float));
    SbConfig cfg64 = {.enable=1, .blur_radius_px=4, .blur_radius_min=2, .dist_begin=1.0f, .dist_scale=0.8f, .sigma_scale=0.5f, .border_mode=SB_BORDER_CLAMP};
    int rmax = 0;
    CK(sb_distance_field(&hard64, &seg64, 1, &cfg64, d64, r64, &rmax) == SB_OK);
    CK(rmax >= 1 && rmax <= 4);
    float kern[9]; CK(sb_kernel(rmax, 0.5f, kern) == 2 * rmax + 1);
    CK(sb_blur_separable(&hard64, &soft64, r64, rmax, kern, &cfg64) == SB_OK);
    CK(sb_composite(&hard64, &soft64, &cfg64, &fin64) == SB_OK);
    SbSoftEdge se;
    CK(sb_measure_soft_edge(&hard64, &fin64, 32, &se) == SB_OK);
    CK(se.width_px > 0.0f);
    printf("  soft edge width(64) = %.3f px, boundary row=%d, 10%%y=%.2f 90%%y=%.2f taps=%ld\n",
           se.width_px, se.boundary_y, se.lo_y, se.hi_y, cfg64.sample_count);
    CK(sb_kernel_export("build/kernel.txt", rmax, 0.5f, SB_BORDER_CLAMP) == 2 * rmax + 1);

    free(d64); free(r64);
    sh_free(&hard64); sh_free(&soft64); sh_free(&fin64);
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
