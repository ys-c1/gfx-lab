#include "mip.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int mip_build(MipChain *c, const unsigned char *src, int w, int h) {
    if (!c || !src || w <= 0 || h <= 0) return -1;
    int levels = 1, ww = w, hh = h;
    while (ww > 1 || hh > 1) { ww = ww > 1 ? ww / 2 : 1; hh = hh > 1 ? hh / 2 : 1; ++levels; }
    c->levels = levels;
    c->w0 = w; c->h0 = h;
    c->pix = (unsigned char**)calloc((size_t)levels, sizeof(unsigned char*));
    c->w = (int*)calloc((size_t)levels, sizeof(int));
    c->h = (int*)calloc((size_t)levels, sizeof(int));
    if (!c->pix || !c->w || !c->h) { mip_free(c); return -1; }
    c->w[0] = w; c->h[0] = h;
    c->pix[0] = (unsigned char*)malloc((size_t)w * (size_t)h * 3);
    if (!c->pix[0]) { mip_free(c); return -1; }
    for (int i = 0; i < w * h * 3; ++i) c->pix[0][i] = src[i];
    for (int l = 1; l < levels; ++l) {
        int pw = c->w[l-1], ph = c->h[l-1];
        int nw = pw > 1 ? pw / 2 : 1, nh = ph > 1 ? ph / 2 : 1;
        c->w[l] = nw; c->h[l] = nh;
        c->pix[l] = (unsigned char*)malloc((size_t)nw * (size_t)nh * 3);
        if (!c->pix[l]) { mip_free(c); return -1; }
        for (int y = 0; y < nh; ++y)
            for (int x = 0; x < nw; ++x)
                for (int ch = 0; ch < 3; ++ch) {
                    int x0 = x*2 < pw ? x*2 : pw-1, x1 = (x*2+1) < pw ? x*2+1 : pw-1;
                    int y0 = y*2 < ph ? y*2 : ph-1, y1 = (y*2+1) < ph ? y*2+1 : ph-1;
                    int s = c->pix[l-1][(y0*pw+x0)*3+ch] + c->pix[l-1][(y0*pw+x1)*3+ch]
                          + c->pix[l-1][(y1*pw+x0)*3+ch] + c->pix[l-1][(y1*pw+x1)*3+ch];
                    c->pix[l][(y*nw+x)*3+ch] = (unsigned char)(s / 4);
                }
    }
    return 0;
}

void mip_free(MipChain *c) {
    if (!c) return;
    for (int l = 0; l < c->levels; ++l) free(c->pix[l]);
    free(c->pix); free(c->w); free(c->h);
    c->pix = NULL; c->w = NULL; c->h = NULL; c->levels = 0;
}

int mip_sample(const MipChain *c, int level, double u, double v, unsigned char out[3]) {
    if (!c || !out || level < 0 || level >= c->levels) return -1;
    int x = (int)(u * c->w[level]);
    int y = (int)(v * c->h[level]);
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= c->w[level]) x = c->w[level] - 1;
    if (y >= c->h[level]) y = c->h[level] - 1;
    for (int ch = 0; ch < 3; ++ch) out[ch] = c->pix[level][(y*c->w[level]+x)*3+ch];
    return 0;
}

/* ---- 分两段:按层采样缩略图 -> 按层拼成预览图 ---- */

const char *mip_err_field(int rc) {
    switch (rc) {
    case MIP_E_ARG:   return "arg";
    case MIP_E_W:     return "width";
    case MIP_E_COUNT: return "samples";
    case MIP_E_LEVEL: return "level";
    case MIP_E_ORDER: return "order";
    case MIP_E_SHEET: return "sheet";
    case MIP_E_THUMB: return "thumb";
    default:          return NULL;
    }
}

static void err_set(MipError *err, int code, int index) {
    if (!err) return;
    err->code = code;
    err->field = mip_err_field(code);
    err->index = index;
}

int mip_thumb_level(const MipChain *c, const MipThumbSpec *spec,
                    MipThumb *out, MipError *err) {
    if (!c || !spec || !out) { err_set(err, MIP_E_ARG, -1); return MIP_E_ARG; }
    if (!c->pix || c->levels <= 0) { err_set(err, MIP_E_THUMB, -1); return MIP_E_THUMB; }
    if (spec->level < 0 || spec->level >= c->levels) {
        err_set(err, MIP_E_LEVEL, spec->level); return MIP_E_LEVEL;
    }
    int lvl = spec->level;
    if (spec->tw <= 0 || spec->th <= 0) { err_set(err, MIP_E_W, -1); return MIP_E_W; }
    if (spec->samples <= 0 || spec->samples > 256) {
        err_set(err, MIP_E_COUNT, -1); return MIP_E_COUNT;
    }
    if ((size_t)spec->tw > (size_t)-1 / 3 / (size_t)spec->th) {
        err_set(err, MIP_E_W, -1); return MIP_E_W;
    }

    size_t npx = (size_t)spec->tw * (size_t)spec->th;
    unsigned char *px = (unsigned char*)malloc(npx * 3);
    if (!px) { err_set(err, MIP_E_ARG, -1); return MIP_E_ARG; }

    /* 采样:缩略图第 x 个像素覆盖层内坐标 [x*lw/tw,(x+1)*lw/tw)。
     * 把它在每轴切成 samples 个等宽格子,每格取中心落在的纹素,再对 samples^2
     * 个样本求算术平均。这是对覆盖区域面积平均的分层点估计:采样越密越接近
     * 精确盒式平均(基准里用分数覆盖权重算出精确值来量误差)。
     * 缩略图尺寸等于层尺寸且 samples == 1 时,每像素恰好取本纹素,整层直取。 */
    const unsigned char *src = c->pix[lvl];
    int lw = c->w[lvl], lh = c->h[lvl];
    int ns = spec->samples;
    unsigned long long reads = 0;
    for (int y = 0; y < spec->th; ++y)
        for (int x = 0; x < spec->tw; ++x) {
            unsigned long long acc[3] = { 0, 0, 0 };
            for (int sy2 = 0; sy2 < ns; ++sy2)
                for (int sx2 = 0; sx2 < ns; ++sx2) {
                    /* 格子中心:([x,x+1) 内的第 sx2 格) 再映射到层内 */
                    double fx = ((double)x + ((double)sx2 + 0.5) / (double)ns);
                    double fy = ((double)y + ((double)sy2 + 0.5) / (double)ns);
                    int sx = (int)(fx * (double)lw / (double)spec->tw);
                    int sy = (int)(fy * (double)lh / (double)spec->th);
                    if (sx < 0) sx = 0;
                    if (sx >= lw) sx = lw - 1;
                    if (sy < 0) sy = 0;
                    if (sy >= lh) sy = lh - 1;
                    for (int ch = 0; ch < 3; ++ch)
                        acc[ch] += src[(sy * lw + sx) * 3 + ch];
                    ++reads;
                }
            for (int ch = 0; ch < 3; ++ch)
                px[((size_t)y * spec->tw + x) * 3 + ch] =
                    (unsigned char)(acc[ch] / (unsigned long long)(ns * ns));
        }

    out->level = lvl;
    out->w = spec->tw;
    out->h = spec->th;
    out->samples = ns;
    out->sample_reads = reads;
    out->chain = c;
    out->pixels = px;
    err_set(err, MIP_OK, lvl);
    return MIP_OK;
}

int mip_sheet_assemble(const MipChain *c, const MipThumb *thumbs, int n,
                       const MipSheetSpec *spec,
                       MipSheet *out, MipError *err) {
    if (!c || !thumbs || !out) { err_set(err, MIP_E_ARG, -1); return MIP_E_ARG; }
    if (n <= 0) { err_set(err, MIP_E_ARG, -1); return MIP_E_ARG; }
    int pad = spec ? spec->pad : 0;
    int label_gap = spec ? spec->label_gap : 0;
    if (pad < 0) { err_set(err, MIP_E_SHEET, -1); return MIP_E_SHEET; }
    if (label_gap < 0) { err_set(err, MIP_E_SHEET, -1); return MIP_E_SHEET; }

    /* 缩略图校验:先看是否可用(非空、尺寸合法、来自本条链),再要求按层号
     * 严格升序 —— 同时杜绝重复、乱序与跨链混拼。err->index 填第一个出错的
     * 数组位置,便于定位。 */
    for (int i = 0; i < n; ++i) {
        if (!thumbs[i].pixels || thumbs[i].w <= 0 || thumbs[i].h <= 0 ||
            thumbs[i].chain != c) {
            err_set(err, MIP_E_THUMB, i); return MIP_E_THUMB;
        }
    }
    for (int i = 0; i < n; ++i) {
        if (thumbs[i].level < 0 || thumbs[i].level >= c->levels) {
            err_set(err, MIP_E_LEVEL, thumbs[i].level); return MIP_E_LEVEL;
        }
        if (i > 0 && thumbs[i].level <= thumbs[i-1].level) {
            err_set(err, MIP_E_ORDER, i); return MIP_E_ORDER;
        }
    }

    /* 网格:cols = ceil(sqrt(n)),行数随之。格宽/格高取各缩略图最大值。 */
    int cols = 1;
    while (cols * cols < n) ++cols;
    int rows = (n + cols - 1) / cols;
    int cell_w = 0, cell_h = 0;
    for (int i = 0; i < n; ++i) {
        if (thumbs[i].w > cell_w) cell_w = thumbs[i].w;
        if (thumbs[i].h > cell_h) cell_h = thumbs[i].h;
    }
    long long sheet_w = (long long)cols * cell_w + (long long)(cols - 1) * pad;
    long long sheet_h = (long long)rows * (cell_h + (long long)label_gap)
                      + (long long)(rows - 1) * pad;
    if (sheet_w <= 0 || sheet_h <= 0 ||
        sheet_w > 0x7fffffffLL || sheet_h > 0x7fffffffLL ||
        (size_t)sheet_w > (size_t)-1 / 3 / (size_t)sheet_h) {
        err_set(err, MIP_E_SHEET, -1); return MIP_E_SHEET;
    }

    size_t sheet_px = (size_t)sheet_w * (size_t)sheet_h;
    unsigned char *px = (unsigned char*)calloc(sheet_px * 3, 1);
    int *xs = (int*)calloc((size_t)n, sizeof(int));
    int *ys = (int*)calloc((size_t)n, sizeof(int));
    int *tws = (int*)calloc((size_t)n, sizeof(int));
    int *ths = (int*)calloc((size_t)n, sizeof(int));
    if (!px || !xs || !ys || !tws || !ths) {
        free(px); free(xs); free(ys); free(tws); free(ths);
        err_set(err, MIP_E_ARG, -1); return MIP_E_ARG;
    }

    /* 逐层贴图;每层在格内水平居中、垂直靠下(顶部留给层号标签) */
    for (int i = 0; i < n; ++i) {
        int col = i % cols, row = i / cols;
        int gx = col * (cell_w + pad) + (cell_w - thumbs[i].w) / 2;
        int gy = row * (cell_h + label_gap + pad) + (cell_h - thumbs[i].h)
               + label_gap;
        xs[i] = gx; ys[i] = gy; tws[i] = thumbs[i].w; ths[i] = thumbs[i].h;
        for (int y = 0; y < thumbs[i].h; ++y)
            for (int x = 0; x < thumbs[i].w; ++x)
                for (int ch = 0; ch < 3; ++ch)
                    px[(((size_t)(gy + y) * (size_t)sheet_w + (size_t)(gx + x)) * 3)
                       + (size_t)ch] =
                        thumbs[i].pixels[((size_t)y * (size_t)thumbs[i].w
                                          + (size_t)x) * 3 + (size_t)ch];
    }

    /* 层间差异:相邻两张缩略图按同层号坐标比较公共区域,逐通道绝对差累加。
     * 层间差异单独统计,不混入上面的像素总数。 */
    unsigned long long diff_sum = 0, diff_px = 0;
    for (int i = 1; i < n; ++i) {
        const MipThumb *a = &thumbs[i-1], *b = &thumbs[i];
        int cw = a->w < b->w ? a->w : b->w;
        int chh = a->h < b->h ? a->h : b->h;
        for (int y = 0; y < chh; ++y)
            for (int x = 0; x < cw; ++x)
                for (int ch = 0; ch < 3; ++ch) {
                    int av = a->pixels[((size_t)y * a->w + x) * 3 + ch];
                    int bv = b->pixels[((size_t)y * b->w + x) * 3 + ch];
                    diff_sum += (unsigned long long)(av > bv ? av - bv : bv - av);
                    ++diff_px;
                }
    }

    out->w = (int)sheet_w;
    out->h = (int)sheet_h;
    out->cols = cols;
    out->count = n;
    out->x = xs; out->y = ys; out->tw = tws; out->th = ths;
    out->inter_layer_bytes = diff_sum;
    out->inter_layer_px = diff_px;
    out->inter_layer_mean = diff_px ? (double)diff_sum / (double)diff_px : 0.0;
    out->pixels = px;
    err_set(err, MIP_OK, -1);
    return MIP_OK;
}


int mip_write_ppm(const unsigned char *pixels, int w, int h, const char *path) {
    if (!pixels || !path || w <= 0 || h <= 0) return MIP_E_ARG;
    FILE *f = fopen(path, "wb");
    if (!f) return MIP_E_ARG;
    int ok = fprintf(f, "P6\n%d %d\n255\n", w, h) > 0
          && fwrite(pixels, 1, (size_t)w * (size_t)h * 3, f)
             == (size_t)w * (size_t)h * 3;
    if (fclose(f) != 0) ok = 0;
    return ok ? MIP_OK : MIP_E_ARG;
}
