#include "halftone.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int fails = 0, checks = 0;
#define CK(c) do { ++checks; if (!(c)) { ++fails; printf("FAIL %d %s\n", __LINE__, #c); } } while (0)

static int is_permutation(const unsigned char *m, int cells) {
    unsigned char *seen = (unsigned char*)calloc(cells, 1);
    int ok = 1;
    for (int i = 0; i < cells; ++i) {
        if (m[i] >= cells || seen[m[i]]) { ok = 0; break; }
        seen[m[i]] = 1;
    }
    free(seen);
    return ok;
}

int main(void) {
    /* 合法 n：2..16 的 2 的幂；矩阵是 0..n*n-1 的排列，且两种构造完全一致 */
    int good[] = {2, 4, 8, 16};
    for (unsigned k = 0; k < sizeof(good) / sizeof(good[0]); ++k) {
        HTMatrix ht;
        CK(ht_init(&ht, good[k]) == 0);
        int cells = ht.n * ht.n;
        CK(is_permutation(ht.m, cells));
        unsigned char *ref = (unsigned char*)malloc((size_t)cells);
        CK(ht_bayer_brute(ref, ht.n) == 0);
        CK(memcmp(ht.m, ref, (size_t)cells) == 0);
        CK(ht.cover[0] == 0);
        for (int q = 1; q < 256; ++q) CK(ht.cover[q] >= ht.cover[q - 1]);
        free(ref);
        ht_free(&ht);
    }
    /* 非法 n：必须拒绝并给出位置 */
    int bad[] = {0, 1, 3, 5, 6, 32, -4};
    for (unsigned k = 0; k < sizeof(bad) / sizeof(bad[0]); ++k) {
        HTMatrix ht;
        CK(ht_init(&ht, bad[k]) == -3);
        CK(ht_error() != NULL);
    }
    CK(ht_bayer_brute(NULL, 4) == -1);

    HTMatrix ht;
    CK(ht_init(&ht, 2) == 0); /* B2 = [[0,2],[3,1]] */
    CK(ht.m[0] == 0 && ht.m[1] == 2 && ht.m[2] == 3 && ht.m[3] == 1);

    const int W = 4, H = 4, N = W * H;
    unsigned char *rgb = (unsigned char*)malloc((size_t)N * 3);
    unsigned char *ref = (unsigned char*)malloc((size_t)N * 3);
    float *alpha = (float*)malloc((size_t)N * sizeof(float));

    /* 不启用：输出保持原样，alpha 可为 NULL */
    for (int i = 0; i < N * 3; ++i) rgb[i] = (unsigned char)(i * 7 + 3);
    HTFog off = {0, {10, 20, 30}};
    memcpy(ref, rgb, (size_t)N * 3);
    CK(ht_apply_fast(&ht, &off, rgb, NULL, W, H) == 0);
    CK(memcmp(rgb, ref, (size_t)N * 3) == 0);
    CK(ht_apply_brute(&ht, &off, rgb, alpha, W, H) == 0);
    CK(memcmp(rgb, ref, (size_t)N * 3) == 0);

    /* alpha 全 0：一个网点都不写 */
    for (int i = 0; i < N; ++i) alpha[i] = 0.0f;
    for (int i = 0; i < N * 3; ++i) rgb[i] = 200;
    CK(ht_apply_fast(&ht, &(HTFog){1, {0, 0, 0}}, rgb, alpha, W, H) == 0);
    for (int i = 0; i < N * 3; ++i) CK(rgb[i] == 200);

    /* alpha 全 1：每个 2x2 平铺写 3 格（rank 3 不写），共 4 个平铺 = 12；
     * 混色值手算：白底 + 黑雾，覆盖率 3/4 -> 255 - 255*3/4 = 64 */
    for (int i = 0; i < N; ++i) alpha[i] = 1.0f;
    for (int i = 0; i < N * 3; ++i) rgb[i] = 255;
    int wf = ht_apply_fast(&ht, &(HTFog){1, {0, 0, 0}}, rgb, alpha, W, H);
    CK(wf == 12);
    int dark = 0;
    for (int i = 0; i < N; ++i) if (rgb[i * 3] == 64) ++dark;
    CK(dark == 12);
    /* B2 = [[0,2],[3,1]]：rank 3 在 (x=0,y=1)，是每块唯一不写的格点 */
    CK(rgb[(1 * W + 0) * 3] == 255);

    /* 快速路径 vs 暴力路径：逐字节一致、网点覆盖数一致 */
    for (int i = 0; i < N; ++i) alpha[i] = (float)((i * 37 % 100) / 100.0);
    unsigned char *rgb2 = (unsigned char*)malloc((size_t)N * 3);
    for (int i = 0; i < N * 3; ++i) { rgb[i] = (unsigned char)(i * 13); rgb2[i] = rgb[i]; }
    HTFog fog = {1, {40, 90, 160}};
    int n1 = ht_apply_fast(&ht, &fog, rgb, alpha, W, H);
    int n2 = ht_apply_brute(&ht, &fog, rgb2, alpha, W, H);
    CK(n1 == n2);
    CK(memcmp(rgb, rgb2, (size_t)N * 3) == 0);

    /* 非整数倍尺寸（5x3，n=2）的平铺对照 */
    unsigned char r5[5 * 3 * 3], r5b[5 * 3 * 3];
    float a5[5 * 3];
    for (int i = 0; i < 5 * 3; ++i) { a5[i] = (i % 5) / 5.0f + 0.1f; r5[i*3]=r5[i*3+1]=r5[i*3+2]=(unsigned char)(i*11); }
    memcpy(r5b, r5, sizeof(r5));
    CK(ht_apply_fast(&ht, &fog, r5, a5, 5, 3) == ht_apply_brute(&ht, &fog, r5b, a5, 5, 3));
    CK(memcmp(r5, r5b, sizeof(r5)) == 0);

    /* 非法值必须拒绝，错误信息带位置，且输出不被破坏 */
    for (int i = 0; i < N * 3; ++i) rgb[i] = (unsigned char)i;
    memcpy(ref, rgb, (size_t)N * 3);
    for (int i = 0; i < N; ++i) alpha[i] = 0.5f;
    alpha[7] = 1.5f;
    CK(ht_apply_fast(&ht, &fog, rgb, alpha, W, H) == -2);
    CK(strstr(ht_error(), "alpha[7]") != NULL);
    CK(memcmp(rgb, ref, (size_t)N * 3) == 0);
    alpha[7] = -0.01f;
    CK(ht_apply_brute(&ht, &fog, rgb, alpha, W, H) == -2);
    CK(strstr(ht_error(), "alpha[7]") != NULL);
    alpha[7] = NAN;
    CK(ht_apply_fast(&ht, &fog, rgb, alpha, W, H) == -2);
    CK(strstr(ht_error(), "alpha[7]") != NULL);
    CK(memcmp(rgb, ref, (size_t)N * 3) == 0);
    CK(ht_apply_fast(&ht, &fog, NULL, alpha, W, H) == -1);
    CK(ht_apply_fast(&ht, &fog, rgb, alpha, 0, H) == -1);
    CK(ht_apply_fast(&ht, &fog, rgb, NULL, W, H) == -1); /* 启用但 alpha=NULL */

    /* 矩阵导出：文本含维度与全部秩，PGM 可读回且尺寸正确 */
    CK(ht_write_matrix_txt(&ht, "build/matrix.txt") == 0);
    CK(ht_write_matrix_pgm(&ht, "build/matrix.pgm") == 0);
    FILE *ft = fopen("build/matrix.txt", "r");
    CK(ft != NULL);
    if (ft) { char line[128]; CK(fgets(line, sizeof(line), ft) != NULL); CK(strstr(line, "2x2") != NULL); fclose(ft); }
    FILE *fp = fopen("build/matrix.pgm", "rb");
    CK(fp != NULL);
    if (fp) {
        char magic[8]; int pw, ph, pv;
        CK(fscanf(fp, "%7s %d %d %d", magic, &pw, &ph, &pv) == 4);
        CK(pw == 2 && ph == 2 && pv == 255);
        fgetc(fp); /* 吃掉头部最后一个空白符 */
        unsigned char px[4];
        CK(fread(px, 1, 4, fp) == 4);
        CK(px[0] == 0 && px[3] == 85); /* rank 0 -> 0, rank 1 -> 255/3=85 */
        fclose(fp);
    }

    free(rgb); free(rgb2); free(ref); free(alpha);
    ht_free(&ht);
    printf("%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
