#define _POSIX_C_SOURCE 199309L
#include "mip.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* 基准:两段流水线在 1 千 / 1 万采样规模下的误差与耗时。
 *
 * 参考实现与库完全独立:
 *   段1 参考 = 对缩略图像素覆盖区域做精确面积加权平均(浮点,按覆盖比例叠加)
 *   段2 参考 = 相邻层公共区域逐通道绝对差
 * 误差为库输出与参考输出的逐通道绝对差,给出均值与最大值。
 * 层间差异单独统计,并与段2参考逐项核对必须完全一致。 */

#define REPS 5

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

/* 参考段1:覆盖 [x*lw/tw,(x+1)*lw/tw) x [y*lh/th,(y+1)*lh/th) 的面积加权平均。
 * 纹素与覆盖矩形按重叠面积加权,不做点采样,得到该区域真实的均值。 */
static void ref_thumb(const MipChain *c, int level, int tw, int th,
                      unsigned char *out) {
    int lw = c->w[level], lh = c->h[level];
    const unsigned char *src = c->pix[level];
    double xscale = (double)lw / (double)tw;
    double yscale = (double)lh / (double)th;
    for (int y = 0; y < th; ++y)
        for (int x = 0; x < tw; ++x) {
            double x0 = x * xscale, x1 = (x + 1) * xscale;
            double y0 = y * yscale, y1 = (y + 1) * yscale;
            int ix0 = (int)floor(x0), ix1 = (int)ceil(x1);
            int iy0 = (int)floor(y0), iy1 = (int)ceil(y1);
            if (ix0 < 0) ix0 = 0;
            if (iy0 < 0) iy0 = 0;
            if (ix1 > lw) ix1 = lw;
            if (iy1 > lh) iy1 = lh;
            double area = (x1 - x0) * (y1 - y0);
            if (area <= 0.0) area = 1.0;
            for (int ch = 0; ch < 3; ++ch) {
                double acc = 0.0;
                for (int sy = iy0; sy < iy1; ++sy) {
                    double oy = (y1 < sy + 1 ? y1 : sy + 1)
                              - (y0 > sy ? y0 : sy);
                    if (oy <= 0.0) continue;
                    for (int sx = ix0; sx < ix1; ++sx) {
                        double ox = (x1 < sx + 1 ? x1 : sx + 1)
                                  - (x0 > sx ? x0 : sx);
                        if (ox <= 0.0) continue;
                        acc += (double)src[((size_t)sy * lw + sx) * 3 + ch]
                             * ox * oy;
                    }
                }
                int v = (int)(acc / area + 0.5);
                if (v < 0) v = 0;
                if (v > 255) v = 255;
                out[((size_t)y * tw + x) * 3 + ch] = (unsigned char)v;
            }
        }
}

/* 参考段2:相邻层公共区域逐通道绝对差 */
static void ref_inter_layer(const MipThumb *a, const MipThumb *b,
                            unsigned long long *sum, unsigned long long *px) {
    int cw = a->w < b->w ? a->w : b->w;
    int chh = a->h < b->h ? a->h : b->h;
    unsigned long long s = 0, n = 0;
    for (int y = 0; y < chh; ++y)
        for (int x = 0; x < cw; ++x)
            for (int k = 0; k < 3; ++k) {
                int av = a->pixels[((size_t)y * a->w + x) * 3 + k];
                int bv = b->pixels[((size_t)y * b->w + x) * 3 + k];
                s += (unsigned long long)(av > bv ? av - bv : bv - av);
                ++n;
            }
    *sum = s; *px = n;
}

typedef struct {
    unsigned long long reads;
    double err_mean, err_max;
    double inter_mean;
    double ms_thumb, ms_sheet;
    int sheet_w, sheet_h;
    int cols;
    unsigned long long inter_px;
} Run;

static Run run_case(const MipChain *c, int side, int samples,
                    int detail) {
    Run r;
    memset(&r, 0, sizeof r);
    int n = c->levels;

    MipThumb *th = (MipThumb*)calloc((size_t)n, sizeof(MipThumb));
    if (!th) exit(1);

    /* 段1:取最小耗时,保留最后一次输出用于误差统计 */
    double best = 1e30;
    for (int rep = 0; rep < REPS; ++rep) {
        for (int i = 0; i < n; ++i) free(th[i].pixels);
        double t0 = now_ms();
        for (int i = 0; i < n; ++i) {
            MipThumbSpec spec = { i, side, side, samples };
            int rc = mip_thumb_level(c, &spec, &th[i], NULL);
            if (rc != MIP_OK) { fprintf(stderr, "thumb rc=%d\n", rc); exit(1); }
        }
        double t1 = now_ms();
        if (t1 - t0 < best) best = t1 - t0;
    }
    r.ms_thumb = best;

    for (int i = 0; i < n; ++i) {
        r.reads += th[i].sample_reads;
    }

    /* 误差:逐层与精确面积加权参考比较 */
    unsigned long long esum = 0, epx = 0;
    int emax = 0;
    size_t np = (size_t)side * (size_t)side * 3;
    unsigned char *ref = (unsigned char*)malloc(np);
    if (!ref) exit(1);
    for (int i = 0; i < n; ++i) {
        ref_thumb(c, i, side, side, ref);
        unsigned long long lsum = 0;
        int lmax = 0;
        for (size_t k = 0; k < np; ++k) {
            int d = th[i].pixels[k] - ref[k];
            if (d < 0) d = -d;
            lsum += (unsigned long long)d;
            if (d > lmax) lmax = d;
            ++epx;
        }
        if (detail)
            printf("   层 %d 尺寸 %dx%d: 平均误差 %.4f 最大误差 %d\n",
                   i, c->w[i], c->h[i], (double)lsum / (double)np, lmax);
        esum += lsum;
        if (lmax > emax) emax = lmax;
    }
    free(ref);
    r.err_mean = epx ? (double)esum / (double)epx : 0.0;
    r.err_max = emax;

    /* 段2:拼接,取最小耗时 */
    MipSheet sh;
    memset(&sh, 0, sizeof sh);
    best = 1e30;
    for (int rep = 0; rep < REPS; ++rep) {
        if (sh.pixels) {
            free(sh.pixels); free(sh.x); free(sh.y); free(sh.tw); free(sh.th);
            sh.pixels = NULL;
        }
        double t0 = now_ms();
        int rc = mip_sheet_assemble(c, th, n, NULL, &sh, NULL);
        double t1 = now_ms();
        if (rc != MIP_OK) { fprintf(stderr, "sheet rc=%d\n", rc); exit(1); }
        if (t1 - t0 < best) best = t1 - t0;
    }
    r.ms_sheet = best;
    r.sheet_w = sh.w; r.sheet_h = sh.h; r.cols = sh.cols;
    r.inter_mean = sh.inter_layer_mean;
    r.inter_px = sh.inter_layer_px;

    if (detail) {
        printf("   预览图 %dx%d (cols=%d), 层间差异累计 %llu / %llu 通道\n",
               sh.w, sh.h, sh.cols, sh.inter_layer_bytes, sh.inter_layer_px);
        for (int i = 1; i < n; ++i) {
            unsigned long long s = 0, p = 0;
            ref_inter_layer(&th[i-1], &th[i], &s, &p);
            printf("   层间 %d->%d: 绝对差均值 %.4f 参与通道 %llu\n",
                   i - 1, i, p ? (double)s / (double)p : 0.0, p);
        }
    }

    /* 层间差异参考核对:必须与独立重算完全一致 */
    unsigned long long rs = 0, rp = 0;
    for (int i = 1; i < n; ++i) {
        unsigned long long s = 0, p = 0;
        ref_inter_layer(&th[i-1], &th[i], &s, &p);
        rs += s; rp += p;
    }
    double ref_inter = rp ? (double)rs / (double)rp : 0.0;
    if (rp != sh.inter_layer_px || rs != sh.inter_layer_bytes ||
        fabs(ref_inter - r.inter_mean) > 1e-12) {
        fprintf(stderr, "层间差异与参考不符: 库 %.9f (%llu/%llu) vs 参考 %.9f\n",
                r.inter_mean, sh.inter_layer_bytes, sh.inter_layer_px, ref_inter);
        exit(1);
    }

    free(sh.pixels); free(sh.x); free(sh.y); free(sh.tw); free(sh.th);
    for (int i = 0; i < n; ++i) free(th[i].pixels);
    free(th);
    return r;
}

/* 默认路径核对:整层直取必须与层像素逐字节一致 */
static void check_default(const MipChain *c) {
    unsigned long long total = 0;
    for (int l = 0; l < c->levels; ++l) {
        MipThumbSpec spec = MIP_THUMB_SPEC_DEFAULT(l, c);
        MipThumb t;
        if (mip_thumb_level(c, &spec, &t, NULL) != MIP_OK) exit(1);
        size_t np = (size_t)t.w * t.h * 3;
        if (memcmp(t.pixels, c->pix[l], np) != 0) {
            fprintf(stderr, "默认路径与原层不一致: level %d\n", l);
            exit(1);
        }
        if (t.samples != 1 || t.sample_reads != (unsigned long long)t.w * t.h) {
            fprintf(stderr, "默认路径统计不符: level %d\n", l);
            exit(1);
        }
        total += t.sample_reads;
        free(t.pixels);
    }
    printf("默认路径核对: 各层整层直取逐字节一致, 采样 %llu 次\n", total);

    /* 默认规则拼起来也要与逐层原像素一致 */
    MipThumb *th = (MipThumb*)calloc((size_t)c->levels, sizeof(MipThumb));
    for (int l = 0; l < c->levels; ++l) {
        MipThumbSpec spec = MIP_THUMB_SPEC_DEFAULT(l, c);
        mip_thumb_level(c, &spec, &th[l], NULL);
    }
    MipSheet sh;
    MipSheetSpec ss = MIP_SHEET_SPEC_DEFAULT;
    if (mip_sheet_assemble(c, th, c->levels, &ss, &sh, NULL) != MIP_OK) exit(1);
    int ok = 1;
    for (int l = 0; l < c->levels; ++l)
        for (int y = 0; y < c->h[l]; ++y)
            for (int x = 0; x < c->w[l]; ++x)
                for (int ch = 0; ch < 3; ++ch)
                    if (sh.pixels[(((size_t)(sh.y[l] + y) * sh.w) + sh.x[l] + x) * 3 + ch]
                        != c->pix[l][((size_t)y * c->w[l] + x) * 3 + ch])
                        ok = 0;
    if (!ok) { fprintf(stderr, "默认预览图与逐层原像素不一致\n"); exit(1); }
    printf("默认预览图核对: 每层区域与 c->pix[level] 逐字节一致, 尺寸 %dx%d\n",
           sh.w, sh.h);
    free(sh.pixels); free(sh.x); free(sh.y); free(sh.tw); free(sh.th);
    for (int l = 0; l < c->levels; ++l) free(th[l].pixels);
    free(th);
}

int main(void) {
    enum { W = 256, H = 256 };
    unsigned char *src = (unsigned char*)malloc((size_t)W * H * 3);
    if (!src) return 1;
    /* 测试图:两通道平滑渐变(宏观结构) + 一通道宽带伪随机噪点(高频细节)。
     * 宽带噪声不含单一周期,点采样的误差随每轴采样点数稳定下降,不会像棋盘格
     * 那样与采样网格共振造成非单调。 */
    unsigned int hstate = 0x12345678u;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            unsigned char *p = src + ((size_t)y * W + x) * 3;
            hstate ^= hstate << 13; hstate ^= hstate >> 17; hstate ^= hstate << 5;
            p[0] = (unsigned char)(x * 255 / (W - 1));
            p[1] = (unsigned char)(y * 255 / (H - 1));
            p[2] = (unsigned char)(hstate & 0xff);
        }
    MipChain c;
    if (mip_build(&c, src, W, H) != 0) return 1;

    printf("源图 %dx%d,层链 %d 层:\n", W, H, c.levels);
    for (int l = 0; l < c.levels; ++l)
        printf("  层 %d: %dx%d\n", l, c.w[l], c.h[l]);
    printf("参与层号: 0..%d (全部 %d 层)\n\n", c.levels - 1, c.levels);
    check_default(&c);
    printf("\n");

    /* 1k / 10k = 每层约 1 千 / 1 万次纹素读取的采样预算。
     * 同一预算下扫每轴采样点数 1/2/4,缩略图边长由 samples = ceil(sqrt(budget/(n*s^2)))
     * 反推,因此 1k 时缩略图渐小、10k 时可保持更大尺寸。 */
    long long budgets[2] = { 1000, 10000 };
        int sample_opts[3] = { 1, 2, 4 };
    printf("== 汇总(每层约 1k/10k 次读取;误差对精确面积加权参考;耗时取 %d 次最小值) ==\n", REPS);
    printf("%-5s %-8s %-8s %-9s %-11s %-12s %-11s %-11s %-12s %-9s %-9s\n",
           "规模", "缩略图", "采样/轴", "预览尺寸", "采样次数", "平均误差",
           "最大误差", "层间差异", "参与通道", "段1ms", "段2ms");
    double emean[2][3], emax[2][3];
    for (int b = 0; b < 2; ++b)
        for (int s = 0; s < 3; ++s) {
            int ns = sample_opts[s];
            long long per = budgets[b] / c.levels;
            int side = (int)sqrt((double)per / (double)(ns * ns));
            if (side < 1) side = 1;
            Run r = run_case(&c, side, ns, 0);
            char thumb[32], sheet[32], budget[32];
            snprintf(thumb, sizeof thumb, "%dx%d", side, side);
            snprintf(sheet, sizeof sheet, "%dx%d", r.sheet_w, r.sheet_h);
            snprintf(budget, sizeof budget, "%lld", budgets[b]);
            printf("%-5s %-8s %-8d %-9s %-11llu %-12.4f %-11.0f %-11.4f %-12llu %-9.3f %-9.3f\n",
                   budget, thumb, ns, sheet, r.reads,
                   r.err_mean, r.err_max, r.inter_mean,
                   r.inter_px, r.ms_thumb, r.ms_sheet);
            emean[b][s] = r.err_mean;
            emax[b][s] = r.err_max;
        }

    /* 明细:两档各取采样/轴=2,列出逐层误差与相邻层差异 */
    for (int b = 0; b < 2; ++b) {
        int side = (int)sqrt((double)(budgets[b] / c.levels) / 4.0);
        if (side < 1) side = 1;
        printf("\n== 明细: 预算 %lld, 缩略图 %dx%d, 采样/轴 2 ==\n",
               budgets[b], side, side);
        run_case(&c, side, 2, 1);
    }

    int monotone = emean[0][0] > emean[0][1] && emean[0][1] > emean[0][2]
                && emean[1][0] > emean[1][1] && emean[1][1] > emean[1][2];
    printf("\n结论: 同预算下采样/轴 1->2->4,平均误差单调下降: %s (1k: %.2f->%.2f->%.2f, "
           "10k: %.2f->%.2f->%.2f);\n"
           "      最大误差 1k: %.0f->%.0f->%.0f, 10k: %.0f->%.0f->%.0f;\n"
           "      层间差异逐项与独立参考完全一致;默认路径与逐层原像素逐字节一致。\n",
           monotone ? "是" : "否",
           emean[0][0], emean[0][1], emean[0][2],
           emean[1][0], emean[1][1], emean[1][2],
           emax[0][0], emax[0][1], emax[0][2],
           emax[1][0], emax[1][1], emax[1][2]);
    if (!monotone) { fprintf(stderr, "误差未随采样密度单调下降\n"); return 1; }

    mip_free(&c);
    free(src);
    return 0;
}
