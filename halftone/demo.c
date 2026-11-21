#include "halftone.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct { int w, h; unsigned char *rgb; } Img;

static void make_scene(Img *im, float *alpha, int size) {
    im->w = im->h = size;
    im->rgb = (unsigned char*)malloc((size_t)size * size * 3);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            int p = y * size + x;
            /* 底图：棋盘格，保证混色差异可见 */
            int cell = ((x / (size / 8 ? size / 8 : 1)) + (y / (size / 8 ? size / 8 : 1))) & 1;
            im->rgb[p*3+0] = (unsigned char)(cell ? 210 : 40);
            im->rgb[p*3+1] = (unsigned char)(cell ? 200 : 90);
            im->rgb[p*3+2] = (unsigned char)(cell ? 180 : 140);
            /* 雾浓度：远处（图像上部）浓、近处淡的层次场，带一点径向变化 */
            float vert = 1.0f - (float)y / (size - 1);
            float dx = (x - size * 0.5f) / size, dy = (y - size * 0.35f) / size;
            float rad = sqrtf(dx * dx + dy * dy);
            float a = 0.15f + 0.75f * vert + 0.10f * (0.5f - rad);
            if (a < 0.0f) a = 0.0f;
            if (a > 1.0f) a = 1.0f;
            alpha[p] = a;
        }
}

static int write_ppm(const Img *im, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fprintf(f, "P6\n%d %d\n255\n", im->w, im->h);
    fwrite(im->rgb, 1, (size_t)im->w * im->h * 3, f);
    fclose(f);
    return 0;
}

static double elapsed_ms(clock_t t0, clock_t t1) {
    return (double)(t1 - t0) * 1000.0 / CLOCKS_PER_SEC;
}

static void run_case(HTMatrix *ht, const HTFog *fog, int size, int fast_runs, int brute_runs) {
    int total = size * size;
    float *alpha = (float*)malloc((size_t)total * sizeof(float));
    Img base;
    make_scene(&base, alpha, size);

    Img fast_img, brute_img;
    fast_img.w = fast_img.h = size;
    brute_img.w = brute_img.h = size;
    fast_img.rgb = (unsigned char*)malloc((size_t)total * 3);
    brute_img.rgb = (unsigned char*)malloc((size_t)total * 3);

    /* 快速路径：重复运行取总耗时与平均耗时 */
    int cover_fast = 0;
    clock_t t0 = clock();
    for (int r = 0; r < fast_runs; ++r) {
        memcpy(fast_img.rgb, base.rgb, (size_t)total * 3);
        cover_fast = ht_apply_fast(ht, fog, fast_img.rgb, alpha, size, size);
        if (cover_fast < 0) { printf("fast error: %s\n", ht_error()); return; }
    }
    clock_t t1 = clock();

    /* 暴力路径：遍历整个矩阵的对照实现 */
    int cover_brute = 0;
    clock_t t2 = clock();
    for (int r = 0; r < brute_runs; ++r) {
        memcpy(brute_img.rgb, base.rgb, (size_t)total * 3);
        cover_brute = ht_apply_brute(ht, fog, brute_img.rgb, alpha, size, size);
        if (cover_brute < 0) { printf("brute error: %s\n", ht_error()); return; }
    }
    clock_t t3 = clock();

    /* 逐像素对照：混色误差（每通道绝对差：最大、总和、平均） */
    long sum_abs = 0, max_abs = 0, diff_pixels = 0;
    for (int i = 0; i < total * 3; ++i) {
        long d = (long)fast_img.rgb[i] - (long)brute_img.rgb[i];
        if (d < 0) d = -d;
        sum_abs += d;
        if (d > max_abs) max_abs = d;
        if (d != 0) ++diff_pixels;
    }

    char ppm_path[64];
    snprintf(ppm_path, sizeof(ppm_path), "demo-%d.ppm", size);
    write_ppm(&fast_img, ppm_path);

    printf("size=%d\n", size);
    printf("  pixels            : %d\n", total);
    printf("  dot coverage      : fast=%d brute=%d (rate %.4f)\n",
           cover_fast, cover_brute, (double)cover_fast / total);
    printf("  blend error       : max=%ld sum=%ld mean=%.6f diff_pixels=%ld\n",
           max_abs, sum_abs, (double)sum_abs / (total * 3), diff_pixels);
    printf("  time fast (x%d)    : %.3f ms total, %.4f ms/run\n",
           fast_runs, elapsed_ms(t0, t1), elapsed_ms(t0, t1) / fast_runs);
    printf("  time brute (x%d)   : %.3f ms total, %.4f ms/run\n",
           brute_runs, elapsed_ms(t2, t3), elapsed_ms(t2, t3) / brute_runs);
    printf("  speedup           : %.2fx\n",
           (elapsed_ms(t2, t3) / brute_runs) / (elapsed_ms(t0, t1) / fast_runs));

    free(alpha); free(base.rgb); free(fast_img.rgb); free(brute_img.rgb);
}

int main(void) {
    HTMatrix ht;
    int rc = ht_init(&ht, 8); /* 8x8 Bayer 网点矩阵 */
    if (rc != 0) { printf("ht_init failed: %s\n", ht_error()); return 1; }

    if (ht_write_matrix_txt(&ht, "demo-matrix.txt") != 0 ||
        ht_write_matrix_pgm(&ht, "demo-matrix.pgm") != 0) {
        printf("matrix export failed: %s\n", ht_error());
        return 1;
    }
    printf("matrix: %dx%d bayer exported to demo-matrix.txt / demo-matrix.pgm\n", ht.n, ht.n);
    printf("cover table: cover[0]=%d cover[128]=%d cover[255]=%d (cells=%d)\n\n",
           ht.cover[0], ht.cover[128], ht.cover[255], ht.n * ht.n);

    HTFog fog = {1, {180, 195, 215}}; /* 冷灰蓝雾色 */
    run_case(&ht, &fog, 64, 50, 5);
    printf("\n");
    run_case(&ht, &fog, 512, 10, 2);

    /* 关闭路径自检：输出必须与输入一致 */
    unsigned char px[6] = {1, 2, 3, 4, 5, 6}, saved[6];
    memcpy(saved, px, sizeof(px));
    HTFog off = {0, {0, 0, 0}};
    if (ht_apply_fast(&ht, &off, px, NULL, 2, 1) != 0 || memcmp(px, saved, sizeof(px)) != 0) {
        printf("disabled-path check FAILED\n");
        return 1;
    }
    printf("\ndisabled path: output unchanged, OK\n");

    /* 非法输入自检：拒绝并说明位置 */
    float bad_a[4] = {0.5f, 0.5f, 1.2f, 0.5f};
    unsigned char bad_px[12] = {0};
    rc = ht_apply_fast(&ht, &fog, bad_px, bad_a, 2, 2);
    printf("invalid input rc=%d error: %s\n", rc, ht_error());
    if (rc != -2) return 1;

    ht_free(&ht);
    return 0;
}
