#include "sharpen.h"
#include <math.h>
#include <stdlib.h>

MipSharpenParams mip_sharpen_default(void) {
    MipSharpenParams p = MIP_SHARPEN_DEFAULT_INIT;
    return p;
}

static int params_ok(const MipSharpenParams *p) {
    if (!p) return 0;
    if (!isfinite(p->strength) || !isfinite(p->grad_lo) || !isfinite(p->grad_hi)) return 0;
    if (p->strength < 0.0 || p->grad_lo < 0.0) return 0;
    if (p->grad_hi <= p->grad_lo) return 0;
    return 1;
}

static double lum_px(const unsigned char *src, size_t i) {
    return 0.299 * src[i * 3] + 0.587 * src[i * 3 + 1] + 0.114 * src[i * 3 + 2];
}

/* 第一段: 按梯度算锐化量 */
int mip_sharpen_amount(const unsigned char *src, int w, int h,
                       const MipSharpenParams *p, float *amount_out) {
    if (!src || !amount_out || w <= 0 || h <= 0) return -1;
    if (!params_ok(p)) return -3;
    for (int y = 0; y < h; ++y) {
        int ym = y > 0 ? y - 1 : 0, yp = y < h - 1 ? y + 1 : h - 1;
        for (int x = 0; x < w; ++x) {
            int xm = x > 0 ? x - 1 : 0, xp = x < w - 1 ? x + 1 : w - 1;
            double gx = lum_px(src, (size_t)y * w + xp) - lum_px(src, (size_t)y * w + xm);
            double gy = lum_px(src, (size_t)yp * w + x) - lum_px(src, (size_t)ym * w + x);
            double g = hypot(gx, gy);
            double t = (g - p->grad_lo) / (p->grad_hi - p->grad_lo);
            if (t < 0.0) t = 0.0;
            if (t > 1.0) t = 1.0;
            amount_out[(size_t)y * w + x] = (float)(p->strength * t);
        }
    }
    return 0;
}

/* 第二段: 按量做无振铃增强(反锐化掩模 + 局部 min/max 钳制) */
int mip_sharpen_apply(const unsigned char *src, const float *amount, int w, int h,
                      unsigned char *dst, MipSharpenStats *stats) {
    if (!src || !amount || !dst || w <= 0 || h <= 0) return -1;
    MipSharpenStats s = { 0.0, 0.0, 0.0, 0, 0 };
    for (int y = 0; y < h; ++y) {
        int ym = y > 0 ? y - 1 : 0, yp = y < h - 1 ? y + 1 : h - 1;
        for (int x = 0; x < w; ++x) {
            int xm = x > 0 ? x - 1 : 0, xp = x < w - 1 ? x + 1 : w - 1;
            double sum[3] = { 0.0, 0.0, 0.0 };
            double mn[3] = { 255.0, 255.0, 255.0 }, mx[3] = { 0.0, 0.0, 0.0 };
            for (int dy = -1; dy <= 1; ++dy) {
                int yy = dy < 0 ? ym : (dy > 0 ? yp : y);
                for (int dx = -1; dx <= 1; ++dx) {
                    int xx = dx < 0 ? xm : (dx > 0 ? xp : x);
                    const unsigned char *q = src + ((size_t)yy * w + xx) * 3;
                    for (int ch = 0; ch < 3; ++ch) {
                        double v = q[ch];
                        sum[ch] += v;
                        if (v < mn[ch]) mn[ch] = v;
                        if (v > mx[ch]) mx[ch] = v;
                    }
                }
            }
            size_t i = (size_t)y * w + x;
            double a = amount[i];
            if (a > 0.0) ++s.edge_pixels;
            s.strength_mean += a;
            if (a > s.strength_max) s.strength_max = a;
            int ring = 0;
            for (int ch = 0; ch < 3; ++ch) {
                double sv = src[i * 3 + ch];
                double raw = sv + a * (sv - sum[ch] / 9.0);
                double ex = raw > mx[ch] ? raw - mx[ch] : (raw < mn[ch] ? mn[ch] - raw : 0.0);
                s.overshoot += ex;
                if (ex > 1e-9) ring = 1;
                double v = raw < mn[ch] ? mn[ch] : (raw > mx[ch] ? mx[ch] : raw);
                if (v < 0.0) v = 0.0;
                if (v > 255.0) v = 255.0;
                dst[i * 3 + ch] = (unsigned char)(v + 0.5);
            }
            if (ring) ++s.ringing_pixels;
        }
    }
    s.strength_mean /= (double)((size_t)w * (size_t)h);
    if (stats) *stats = s;
    return 0;
}

int mip_sharpen(const unsigned char *src, int w, int h,
                const MipSharpenParams *p, unsigned char *dst, MipSharpenStats *stats) {
    if (!src || !dst || w <= 0 || h <= 0) return -1;
    if (!params_ok(p)) return -3;
    float *amount = (float *)malloc(sizeof(float) * (size_t)w * (size_t)h);
    if (!amount) return -1;
    int rc = mip_sharpen_amount(src, w, h, p, amount);
    if (rc == 0) rc = mip_sharpen_apply(src, amount, w, h, dst, stats);
    free(amount);
    return rc;
}

int mip_sharpen_batch(int n, const unsigned char *const *srcs,
                      const int *ws, const int *hs,
                      const MipSharpenParams *p, unsigned char *const *dsts,
                      int *bad_index) {
    if (bad_index) *bad_index = -1;
    if (n <= 0 || !srcs || !ws || !hs || !dsts) return -1;
    if (!params_ok(p)) return -3;
    for (int i = 0; i < n; ++i) {
        if (!srcs[i] || !dsts[i] || ws[i] <= 0 || hs[i] <= 0) {
            if (bad_index) *bad_index = i;
            return -2;
        }
    }
    for (int i = 0; i < n; ++i) {
        if (mip_sharpen(srcs[i], ws[i], hs[i], p, dsts[i], NULL) != 0) {
            if (bad_index) *bad_index = i;
            return -2;
        }
    }
    return 0;
}

/* 参考实现: 每次访问都重新做边界钳制, 逐像素重算 3x3 邻域 */
static double ref_lum(const unsigned char *src, int w, int h, int x, int y) {
    if (x < 0) x = 0;
    if (x >= w) x = w - 1;
    if (y < 0) y = 0;
    if (y >= h) y = h - 1;
    const unsigned char *px = src + ((size_t)y * w + x) * 3;
    return 0.299 * px[0] + 0.587 * px[1] + 0.114 * px[2];
}

static int ref_px(const unsigned char *src, int w, int h, int x, int y, int ch) {
    if (x < 0) x = 0;
    if (x >= w) x = w - 1;
    if (y < 0) y = 0;
    if (y >= h) y = h - 1;
    return src[((size_t)y * w + x) * 3 + ch];
}

int mip_sharpen_ref(const unsigned char *src, int w, int h,
                    const MipSharpenParams *p, unsigned char *dst, MipSharpenStats *stats) {
    if (!src || !dst || w <= 0 || h <= 0) return -1;
    if (!params_ok(p)) return -3;
    MipSharpenStats s = { 0.0, 0.0, 0.0, 0, 0 };
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            double gx = ref_lum(src, w, h, x + 1, y) - ref_lum(src, w, h, x - 1, y);
            double gy = ref_lum(src, w, h, x, y + 1) - ref_lum(src, w, h, x, y - 1);
            double g = hypot(gx, gy);
            double t = (g - p->grad_lo) / (p->grad_hi - p->grad_lo);
            if (t < 0.0) t = 0.0;
            if (t > 1.0) t = 1.0;
            float a = (float)(p->strength * t);
            if (a > 0.0) ++s.edge_pixels;
            s.strength_mean += a;
            if (a > s.strength_max) s.strength_max = a;
            int ring = 0;
            for (int ch = 0; ch < 3; ++ch) {
                double sum = 0.0, mn = 255.0, mx = 0.0;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        double v = ref_px(src, w, h, x + dx, y + dy, ch);
                        sum += v;
                        if (v < mn) mn = v;
                        if (v > mx) mx = v;
                    }
                double sv = ref_px(src, w, h, x, y, ch);
                double raw = sv + a * (sv - sum / 9.0);
                double ex = raw > mx ? raw - mx : (raw < mn ? mn - raw : 0.0);
                s.overshoot += ex;
                if (ex > 1e-9) ring = 1;
                double v = raw < mn ? mn : (raw > mx ? mx : raw);
                if (v < 0.0) v = 0.0;
                if (v > 255.0) v = 255.0;
                dst[((size_t)y * w + x) * 3 + ch] = (unsigned char)(v + 0.5);
            }
            if (ring) ++s.ringing_pixels;
        }
    }
    s.strength_mean /= (double)((size_t)w * (size_t)h);
    if (stats) *stats = s;
    return 0;
}
