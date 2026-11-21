#include "halftone.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 最近一次错误：单线程实验模块，静态缓冲足够；记录函数与非法值位置。 */
static char g_err[160];
static void seterr(const char *msg) {
    strncpy(g_err, msg, sizeof(g_err) - 1);
    g_err[sizeof(g_err) - 1] = '\0';
}
const char *ht_error(void) { return g_err[0] ? g_err : NULL; }

static int is_pow2(int n) { return n > 0 && (n & (n - 1)) == 0; }

/* 递推构造 Bayer 有序抖动矩阵：
 * B_{2n} = [[4 B_n +0, 4 B_n +2],
 *           [4 B_n +3, 4 B_n +1]]，秩覆盖 0..n*n-1。 */
static int bayer_recursive(unsigned char *m, int n) {
    if (n == 1) { m[0] = 0; return 0; }
    int h = n / 2;
    unsigned char *sub = (unsigned char*)malloc((size_t)h * (size_t)h);
    if (!sub) return -1;
    if (bayer_recursive(sub, h) != 0) { free(sub); return -1; }
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < h; ++x) {
            unsigned char v = sub[(size_t)y * h + x];
            m[(size_t)y * n + x]             = (unsigned char)(4 * v + 0);
            m[(size_t)y * n + (x + h)]       = (unsigned char)(4 * v + 2);
            m[(size_t)(y + h) * n + x]       = (unsigned char)(4 * v + 3);
            m[(size_t)(y + h) * n + (x + h)] = (unsigned char)(4 * v + 1);
        }
    free(sub);
    return 0;
}

int ht_bayer_brute(unsigned char *out, int n) {
    g_err[0] = '\0';
    if (!out) { seterr("ht_bayer_brute out=NULL"); return -1; }
    if (!is_pow2(n) || n < 2 || n > 16) {
        snprintf(g_err, sizeof(g_err), "ht_bayer_brute n=%d not a power of two in [2,16]", n);
        return -3;
    }
    /* 位级定义：每一层 (x_b,y_b) 查 2x2 基矩阵 [[0,2],[3,1]] 得 2 位象限码；
     * 递推式 rank = 4*子矩阵秩 + 象限码，故顶层（高位比特）象限码占低位、
     * 底层占高位。与 bayer_recursive 的构造等价但实现路径完全独立。 */
    static const unsigned char quad[2][2] = {{0, 2}, {3, 1}};
    int bits = 0;
    for (int t = n; t > 1; t >>= 1) ++bits;
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            unsigned char rank = 0;
            for (int b = 0; b < bits; ++b) {
                int xb = (x >> b) & 1;
                int yb = (y >> b) & 1;
                rank = (unsigned char)(rank | quad[yb][xb] << (2 * (bits - 1 - b)));
            }
            out[(size_t)y * n + x] = rank;
        }
    return 0;
}

static void build_cover(HTMatrix *ht) {
    int cells = ht->n * ht->n;
    for (int q = 0; q < 256; ++q) {
        int cnt = 0;
        /* 量化等级 q 对应阈值 q*cells/256，秩严格小于阈值的格点被写。 */
        int limit = (int)(((long)q * cells) / 256);
        for (int i = 0; i < cells; ++i)
            if (ht->m[i] < limit) ++cnt;
        ht->cover[q] = cnt;
    }
}

int ht_init(HTMatrix *ht, int n) {
    g_err[0] = '\0';
    if (!ht) { seterr("ht_init ht=NULL"); return -1; }
    memset(ht, 0, sizeof(*ht));
    if (!is_pow2(n) || n < 2 || n > 16) {
        snprintf(g_err, sizeof(g_err), "ht_init n=%d not a power of two in [2,16]", n);
        return -3;
    }
    ht->n = n;
    ht->m = (unsigned char*)malloc((size_t)n * (size_t)n);
    ht->cover = (int*)malloc(256 * sizeof(int));
    if (!ht->m || !ht->cover) {
        free(ht->m); free(ht->cover);
        seterr("ht_init allocation failed");
        return -1;
    }
    if (bayer_recursive(ht->m, n) != 0) {
        ht_free(ht);
        seterr("ht_init matrix build failed");
        return -1;
    }
    build_cover(ht);
    return 0;
}

void ht_free(HTMatrix *ht) {
    if (!ht) return;
    free(ht->m);
    free(ht->cover);
    ht->m = NULL;
    ht->cover = NULL;
    ht->n = 0;
}

/* alpha -> 量化等级 0..255；非法（NaN/越界）由调用方先拦截。 */
static int quantize(float a) {
    int q = (int)lrintf(a * 255.0f);
    if (q < 0) q = 0;
    if (q > 255) q = 255;
    return q;
}

/* 第二段：按覆盖率 c（每 cells 格点中写 cnt 个）做整数混色。 */
static inline void blend(unsigned char *px, const unsigned char fog[3], int cnt, int cells) {
    for (int c = 0; c < 3; ++c) {
        int v = px[c] + ((int)fog[c] - (int)px[c]) * cnt / cells;
        px[c] = (unsigned char)v;
    }
}

static int validate_common(HTMatrix *ht, const HTFog *fog, unsigned char *rgb,
                           const float *alpha, int w, int h) {
    if (!ht || !ht->m || !ht->cover) { seterr("ht_apply ht=NULL or uninitialized"); return -1; }
    if (!fog) { seterr("ht_apply fog=NULL"); return -1; }
    if (!rgb) { seterr("ht_apply rgb=NULL"); return -1; }
    if (w <= 0 || h <= 0) {
        snprintf(g_err, sizeof(g_err), "ht_apply bad size w=%d h=%d", w, h);
        return -1;
    }
    if (fog->enabled && !alpha) { seterr("ht_apply alpha=NULL while enabled"); return -1; }
    return 0;
}

static int check_alpha(const float *alpha, int total) {
    for (int i = 0; i < total; ++i) {
        float a = alpha[i];
        if (!(a >= 0.0f && a <= 1.0f)) { /* NaN 同时被捕获 */
            snprintf(g_err, sizeof(g_err),
                     "ht_apply alpha[%d]=%g out of range [0,1]", i, (double)a);
            return -2;
        }
    }
    return 0;
}

int ht_apply_fast(HTMatrix *ht, const HTFog *fog, unsigned char *rgb,
                  const float *alpha, int w, int h) {
    g_err[0] = '\0';
    int rc = validate_common(ht, fog, rgb, alpha, w, h);
    if (rc != 0) return rc;
    int total = w * h, cells = ht->n * ht->n, written = 0;

    if (!fog->enabled) return 0; /* 关闭：输出原样，不读取 alpha */

    rc = check_alpha(alpha, total);
    if (rc != 0) return rc;

    /* 第一段（网点判定）与第二段（覆盖率混色）分离：
     * 先由 cover 查找表得到该等级的格点数，再由矩阵秩决定本像素在不在网点上。 */
    for (int y = 0; y < h; ++y) {
        int my = y % ht->n;
        for (int x = 0; x < w; ++x) {
            int p = y * w + x;
            int q = quantize(alpha[p]);
            int limit = (int)(((long)q * cells) / 256);
            unsigned char rank = ht->m[(size_t)my * ht->n + (x % ht->n)];
            if (rank < limit) {
                blend(rgb + (size_t)p * 3, fog->rgb, ht->cover[q], cells);
                ++written;
            }
        }
    }
    return written;
}

int ht_apply_brute(HTMatrix *ht, const HTFog *fog, unsigned char *rgb,
                   const float *alpha, int w, int h) {
    g_err[0] = '\0';
    int rc = validate_common(ht, fog, rgb, alpha, w, h);
    if (rc != 0) return rc;
    int total = w * h, cells = ht->n * ht->n, written = 0;

    if (!fog->enabled) return 0;

    rc = check_alpha(alpha, total);
    if (rc != 0) return rc;

    /* 暴力：不使用 cover 表，每个像素遍历整个 n*n 矩阵数出格点（覆盖率），
     * 再单独判定本像素写不写。 */
    for (int y = 0; y < h; ++y) {
        int my = y % ht->n;
        for (int x = 0; x < w; ++x) {
            int p = y * w + x;
            int q = quantize(alpha[p]);
            int limit = (int)(((long)q * cells) / 256);
            int cnt = 0;
            for (int i = 0; i < cells; ++i)
                if (ht->m[i] < limit) ++cnt;
            unsigned char rank = ht->m[(size_t)my * ht->n + (x % ht->n)];
            if (rank < limit) {
                blend(rgb + (size_t)p * 3, fog->rgb, cnt, cells);
                ++written;
            }
        }
    }
    return written;
}

int ht_write_matrix_txt(const HTMatrix *ht, const char *path) {
    if (!ht || !ht->m || !path) { seterr("ht_write_matrix_txt bad argument"); return -1; }
    FILE *f = fopen(path, "w");
    if (!f) { snprintf(g_err, sizeof(g_err), "ht_write_matrix_txt cannot open %s", path); return -4; }
    fprintf(f, "# bayer %dx%d, ranks 0..%d\n", ht->n, ht->n, ht->n * ht->n - 1);
    for (int y = 0; y < ht->n; ++y) {
        for (int x = 0; x < ht->n; ++x)
            fprintf(f, x ? " %3d" : "%3d", (int)ht->m[(size_t)y * ht->n + x]);
        fputc('\n', f);
    }
    fclose(f);
    return 0;
}

int ht_write_matrix_pgm(const HTMatrix *ht, const char *path) {
    if (!ht || !ht->m || !path) { seterr("ht_write_matrix_pgm bad argument"); return -1; }
    FILE *f = fopen(path, "wb");
    if (!f) { snprintf(g_err, sizeof(g_err), "ht_write_matrix_pgm cannot open %s", path); return -4; }
    int cells = ht->n * ht->n;
    fprintf(f, "P5\n%d %d\n255\n", ht->n, ht->n);
    for (int i = 0; i < cells; ++i) {
        unsigned char v = (unsigned char)((int)ht->m[i] * 255 / (cells - 1));
        fwrite(&v, 1, 1, f);
    }
    fclose(f);
    return 0;
}
