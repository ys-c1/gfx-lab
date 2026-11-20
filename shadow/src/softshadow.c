#include "softshadow.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *sb_error_msg(int code) {
    switch (code) {
    case SB_OK:              return "ok";
    case SB_ERR_ARG:         return "invalid argument: null pointer or size mismatch";
    case SB_ERR_SIGMA:       return "invalid field 'sigma_scale': sigma must be a finite positive number";
    case SB_ERR_RADIUS_ZERO: return "invalid field 'blur_radius_px': blur radius must be > 0 when soft edge is enabled";
    case SB_ERR_KERNEL_EVEN: return "invalid field 'kernel_len': kernel length (2*blur_radius_px+1) must be odd, got even";
    case SB_ERR_BORDER:      return "invalid field 'border_mode': must be SB_BORDER_CLAMP(0) or SB_BORDER_ZERO(1)";
    case SB_ERR_RADIUS_MIN:  return "invalid field 'blur_radius_min': require 0 <= blur_radius_min <= blur_radius_px";
    case SB_ERR_MEM:         return "out of memory";
    default:                 return "unknown error";
    }
}

/* ---------------- 模糊核（可导出） ---------------- */

int sb_kernel(int r, float sigma_scale, float *out) {
    if (r < 0 || !out || !isfinite(sigma_scale) || sigma_scale <= 0.0f)
        return SB_ERR_SIGMA;
    int len = 2 * r + 1;                 /* 恒为奇数 */
    float sigma = sigma_scale * (r > 0 ? (float)r : 1.0f);
    float sum = 0.0f;
    for (int k = 0; k < len; ++k) {
        float x = (float)(k - r);
        float w = expf(-(x * x) / (2.0f * sigma * sigma));
        out[k] = w;
        sum += w;
    }
    for (int k = 0; k < len; ++k) out[k] /= sum;   /* 归一化，保证 DC 增益=1 */
    return len;
}

int sb_kernel_export(const char *path, int r, float sigma_scale, int border_mode) {
    if (!path) return SB_ERR_ARG;
    float tmp[512];
    int len = sb_kernel(r, sigma_scale, tmp);
    if (len < 0) return len;
    if (len > 512) return SB_ERR_ARG;
    FILE *f = fopen(path, "w");
    if (!f) return SB_ERR_ARG;
    fprintf(f, "# soft-shadow separable gaussian kernel\n");
    fprintf(f, "# radius_px=%d kernel_len=%d (odd) sigma=%.6f sigma_scale=%.4f\n",
            r, len, sigma_scale * (float)r, sigma_scale);
    fprintf(f, "# border_mode=%s (clamp: edge pixel repeated; zero: outside treated as 0)\n",
            border_mode == SB_BORDER_ZERO ? "zero" : "clamp");
    fprintf(f, "# index offset weight\n");
    for (int k = 0; k < len; ++k)
        fprintf(f, "%d %d %.8f\n", k, k - r, tmp[k]);
    fclose(f);
    return len;
}

/* ---------------- 配置校验（半径为零 / 核长为偶，均指出字段） ---------------- */

static int sb_validate(const SbConfig *cfg) {
    if (!cfg) return SB_ERR_ARG;
    if (!isfinite(cfg->sigma_scale) || cfg->sigma_scale <= 0.0f) return SB_ERR_SIGMA;
    if (cfg->border_mode != SB_BORDER_CLAMP && cfg->border_mode != SB_BORDER_ZERO)
        return SB_ERR_BORDER;
    if (cfg->enable) {
        if (cfg->blur_radius_px == 0)
            return SB_ERR_RADIUS_ZERO;                 /* 字段 blur_radius_px */
        int kernel_len = 2 * cfg->blur_radius_px + 1; /* 字段 kernel_len */
        if ((kernel_len & 1) == 0)
            return SB_ERR_KERNEL_EVEN;
        if (cfg->blur_radius_min < 0 || cfg->blur_radius_min > cfg->blur_radius_px)
            return SB_ERR_RADIUS_MIN;                  /* 字段 blur_radius_min */
    }
    return SB_OK;
}

int sb_validate_config(const SbConfig *cfg) { return sb_validate(cfg); }

int sb_check_kernel_len(int radius_px, int kernel_len) {
    if (radius_px == 0) return SB_ERR_RADIUS_ZERO;
    if (kernel_len <= 0 || (kernel_len & 1) == 0) return SB_ERR_KERNEL_EVEN;
    if (kernel_len != 2 * radius_px + 1) return SB_ERR_KERNEL_EVEN;
    return SB_OK;
}

/* ---------------- A 阶段：距离场 -> 每像素模糊半径 ---------------- */

static float point_seg_dist(float px, float py, const ShSeg *s) {
    float vx = s->x1 - s->x0, vy = s->y1 - s->y0;
    float wx = px - s->x0, wy = py - s->y0;
    float len2 = vx * vx + vy * vy;
    float t = len2 > 0.0f ? (wx * vx + wy * vy) / len2 : 0.0f;
    if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
    float cx = s->x0 + t * vx, cy = s->y0 + t * vy;
    float dx = px - cx, dy = py - cy;
    return sqrtf(dx * dx + dy * dy);
}

int sb_distance_field(const ShadowMap *m, const ShSeg *segs, int nseg,
                      const SbConfig *cfg, float *dist, float *radius, int *out_rmax) {
    if (!m || !cfg || !dist || !radius) return SB_ERR_ARG;
    if (nseg > 0 && !segs) return SB_ERR_ARG;
    int vc = sb_validate(cfg);
    if (vc != SB_OK) return vc;
    int rmax = 0;
    for (int y = 0; y < m->h; ++y) {
        for (int x = 0; x < m->w; ++x) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            float d = 1e30f;
            for (int i = 0; i < nseg; ++i) {
                float di = point_seg_dist(px, py, &segs[i]);
                if (di < d) d = di;
            }
            if (nseg == 0) d = 0.0f;
            float r = 0.0f;
            if (cfg->enable) {
                /* 边界处即有基础软度 blur_radius_min，随距离线性增长并封顶到 blur_radius_px */
                float g = (d - cfg->dist_begin) * cfg->dist_scale;
                if (g < 0.0f) g = 0.0f;
                r = (float)cfg->blur_radius_min + g;
                if (r > (float)cfg->blur_radius_px) r = (float)cfg->blur_radius_px;
            }
            size_t idx = (size_t)y * m->w + x;
            dist[idx] = d;
            radius[idx] = r;
            int ri = (int)(r + 0.5f);   /* 离散到整数 tap 半径 */
            if (ri > rmax) rmax = ri;
        }
    }
    if (out_rmax) *out_rmax = rmax;
    return SB_OK;
}

/* ---------------- B 阶段：可分离高斯模糊 ---------------- */

static inline float sample_clamped(const ShadowMap *s, int x, int y) {
    if (x < 0) x = 0; else if (x >= s->w) x = s->w - 1;
    if (y < 0) y = 0; else if (y >= s->h) y = s->h - 1;
    return s->buf[(size_t)y * s->w + x];
}

static inline float sample_zero(const ShadowMap *s, int x, int y) {
    if (x < 0 || y < 0 || x >= s->w || y >= s->h) return 0.0f;
    return s->buf[(size_t)y * s->w + x];
}

static inline float sample_border(const ShadowMap *s, int x, int y, int mode) {
    return mode == SB_BORDER_ZERO ? sample_zero(s, x, y) : sample_clamped(s, x, y);
}

int sb_blur_separable(const ShadowMap *src, ShadowMap *dst,
                      const float *radius, int rmax, const float *kernel,
                      SbConfig *cfg) {
    if (!src || !dst || !radius || !kernel || !cfg) return SB_ERR_ARG;
    if (src->w != dst->w || src->h != dst->h) return SB_ERR_ARG;
    int vc = sb_validate(cfg);
    if (vc != SB_OK) return vc;
    int w = src->w, h = src->h;
    float *tmp = (float*)malloc((size_t)w * (size_t)h * sizeof(float));
    if (!tmp) return SB_ERR_MEM;
    long taps = 0;
    /* 水平 pass：逐像素用各自的整数半径截取同一条核（核按 rmax 生成，取中心段） */
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            size_t idx = (size_t)y * w + x;
            int r = (int)(radius[idx] + 0.5f);
            if (r == 0) { tmp[idx] = src->buf[idx]; taps += 1; continue; }
            int off = rmax - r;                 /* 大核中心段的起始下标 */
            float acc = 0.0f, wsum = 0.0f;
            for (int k = -r; k <= r; ++k) {
                float wt = kernel[off + (k + r)];
                acc += wt * sample_border(src, x + k, y, cfg->border_mode);
                wsum += wt;
                ++taps;
            }
            tmp[idx] = acc / wsum;              /* 截取段重新归一，保证越界钳位时能量守恒 */
        }
    }
    /* 垂直 pass：半径图不变，作用于 tmp */
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            size_t idx = (size_t)y * w + x;
            int r = (int)(radius[idx] + 0.5f);
            if (r == 0) { dst->buf[idx] = tmp[idx]; taps += 1; continue; }
            int off = rmax - r;
            float acc = 0.0f, wsum = 0.0f;
            for (int k = -r; k <= r; ++k) {
                float wt = kernel[off + (k + r)];
                int yy = y + k;
                float v;
                if (cfg->border_mode == SB_BORDER_ZERO) {
                    v = (yy < 0 || yy >= h) ? 0.0f : tmp[(size_t)yy * w + x];
                } else {
                    if (yy < 0) yy = 0; else if (yy >= h) yy = h - 1;
                    v = tmp[(size_t)yy * w + x];
                }
                acc += wt * v;
                wsum += wt;
                ++taps;
            }
            dst->buf[idx] = acc / wsum;
        }
    }
    cfg->sample_count += taps;
    free(tmp);
    return SB_OK;
}

/* ---------------- C 阶段：合成 ---------------- */

int sb_composite(const ShadowMap *hard, const ShadowMap *soft,
                 const SbConfig *cfg, ShadowMap *dst) {
    if (!hard || !soft || !cfg || !dst) return SB_ERR_ARG;
    if (hard->w != dst->w || hard->h != dst->h || soft->w != dst->w || soft->h != dst->h)
        return SB_ERR_ARG;
    const ShadowMap *src = cfg->enable ? soft : hard;   /* 关闭时画面照旧 */
    memcpy(dst->buf, src->buf, (size_t)dst->w * (size_t)dst->h * sizeof(float));
    return SB_OK;
}

/* ---------------- 软边宽度（10%~90% 过渡带） ---------------- */

static float interp_cross(float y0, float v0, float y1, float v1, float level) {
    if (v1 == v0) return y0;
    float t = (level - v0) / (v1 - v0);
    return y0 + t * (y1 - y0);
}

int sb_measure_soft_edge(const ShadowMap *hard, const ShadowMap *soft,
                         int col, SbSoftEdge *out) {
    if (!hard || !soft || !out || col < 0 || col >= hard->w) return SB_ERR_ARG;
    if (hard->w != soft->w || hard->h != soft->h) return SB_ERR_ARG;
    int h = hard->h;
    /* 1) 硬阴影边界：硬图在该列 0/非0 的跳变行，并记录方向（上方是否为亮侧） */
    int edge = -1, top_lit = 0;
    for (int y = 1; y < h; ++y) {
        float a = hard->buf[(size_t)(y - 1) * hard->w + col];
        float b = hard->buf[(size_t)y * hard->w + col];
        if ((a == 0.0f) != (b == 0.0f)) { edge = y; top_lit = (a > 0.0f); break; }
    }
    if (edge < 0) return SB_ERR_ARG;
    out->boundary_y = edge;
    /* 2) 取边界两侧稳定平台值（距边界 4 行，避开过渡带） */
    int ya = edge - 4; if (ya < 0) ya = 0;
    int yb = edge + 4; if (yb >= h) yb = h - 1;
    float v_top = soft->buf[(size_t)ya * soft->w + col];
    float v_bot = soft->buf[(size_t)yb * soft->w + col];
    float v_shadow = top_lit ? v_bot : v_top;
    float v_lit    = top_lit ? v_top : v_bot;
    if (v_lit <= v_shadow) return SB_ERR_ARG;
    float lo = v_shadow + 0.1f * (v_lit - v_shadow);
    float hi = v_shadow + 0.9f * (v_lit - v_shadow);
    /* 3) 沿亮->暗方向求 10% 与 90% 交叉点（亚像素线性插值），宽度与方向无关 */
    int dir = top_lit ? 1 : -1;           /* 扫描方向上的行增量 */
    int ys = top_lit ? ya : yb;           /* 亮侧起点 */
    int ye = top_lit ? yb : ya;           /* 暗侧终点 */
    out->lo_y = -1.0f; out->hi_y = -1.0f;
    for (int y = ys; y != ye; y += dir) {
        int yn = y + dir;
        if (yn < 0 || yn >= h) break;
        float v0 = soft->buf[(size_t)y * soft->w + col];
        float v1 = soft->buf[(size_t)yn * soft->w + col];
        /* 交叉判定对增减方向都成立：level 落在相邻两值之间且首次跨越 */
        if (out->lo_y < 0.0f && (v0 - lo) * (v1 - lo) <= 0.0f && v0 != v1)
            out->lo_y = interp_cross((float)y, v0, (float)yn, v1, lo);
        if (out->hi_y < 0.0f && (v0 - hi) * (v1 - hi) <= 0.0f && v0 != v1)
            out->hi_y = interp_cross((float)y, v0, (float)yn, v1, hi);
    }
    if (out->lo_y < 0.0f || out->hi_y < 0.0f) return SB_ERR_ARG;
    out->width_px = fabsf(out->hi_y - out->lo_y);
    return SB_OK;
}

int sb_write_pgm_f(const char *path, int w, int h, const float *buf) {
    if (!path || !buf || w <= 0 || h <= 0) return SB_ERR_ARG;
    FILE *f = fopen(path, "wb");
    if (!f) return SB_ERR_ARG;
    fprintf(f, "P5\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; ++i) {
        float v = buf[i];
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        unsigned char b = (unsigned char)(v * 255.0f + 0.5f);
        fwrite(&b, 1, 1, f);
    }
    fclose(f);
    return SB_OK;
}
